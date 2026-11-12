// Native hooks inside recompiled game functions (arm64_recomp.py --hook):
// each runs before the original, which it calls as f_<addr>_orig.
//
//   ope_pl_status.Damage(int)            damage the player's character takes
//   ope_en_status.Damage(int, int, int)  damage an enemy takes
//   uiUtility.GetKeyboardInputText(KEYBOARD_TRIGGER)  key names in the options
//   SPFXCameraSimple/SPFXCameraCommandBuffer.UpdateVRev()  effects' vertical reverse
//   gsVideoPlayer.ErrorProc(VideoPlayer, string)  logs videos Windows can't decode
//   ope_og.Director.Loader / outMuseumModel.Enemy.Director .Finalize()  keep models' bundles
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <cstdint>

#include "il2cpp-api.h"
#include "recomp_ctx.h"

namespace recomp
{
    void f_23b08bc_orig(Ctx& c);
    void f_1e08d38_orig(Ctx& c);
    void f_1bec6fc_orig(Ctx& c);
    void f_2f03f64_orig(Ctx& c);
    void f_2f01c78_orig(Ctx& c);
    void f_24fe5e8_orig(Ctx& c);
    bool MouseButtonsEnabled();  // mouse_keys.cpp

    // Set from the debug menu
    std::atomic<bool> g_invincible{ false };
    std::atomic<int> g_damageDealtFactor{ 1 };

    void HookPlayerDamage(Ctx& c)
    {
        if (g_invincible)
            c.x[1] = 0;
        f_23b08bc_orig(c);
    }

    void HookEnemyDamage(Ctx& c)
    {
        int factor = g_damageDealtFactor;
        if (factor > 1)
        {
            int64_t damage = int64_t(int32_t(uint32_t(c.x[1]))) * factor;
            if (damage > 999999999)
                damage = 999999999;
            c.x[1] = uint32_t(int32_t(damage));
        }
        f_1e08d38_orig(c);
    }

    // The game's key list has no mouse buttons; they stand in for keys a PC
    // keyboard lacks (mouse_keys.cpp), and get their own names here.
    void HookKeyboardInputText(Ctx& c)
    {
        const char* name = nullptr;
        if (MouseButtonsEnabled())
        {
            switch (int32_t(uint32_t(c.x[0])))  // gsJoystick.KEYBOARD_TRIGGER
            {
            case 83: name = "Mouse L"; break;  // LeftCommand
            case 82: name = "Mouse R"; break;  // RightCommand
            case 49: name = "Mouse M"; break;  // F15
            case 47: name = "Mouse 4"; break;  // F13
            case 48: name = "Mouse 5"; break;  // F14
            }
        }
        if (!name)
            return f_1bec6fc_orig(c);
        c.x[0] = reinterpret_cast<uint64_t>(il2cpp_string_new(name));
    }

    // The effect cameras flip their projection per graphics API (VRevDirectX,
    // VRevMetal, ... chosen from SystemInfo.graphicsDeviceType). The Direct3D
    // values were made for the game's own Windows rendering setup; here the
    // macOS setup runs, which needs the Metal ones: with the Direct3D values
    // the UI cameras' effects (results, rewards, skill tree) came out upside
    // down. VRevDirectX 0x41, VRevMetal 0x43 in both components.
    static void UseMetalReverse(Ctx& c)
    {
        if (auto* self = reinterpret_cast<uint8_t*>(c.x[0]))
            self[0x41] = self[0x43];
    }

    void HookSimpleCameraVRev(Ctx& c)
    {
        uint64_t self = c.x[0];
        f_2f03f64_orig(c);
        c.x[0] = self;
        UseMetalReverse(c);
    }

    void HookCommandBufferCameraVRev(Ctx& c)
    {
        uint64_t self = c.x[0];
        f_2f01c78_orig(c);
        c.x[0] = self;
        UseMetalReverse(c);
    }

    // A movie Windows' decoder can't play (hardware decoding broken in the
    // driver, some Wine/Proton builds): logged, then the game's own handling.
    void HookVideoError(Ctx& c)
    {
        const char* text = "?";
        std::string message;
        if (auto* msg = reinterpret_cast<Il2CppString*>(c.x[2]))
        {
            for (int i = 0; i < il2cpp_string_length(msg); i++)
                message += char(il2cpp_string_chars(msg)[i] < 128 ? il2cpp_string_chars(msg)[i] : '?');
            text = message.c_str();
        }
        fprintf(stderr, "GoSRecomp: a video could not be played (%s)\n", text);
        f_24fe5e8_orig(c);
    }

    // The loaders of the hub/roster characters (ope_og.Director.Loader) and of
    // the museum's enemy models unload their asset bundles in their
    // finalizers, i.e. whenever the garbage collector happens to collect them,
    // while the model they built is still on screen using those animation
    // clips. The clips then vanish and the model falls back to its controller's
    // placeholder: the T-pose in the hub and roster, which came and went with
    // the collector's timing (driver, Proton version, a Dispatch mission...).
    // The bundles are left loaded; the game still unloads them on its normal
    // paths, and loading the same bundle again reuses it.
    void HookCharacterLoaderFinalize(Ctx&) {}
    void HookMuseumEnemyDirectorFinalize(Ctx&) {}
}
