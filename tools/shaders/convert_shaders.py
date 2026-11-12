# Converts the Metal-only Shader objects of the game's asset files to D3D11.
#
# For each Shader: every Metal subprogram is translated to HLSL (msl2hlsl),
# all programs are compiled in one batch with d3dcompiler_47 (hlslc.exe), and
# the platform blob is rebuilt with D3D11 subprograms in Unity 2020.3's format
# (see docs/SHADERS.md): code = 38-byte header + DXBC, followed by the bind
# channels, constant buffers and resource bindings, re-laid out for HLSL.
#
# usage: python3 -I convert_shaders.py <hlslc dir> <work dir> <out dir> <asset files...>
import os, re, sys, struct, subprocess, collections
import lz4.block
import UnityPy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import msl2hlsl

PLATFORM_METAL, PLATFORM_D3D11 = 14, 4
METAL_VS, METAL_FS = 23, 24
DX11_VS_SM50, DX11_PS_SM50 = 16, 18
METAL_HEADER = 56  # 0xF00DCAFE container before the MSL text


# ------------------------------------------------------------ blob format

class Reader:
    def __init__(self, b, p=0):
        self.b, self.p = b, p

    def i32(self):
        v = struct.unpack_from("<i", self.b, self.p)[0]
        self.p += 4
        return v

    def align(self):
        self.p = (self.p + 3) & ~3

    def string(self):
        n = self.i32()
        v = self.b[self.p:self.p + n].decode("utf-8", "replace")
        self.p += n
        self.align()
        return v

    def blob(self):
        n = self.i32()
        v = self.b[self.p:self.p + n]
        self.p += n
        self.align()
        return v


class Writer:
    def __init__(self):
        self.parts = []
        self.size = 0

    def raw(self, b):
        self.parts.append(b)
        self.size += len(b)

    def i32(self, v):
        self.raw(struct.pack("<i", v))

    def align(self):
        pad = (-self.size) & 3
        if pad:
            self.raw(bytes(pad))

    def string(self, s):
        b = s.encode("utf-8")
        self.i32(len(b))
        self.raw(b)
        self.align()

    def blob(self, b):
        self.i32(len(b))
        self.raw(b)
        self.align()

    def bytes(self):
        return b"".join(self.parts)


def read_entries(raw):
    n = struct.unpack_from("<i", raw, 0)[0]
    return [struct.unpack_from("<3i", raw, 4 + 12 * i) for i in range(n)]


def read_subprogram(sub):
    r = Reader(sub)
    o = {"version": r.i32(), "type": r.i32(), "stats": [r.i32() for _ in range(4)]}
    o["keywords"] = [r.string() for _ in range(r.i32())]
    o["local_keywords"] = [r.string() for _ in range(r.i32())]
    o["code"] = r.blob()
    o["source_map"] = r.i32()
    o["channels"] = [(r.i32(), r.i32()) for _ in range(r.i32())]
    cbs = []
    for _ in range(r.i32()):
        cb = {"name": r.string(), "size": r.i32(), "params": []}
        for _ in range(r.i32()):
            cb["params"].append({"name": r.string(), "type": r.i32(), "rows": r.i32(), "cols": r.i32(),
                                 "matrix": r.i32(), "array": r.i32(), "offset": r.i32()})
        cb["struct_params"] = r.i32()
        cbs.append(cb)
    o["cbuffers"] = cbs
    binds = []
    for _ in range(r.i32()):
        b = {"name": r.string(), "kind": r.i32(), "index": r.i32(), "extra": r.i32()}
        if b["kind"] == 0:
            b["dim"] = r.i32()
        binds.append(b)
    o["bindings"] = binds
    if r.p != len(sub):
        raise ValueError("unparsed subprogram tail")
    return o


