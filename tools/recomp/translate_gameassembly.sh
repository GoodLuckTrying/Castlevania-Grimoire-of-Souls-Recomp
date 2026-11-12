#!/usr/bin/env bash
# Translates the game code of the macOS v1.4.2 GameAssembly (arm64 slice) to
# C++: the registration-derived functions and the compiler-outlined helpers
# (see BUILDING.md for how the inputs are produced).
#
# Required environment:
#   GAME_ARM64   arm64 slice of GameAssembly.dylib (lipo -thin arm64)
#   WORK         directory with ft_all.json, ft_gap.json, type_classes.json,
#                lsda.json and Il2CppDumper's script.json
#   RECOMP_OUT   output directory for the main translation
#   GAP_OUT      output directory for the helper translation
set -euo pipefail
ulimit -s unlimited
HERE="$(cd "$(dirname "$0")" && pwd)"

# Addresses in the v1.4.2 arm64 image:
#   0x192c68, 0x1931a0  helpers returning native function pointers (icall and
#                       P/Invoke resolution)
#   0x3abc148           Il2CppExceptionWrapper's typeinfo
#   0x182b004           LocalServer coroutine, replaced by the native offline server
#   0x23b08bc           ope_pl_status.Damage           (debug menu: invincibility)
#   0x1e08d38           ope_en_status.Damage           (debug menu: damage factor)
#   0x1bec6fc           GetKeyboardInputText           (mouse button names)
#   0x200fbe0           gsJoystick.IsTrigger           (controller axes 5-28, Xbox LT/RT)
#   0x200f60c           gsJoystick.NewInputDevices     (d-pad moves, optional)
#   0x1ecc8f0           GetInputSystemKeyDown          (rebinding Xbox LT/RT)
#   0x2f03f64, 0x2f01c78  SPFXCamera*.UpdateVRev         (effects' vertical reverse)
#   0x24fe5e8           gsVideoPlayer.ErrorProc        (logs undecodable videos)
#   0x1bb7d8c, 0x2108848  model loaders' Finalize        (T-pose: bundles unloaded by the GC)
COMMON=(--signatures "$WORK/script.json" --types "$WORK/type_classes.json"
        --native-resolvers 0x192c68,0x1931a0 --lsda "$WORK/lsda.json" --wrapper-typeinfo 0x3abc148)

python3 -I "$HERE/arm64_recomp.py" "$GAME_ARM64" "$WORK/ft_all.json" "$RECOMP_OUT" --split 512 "${COMMON[@]}" \
    --override 0x182b004=LocalServerMoveNext \
    --hook 0x23b08bc=HookPlayerDamage --hook 0x1e08d38=HookEnemyDamage --hook 0x1bec6fc=HookKeyboardInputText \
    --hook 0x200fbe0=HookIsTrigger --hook 0x200f60c=HookJoystickInput --hook 0x1ecc8f0=HookRebindInputSystemKeyDown \
    --hook 0x2f03f64=HookSimpleCameraVRev --hook 0x2f01c78=HookCommandBufferCameraVRev \
    --hook 0x24fe5e8=HookVideoError \
    --hook 0x1bb7d8c=HookCharacterLoaderFinalize --hook 0x2108848=HookMuseumEnemyDirectorFinalize
python3 -I "$HERE/arm64_recomp.py" "$GAME_ARM64" "$WORK/ft_gap.json" "$GAP_OUT" --split 8 --extra-table "${COMMON[@]}"
cp "$HERE/../../recomp/runtime/recomp_simd.h" "$RECOMP_OUT/"
cp "$HERE/../../recomp/runtime/recomp_simd.h" "$GAP_OUT/"
