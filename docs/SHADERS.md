# Shaders

The macOS build ships every shader with Metal subprograms only (`platforms = [14]`). The Windows player needs D3D11 ones (`platforms = [4]`). `tools/shaders/convert_shaders.py` rewrites the Shader objects in place; `tools/shaders/msl2hlsl.py` translates the programs.

## Shader object (Unity 2020.3)

* `compressedBlob`: one LZ4 block per platform (`offsets`, `compressedLengths`, `decompressedLengths` are lists of segments; the game uses a single segment).
* `m_ParsedForm`: subshaders -> passes -> `progVertex` / `progFragment` -> `m_SubPrograms`. Each `SerializedSubProgram` names its subprogram by `m_BlobIndex` and records `m_GpuProgramType` (Metal VS 23, FS 24; D3D11 VS SM5.0 16, PS SM5.0 18) and the bind channels. Its `m_Parameters` is empty: the parameters live in the blob.

## Platform blob

```
int32 count
{ int32 offset, int32 length, int32 segment } * count   // offsets from the blob start
subprograms, back to back
```

## Subprogram (version 201806140)

All strings are `int32 length` + bytes, padded to 4.

```
int32 version, int32 gpuProgramType, int32 stats[4]
int32 n, string keywords[n]
int32 n, string localKeywords[n]
int32 n, byte code[n] (padded)
int32 sourceMap                       // bitmask of the channels below' sources
int32 n, { int32 source, int32 target } channels[n]
int32 n, constantBuffer[n]            // the first is an unnamed, empty "globals" group
    string name, int32 size, int32 m, param[m], int32 structParamCount
    param: string name, int32 type (0 float, 1 int), rows, cols, isMatrix, arraySize, offset
int32 n, binding[n]
    string name, int32 kind, int32 index, int32 extra [, int32 dimension if kind == 0]
    kind 0 texture: index = t#, extra = sampler slot, dimension (4 = 2D, 6 = cube, 8 = 3D?)
    kind 1 constant buffer: index = b#
    kind 2 buffer
```

Code:

* Metal: a `0xF00DCAFE` container; the MSL source starts at offset 56 (entry point `xlatMtlMain`).
* D3D11: a 38-byte header - `2, textureCount, constantBufferCount, samplerCount`, then 34 zero bytes - followed by the DXBC container.

Channel source is Unity's ShaderChannel (0 vertex, 1 normal, 2 tangent, 3 color, 4-11 texcoord0-7). The target is a vertex component: D3D11 uses semantics (0 POSITION, 1 NORMAL, 2 TANGENT, 3 COLOR, 5-12 TEXCOORD0-7), Metal uses generic attributes from 13 (`13 + [[attribute(n)]]`).

These layouts were checked by parsing every subprogram of the Windows `unity default resources` (D3D11, GLES, Metal, GLCore, Vulkan) and of the game (3,935 Metal subprograms) with no bytes left over.

## Translation

The MSL is HLSLcc output and very regular, so the translator keeps each function body and rewrites only:

* intrinsics (`fma`->`mad`, `fract`->`frac`, `rint`->`round`, `dfdx`->`ddx`, `select`, `as_type`, `discard_fragment`), and single-argument vector constructors into casts (Metal broadcasts scalars there, HLSL constructors do not);
* texture methods (`sample` with `level`/`bias`/`gradient2d`, array slices, `sample_compare`, `read`);
* constant buffer access (`VGlobals.x` -> `x` in a `cbuffer` with explicit `packoffset`). The buffers are re-laid out with HLSL packing rules and the parameter offsets in the bindings follow. Float arrays keep Metal's 4-byte stride: they become `float4` arrays indexed as `a[(i) >> 2][(i) & 3]`, and arrays that share a 16-byte register (one ends where the next begins, as in `Custom/DarkWipe`) are merged into one, each indexed from its own offset.

D3D11 links shader stages by register order while Metal links them by name. So for every pass, all vertex and fragment variants share one varying layout: the union of their varyings in semantic order, after `SV_Position`. Metal's in/out structs stay as local structs, and generated wrappers copy into and out of the shared layout.

Textures and samplers keep their Metal slots. Inline samplers (`constexpr sampler`) are not bound by Unity on Metal either; they take an unused register, which D3D11 samples linear/clamp. The few variants using structured buffers (particle instancing, post-processing debug views) get a stub that draws nothing. The game never draws those variants: none of its particle systems uses GPU instancing.

HLSL is compiled with `d3dcompiler_47.dll` (`tools/shaders/hlslc.cpp`), which every Windows 10/11 system has; the conversion could therefore also run on the user's machine.