def write_subprogram(o):
    w = Writer()
    w.i32(o["version"])
    w.i32(o["type"])
    for s in o["stats"]:
        w.i32(s)
    w.i32(len(o["keywords"]))
    for k in o["keywords"]:
        w.string(k)
    w.i32(len(o["local_keywords"]))
    for k in o["local_keywords"]:
        w.string(k)
    w.blob(o["code"])
    w.i32(o["source_map"])
    w.i32(len(o["channels"]))
    for s, t in o["channels"]:
        w.i32(s)
        w.i32(t)
    w.i32(len(o["cbuffers"]))
    for cb in o["cbuffers"]:
        w.string(cb["name"])
        w.i32(cb["size"])
        w.i32(len(cb["params"]))
        for p in cb["params"]:
            w.string(p["name"])
            for k in ("type", "rows", "cols", "matrix", "array", "offset"):
                w.i32(p[k])
        w.i32(cb["struct_params"])
    w.i32(len(o["bindings"]))
    for b in o["bindings"]:
        w.string(b["name"])
        w.i32(b["kind"])
        w.i32(b["index"])
        w.i32(b["extra"])
        if b["kind"] == 0:
            w.i32(b["dim"])
    return w.bytes()


def write_entries(subs):
    head = 4 + 12 * len(subs)
    w = Writer()
    w.i32(len(subs))
    off = head
    for s in subs:
        w.i32(off)
        w.i32(len(s))
        w.i32(0)
        off += len(s)
    for s in subs:
        w.raw(s)
    return w.bytes()


# ----------------------------------------------------------- conversion

def msl_of(sub):
    return sub["code"][METAL_HEADER:].decode("utf-8", "replace").rstrip("\0")


# A program for variants the translator cannot handle (instancing buffers):
# draws nothing (the pixel stage discards) rather than failing the whole shader.
STUB_VS = "struct O { float4 p : SV_Position; };\nO main() { O o; o.p = float4(0, 0, 0, 0); return o; }\n"
STUB_PS = "float4 main() : SV_Target0 { discard; return float4(0, 0, 0, 0); }\n"


def member_offsets(info):
    """{cbuffer name: ({param-ish name: offset}, size, register)}"""
    out = {}
    for name, reg, members, size in info["cbuffers"]:
        offs = {}
        for mname, off in members:
            offs[mname] = off
            for prefix in ("hlslcc_mtx4x4", "hlslcc_mtx3x3", "hlslcc_mtx2x2"):
                if mname.startswith(prefix):
                    offs[mname[len(prefix):]] = off
        out[name] = (offs, size, reg)
    return out


def d3d11_subprogram(metal, info, dxbc, stage, stub, counts=None):
    o = dict(metal)
    o["type"] = DX11_VS_SM50 if stage == "vertex" else DX11_PS_SM50
    if stub:
        o["channels"], o["source_map"] = [], 0
        o["cbuffers"] = [c for c in metal["cbuffers"] if c["name"] == ""][:1] or [{"name": "", "size": 0, "params": [], "struct_params": 0}]
        o["bindings"] = []
        o["code"] = bytes([2, 0, 0, 0]) + bytes(34) + dxbc
        return o
    if stage == "vertex":
        comp = {idx: msl2hlsl.vertex_component(sem) for idx, sem in info["inputs"]}
        chans = []
        for src, tgt in metal["channels"]:
            if tgt - 13 not in comp:
                raise msl2hlsl.Unsupported(f"channel target {tgt}")
            chans.append((src, comp[tgt - 13]))
        o["channels"] = chans
    layouts = member_offsets(info)
    cbs = []
    for cb in metal["cbuffers"]:
        if cb["name"] == "":
            cbs.append(cb)
            continue
        if cb["name"] not in layouts:
            raise msl2hlsl.Unsupported(f"constant buffer {cb['name']} not declared")
        offs, size, _ = layouts[cb["name"]]
        params = []
        for p in cb["params"]:
            if p["name"] not in offs:
                raise msl2hlsl.Unsupported(f"parameter {p['name']} not in {cb['name']}")
            params.append(dict(p, offset=offs[p["name"]]))
        cbs.append(dict(cb, size=size, params=params))
    o["cbuffers"] = cbs
    binds = []
    for b in metal["bindings"]:
        if b["kind"] == 1:
            if b["name"] not in layouts:
                raise msl2hlsl.Unsupported(f"constant buffer binding {b['name']}")
            binds.append(dict(b, index=layouts[b["name"]][2]))
        elif b["kind"] == 0:
            binds.append(dict(b))  # t# and s# keep the Metal slots
        else:
            raise msl2hlsl.Unsupported("buffer binding")
    o["bindings"] = binds
    ntex = sum(1 for b in binds if b["kind"] == 0)
    ncb = sum(1 for b in binds if b["kind"] == 1)
    nsamp = len({b["extra"] for b in binds if b["kind"] == 0})
    if counts:  # Unity 2020.3 keeps the parameters in the serialized program
        ntex, ncb, nsamp = max(ntex, counts[0]), max(ncb, counts[1]), max(nsamp, counts[2])
    o["code"] = bytes([2, ntex, ncb, nsamp]) + bytes(34) + dxbc
    return o


