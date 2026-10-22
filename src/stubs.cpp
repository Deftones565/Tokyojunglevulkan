/*
 * Tokyo Jungle -- title-specific hooks and overrides.
 *
 * Holds the import forwarders below (tj_register_hle_overrides), and nothing
 * else yet. The prior port's src/stubs.cpp defined trophy,
 * NetCtl, NpCommerce2 and EULA stubs, but its register_overrides() was a TODO,
 * so none of them was ever wired in; every import it ran with was answered by
 * ps3recomp's own HLE through the NID table. Its hle_imports.h and
 * import_resolver.h, which DID route imports, are keyed to v1.00 import-slot
 * addresses (0x342810, ...) and are superseded by the lifter's --hle-stubs
 * path. See docs/PRIOR_PORT.md. Add overrides here only when the v1.01 boot
 * shows ps3recomp answering one wrongly.
 *
 * ---- the template's own description follows ----
 *
 * ps3recomp game project -- game-specific hooks and overrides.
 *
 * main.cpp is the runner and is game-agnostic. This file is where a port puts
 * everything that is not: the PRX loading hook it must answer, overrides for
 * firmware functions the HLE library gets wrong for this title, and patches
 * over recompiled code.
 *
 * It starts out as the empty version of all of that, which is what a game with
 * no lifted PRX and no overrides needs.
 */

#include <cstdio>
#include <cstring>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

/* ---------------------------------------------------------------------------
 * PRX loading
 *
 * main.cpp calls this once, after the lifted function table is registered and
 * vm_base is live, before the game runs. A title that lifts a real system PRX
 * -- libsre, which is cellSpurs and cellSync -- loads its image into guest RAM
 * here and registers its exports. Most ports never need to: the HLE library
 * answers those modules instead.
 * -----------------------------------------------------------------------*/

/* src/font_hle.cpp: cellFont/cellFontFT over stb_truetype. */
extern "C" void tj_font_hle_register(void);
/* src/sail_hle.cpp: cellSail (movies) and its two cellPamf calls. */
extern "C" void tj_sail_hle_register(void);

/* No system PRX runs lifted: every firmware module the game uses is answered
 * natively (fonts, movies) or by ps3recomp's HLE. */
extern "C" void ps3_load_prx_modules(void)
{
}

/* ---------------------------------------------------------------------------
 * Import forwarders
 *
 * NIDs this title imports that ps3recomp registers under a sibling name only.
 * Unregistered, each would fall to the unresolved-NID path, which returns a
 * fabricated CELL_OK and leaves every out-parameter untouched (ppu_hle.cpp).
 * These forward to the implementation that IS registered, through the same
 * ps3_hle_call bridge, so behaviour stays the runtime's. Names come from
 * RPCS3's import table for this EBOOT (tools/oracle).
 *
 * Not here, on purpose: cellFontInitLibraryFreeTypeWithRevision and
 * cellFontOpenFontInstance have no implementation to forward to; they are real
 * HLE and go upstream (patches/ps3recomp). The cellSail *Finalize imports only
 * run during SGX teardown.
 * -----------------------------------------------------------------------*/

#include "ppu_context.h"

extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" void ps3_hle_call(uint32_t nid, ppu_context* ctx);

namespace {

constexpr uint32_t NID_cellSysmoduleLoadModule   = 0x32267A31u;
constexpr uint32_t NID_cellSysmoduleUnloadModule = 0x112A5EE9u;

/* cellSysmoduleLoadModuleEx(u16 id, ...): the trailing arguments select a
 * memory container, which the runtime does not model. The id is already in r3. */
void LoadModuleEx(ppu_context* ctx)   { ps3_hle_call(NID_cellSysmoduleLoadModule, ctx); }
void UnloadModuleEx(ppu_context* ctx) { ps3_hle_call(NID_cellSysmoduleUnloadModule, ctx); }

/* cellSysmodule{Load,Unload}ModuleInternal(u16 id): what firmware modules call
 * to pull in their own dependencies. Same argument as the public calls, so
 * they take the same path. */
void LoadModuleInternal(ppu_context* ctx)   { ps3_hle_call(NID_cellSysmoduleLoadModule, ctx); }
void UnloadModuleInternal(ppu_context* ctx) { ps3_hle_call(NID_cellSysmoduleUnloadModule, ctx); }

/* cellHttpInitCookie(pool, poolSize) / cellHttpEndCookie(): cookie storage for
 * HTTP requests this port never makes (no network). Accepting them is the
 * honest answer; there is nothing to set up or tear down. */
void HttpCookieOk(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }

} // namespace

