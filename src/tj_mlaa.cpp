/* The settings menu's "Anti-aliasing" (src/settings_menu.cpp; settings.ini
 * anti_aliasing, TJ_DISABLE_MLAA=0/1 overrides): off skips Tokyo Jungle's
 * per-frame Edge MLAA, the default.
 *
 * func_001536EC (v1.01; func_00153834 in v1.00, where the LasVola port found
 * it) wraps the game's SPU anti-aliasing: an NV3089 copy of the finished frame
 * from local to main memory, the SPU MLAA jobs, and a copy back. For this port
 * the copy out is a GPU flush and a full-frame readback, twice a frame (two
 * colour targets): the frame-clock thread spent most of its blocked time there
 * and the main PPU thread waited on it through 0065's RSX wait. Skipping the
 * wrapper flips the frame without anti-aliasing, as the LasVola port's
 * "-Recommended" preset does.
 *
 * Its one caller (func_00018xxx in another translation unit) reaches it
 * through the linker's --wrap (CMakeLists.txt), so the lifted source is not
 * edited. Returning at entry leaves the guest state as the caller set it; the
 * caller reloads every register it uses afterwards. */
#include "ppu_recomp.h"
#include "settings_menu.h"

#include <chrono>
#include <stdio.h>
#include <stdlib.h>

extern "C" void __real__Z13func_001536ECP11ppu_context(ppu_context* ctx);

extern "C" void __wrap__Z13func_001536ECP11ppu_context(ppu_context* ctx)
{
    /* Read every frame: the menu switches it live. */
    bool skip = tj_settings_anti_aliasing() != TJ_AA_GAME_MLAA;
    /* TJ_MLAA_AFTER_S=<s> (tests): the game's MLAA stays off for the first s
     * seconds, so a scripted run reaches gameplay on its usual timing. */
    { static const char* e = getenv("TJ_MLAA_AFTER_S");
      static const auto t0 = std::chrono::steady_clock::now();
      if (!skip && e && *e && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(atoi(e)))
          skip = true; }
    static int last = -1;
    if ((int)skip != last) {
        fprintf(stderr, "[tj-mlaa] game MLAA %s\n", skip ? "off" : "on");
        last = skip;
    }
    if (skip) return;
    __real__Z13func_001536ECP11ppu_context(ctx);
}