def program_parameters(prog, sp):
    """The parameter tables Unity binds for one subprogram: the program's
    common parameters plus the subprogram's own."""
    out = {}
    for src in (prog.m_CommonParameters, sp):
        for field in ("m_TextureParams", "m_ConstantBuffers", "m_ConstantBufferBindings", "m_Samplers"):
            out.setdefault(field, []).extend(getattr(src, field, None) or [])
    return out


def binding_counts(params):
    """(textures, constant buffers, samplers) for the D3D11 code header."""
    samplers = {t.m_SamplerIndex for t in params["m_TextureParams"] if t.m_SamplerIndex is not None and t.m_SamplerIndex >= 0}
    samplers |= {smp.sampler for smp in params["m_Samplers"] if hasattr(smp, "sampler")}
    return len(params["m_TextureParams"]), len(params["m_ConstantBufferBindings"]), len(samplers)


def check_layout(shader_name, names, params, info, log):
    """Unity fills constant buffers at the offsets in its tables; the HLSL
    must declare every parameter at the same place."""
    if not info:
        return
    layouts = member_offsets(info)
    for cb in params["m_ConstantBuffers"]:
        cbname = names.get(cb.m_NameIndex)
        if cbname not in layouts:
            continue
        offs = layouts[cbname][0]
        for prm in list(cb.m_VectorParams) + list(cb.m_MatrixParams):
            pname = names.get(prm.m_NameIndex)
            if pname in offs and offs[pname] != prm.m_Index:
                log[f"{shader_name}: {cbname}.{pname} at {offs[pname]}, Unity writes {prm.m_Index}"] += 1


def pass_groups(parsed):
    """Lists of (vertex blob indices, fragment blob indices) per pass."""
    groups = []
    for ss in parsed.m_SubShaders:
        for p in ss.m_Passes:
            vs = [sp.m_BlobIndex for sp in (p.progVertex.m_SubPrograms or [])]
            fs = [sp.m_BlobIndex for sp in (p.progFragment.m_SubPrograms or [])]
            groups.append((vs, fs))
    return groups


class Job:
    """One subprogram to compile."""
    def __init__(self, key, stage, hlsl, info, stub):
        self.key, self.stage, self.hlsl, self.info, self.stub = key, stage, hlsl, info, stub
        self.dxbc = None