extern "C" void cellSpurs_set_synthetic_job_events(int on);

extern "C" void tj_register_hle_overrides(void)
{
    /* The SOC sound job reports each answer itself (SPU send_event, port 16,
     * one-deep queue). ps3recomp's synthetic job-done events raced it for that
     * slot and handed the audio thread a bogus module index. Real SPURS sends
     * no such events. */
    cellSpurs_set_synthetic_job_events(0);

    ps3_hle_register_ctx(0x1A267F98u, "cellSysmoduleLoadModuleEx", LoadModuleEx);
    ps3_hle_register_ctx(0xFD0E1CACu, "cellSysmoduleUnloadModuleEx", UnloadModuleEx);
    ps3_hle_register_ctx(0xD9B8C0EEu, "cellSysmoduleLoadModuleInternal", LoadModuleInternal);
    ps3_hle_register_ctx(0xB498BF77u, "cellSysmoduleUnloadModuleInternal", UnloadModuleInternal);
    /* The game's text and movies, answered natively (src/font_hle.cpp,
     * src/sail_hle.cpp). */
    tj_font_hle_register();
    tj_sail_hle_register();
    ps3_hle_register_ctx(0x9638F766u, "cellHttpInitCookie", HttpCookieOk);
    ps3_hle_register_ctx(0x61B2BADEu, "cellHttpEndCookie", HttpCookieOk);
}

/* ---------------------------------------------------------------------------
 * Example: Override a specific NID
 *
 * The module system resolves function imports by NID.  To replace a default
 * HLE stub with custom logic, register your function at init time.
 * -----------------------------------------------------------------------*/

/*
 * Example: override cellGcmSetFlipMode (NID 0xA53D12AE) to force vsync.
 *
 *   static void my_cellGcmSetFlipMode(u32 mode)
 *   {
 *       printf("[hook] cellGcmSetFlipMode: forcing VSYNC (was %u)\n", mode);
 *       // Force vsync regardless of what the game requested
 *       cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
 *   }
 *
 * Then in your init code:
 *
 *   ps3::modules::override_nid(0xA53D12AE,
 *       reinterpret_cast<void*>(my_cellGcmSetFlipMode));
 */

/* ---------------------------------------------------------------------------
 * Example: Hook into module loading
 *
 * You can intercept cellSysmoduleLoadModule to perform custom actions
 * when a particular module is loaded.
 * -----------------------------------------------------------------------*/

/*
 *   static s32 my_loadmodule_hook(u16 id)
 *   {
 *       printf("[hook] Module 0x%04X loaded\n", id);
 *
 *       if (id == CELL_SYSMODULE_AUDIO) {
 *           // Perform custom audio backend initialization
 *           init_my_audio_backend();
 *       }
 *
 *       // Call the original implementation
 *       return cellSysmoduleLoadModule(id);
 *   }
 */

/* ---------------------------------------------------------------------------
 * Game-specific patches
 *
 * Sometimes you need to patch recompiled code at specific addresses.
 * Use the patch system to modify behavior without editing generated code.
 * -----------------------------------------------------------------------*/

/*
 *   // Skip an integrity check at guest address 0x00812FA0
 *   ps3::patches::nop_range(0x00812FA0, 0x00812FB0);
 *
 *   // Replace a function call with a direct return
 *   ps3::patches::force_return(0x009A1000, 0);  // return 0
 */