def plan_shader(shader_name, subs, groups, jobs, log):
    """Translates the subprograms of one shader; returns {blob index: Job}."""
    parsed_progs = {}
    for i, s in enumerate(subs):
        if s["type"] not in (METAL_VS, METAL_FS):
            raise msl2hlsl.Unsupported(f"program type {s['type']}")
        try:
            parsed_progs[i] = msl2hlsl.parse(msl_of(s))
        except msl2hlsl.Unsupported as e:
            parsed_progs[i] = e
    out = {}
    for vs, fs in groups:
        members = [parsed_progs[i] for i in vs + fs if not isinstance(parsed_progs[i], Exception)]
        try:
            varyings = msl2hlsl.union_varyings(members)
        except msl2hlsl.Unsupported as e:
            log[f"{shader_name}: {e}"] += 1
            varyings = None
        for i in vs + fs:
            if i in out:
                continue
            p = parsed_progs[i]
            stage = "vertex" if subs[i]["type"] == METAL_VS else "fragment"
            job = None
            if not isinstance(p, Exception) and varyings is not None:
                try:
                    info = msl2hlsl.emit(p, varyings)
                    job = Job((shader_name, i), stage, info["hlsl"], info, False)
                except msl2hlsl.Unsupported as e:
                    log[f"{shader_name}: {e}"] += 1
            else:
                log[f"{shader_name}: {p if isinstance(p, Exception) else 'varyings'}"] += 1
            if job is None:
                job = Job((shader_name, i), stage, STUB_VS if stage == "vertex" else STUB_PS, None, True)
            out[i] = job
            jobs.append(job)
    for i in range(len(subs)):
        if i not in out:  # not referenced by any pass
            stage = "vertex" if subs[i]["type"] == METAL_VS else "fragment"
            job = Job((shader_name, i), stage, STUB_VS if stage == "vertex" else STUB_PS, None, True)
            out[i] = job
            jobs.append(job)
    return out


def compile_jobs(jobs, hlslc_dir, work):
    """Compiles unique HLSL sources once each."""
    os.makedirs(work, exist_ok=True)
    unique = {}
    for j in jobs:
        unique.setdefault((j.stage, j.hlsl), []).append(j)
    lines = []
    native = os.name == "nt"
    to_path = (lambda p: p) if native else (lambda p: "Z:" + p)
    for n, ((stage, hlsl), js) in enumerate(unique.items()):
        src = os.path.join(work, f"p{n:05d}.hlsl")
        with open(src, "w") as f:
            f.write(hlsl)
        lines.append(f"{'vs_5_0' if stage == 'vertex' else 'ps_5_0'} {to_path(src)} {to_path(src[:-5] + '.dxbc')}")
    lst = os.path.join(work, "list.txt")
    with open(lst, "w") as f:
        f.write("\n".join(lines) + "\n")
    exe = os.path.join(hlslc_dir, "hlslc.exe")
    cmd = [exe, lst] if native else ["wine", exe, to_path(lst)]
    env = dict(os.environ, WINEDEBUG="-all", WINEDLLOVERRIDES="d3dcompiler_47=n")
    print(subprocess.run(cmd, env=env, capture_output=True, text=True).stdout.strip())
    failed = 0
    for n, ((stage, hlsl), js) in enumerate(unique.items()):
        out = os.path.join(work, f"p{n:05d}.dxbc")
        if os.path.exists(out):
            dxbc = open(out, "rb").read()
            for j in js:
                j.dxbc = dxbc
        else:
            failed += 1
    return failed


def stub_dxbc(hlslc_dir, work, stage):
    j = Job(None, stage, STUB_VS if stage == "vertex" else STUB_PS, None, True)
    compile_jobs([j], hlslc_dir, os.path.join(work, "stub_" + stage))
    return j.dxbc


def convert_files(paths, hlslc_dir, work, out_dir):
    log = collections.Counter()
    loaded = []
    jobs = []
    for path in paths:
        env = UnityPy.load(path)
        shaders = []
        for obj in env.objects:
            if obj.type.name != "Shader":
                continue
            d = obj.read()
            if list(d.platforms) != [PLATFORM_METAL]:
                continue
            blob = bytes(d.compressedBlob)
            off, cl, dl = d.offsets[0][0], d.compressedLengths[0][0], d.decompressedLengths[0][0]
            if len(d.offsets[0]) != 1:
                log[f"{d.m_ParsedForm.m_Name}: segmented blob"] += 1
                continue
            raw = lz4.block.decompress(blob[off:off + cl], uncompressed_size=dl)
            subs = [read_subprogram(raw[o:o + n]) for o, n, _ in read_entries(raw)]
            plan = plan_shader(d.m_ParsedForm.m_Name, subs, pass_groups(d.m_ParsedForm), jobs, log)
            shaders.append((obj, d, subs, plan))
        loaded.append((path, env, shaders))
        print(f"{path}: {len(shaders)} shaders")
    failed = compile_jobs(jobs, hlslc_dir, work)
    stubs = {"vertex": stub_dxbc(hlslc_dir, work, "vertex"), "fragment": stub_dxbc(hlslc_dir, work, "fragment")}
    print(f"{len(jobs)} subprograms, {failed} unique sources failed to compile")

    for path, env, shaders in loaded:
        if not shaders:
            continue
        for obj, d, subs, plan in shaders:
            params_of, names_of = {}, {}
            for ss in d.m_ParsedForm.m_SubShaders:
                for p in ss.m_Passes:
                    names = {v: k for k, v in (p.m_NameIndices or [])}  # parameter names are per pass
                    for prog in (p.progVertex, p.progFragment):
                        for sp in prog.m_SubPrograms or []:
                            params_of[sp.m_BlobIndex] = program_parameters(prog, sp)
                            names_of[sp.m_BlobIndex] = names
                        # D3D11 constant buffers are sized in 16-byte steps
                        for cb in prog.m_CommonParameters.m_ConstantBuffers or []:
                            cb.m_Size = (cb.m_Size + 15) & ~15
            new = []
            for i, s in enumerate(subs):
                j = plan[i]
                stub = j.stub or j.dxbc is None
                if j.dxbc is None and not j.stub:
                    log[f"{d.m_ParsedForm.m_Name}: compile failure"] += 1
                params = params_of.get(i)
                counts = binding_counts(params) if params and not stub else None
                if params and not stub:
                    check_layout(d.m_ParsedForm.m_Name, names_of[i], params, j.info, log)
                try:
                    o = d3d11_subprogram(s, j.info, stubs[j.stage] if stub else j.dxbc, j.stage, stub, counts)
                except msl2hlsl.Unsupported as e:
                    log[f"{d.m_ParsedForm.m_Name}: {e}"] += 1
                    o = d3d11_subprogram(s, None, stubs[j.stage], j.stage, True)
                new.append(write_subprogram(o))
            raw = write_entries(new)
            comp = lz4.block.compress(raw, store_size=False, mode="high_compression")
            d.platforms = [PLATFORM_D3D11]
            d.offsets = [[0]]
            d.compressedLengths = [[len(comp)]]
            d.decompressedLengths = [[len(raw)]]
            d.compressedBlob = comp
            for ss in d.m_ParsedForm.m_SubShaders:
                for p in ss.m_Passes:
                    for prog, dx in ((p.progVertex, DX11_VS_SM50), (p.progFragment, DX11_PS_SM50)):
                        for sp in prog.m_SubPrograms or []:
                            sp.m_GpuProgramType = dx
                            o = read_subprogram(new[sp.m_BlobIndex])
                            ch = sp.m_Channels
                            for k, (src, tgt) in enumerate(o["channels"]):
                                if k < len(ch.m_Channels):
                                    ch.m_Channels[k].source, ch.m_Channels[k].target = src, tgt
            d.save()
        rel = os.path.basename(path)
        os.makedirs(out_dir, exist_ok=True)
        is_bundle = type(env.file).__name__ == "BundleFile"
        with open(os.path.join(out_dir, rel), "wb") as f:
            f.write(env.file.save(packer="lz4") if is_bundle else env.file.save())
        print(f"wrote {os.path.join(out_dir, rel)}")
    for k, v in log.most_common():
        print(f"  {v:5d}  {k}")


if __name__ == "__main__":
    convert_files(sys.argv[4:], sys.argv[1], sys.argv[2], sys.argv[3])
