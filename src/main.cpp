/*
 * Tokyo Jungle (NPUA80523, v1.01) -- runner.
 *
 * Started from third_party/ps3recomp/templates/project/main.cpp. What differs:
 *   - the Vulkan backend is used on Linux when the runtime is built with
 *     PS3RECOMP_RSX_VULKAN (this project's CMakeLists turns it on);
 *   - setup_vfs() builds the host directory both filesystem layers read from
 *     and points PS3_HDD0_ROOT, ppu_vfs_root and cellFs at it;
 *   - argv[0] and the window title default to this title's values.
 * Everything else is the template's, comments and all.
 *
 * Usage: tokyojungle <EBOOT.ELF> [<game dir holding USRDIR/>]
 *
 * ---- the template's own description follows ----
 *
 * ps3recomp game project -- entry point template.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: allocate the flat guest VM, load the PPU ELF named on the command
 * line, register the lifted function table and the HLE NID handlers, start a
 * frame clock, and dispatch the entry OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#include "ppu_recomp.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"

#include <chrono>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <limits.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);
void     cellPad_host_init(void);             /* cellPad.c: SDL input up before threads */

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);
void     tj_register_hle_overrides(void);     /* stubs.cpp */

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */
void     cellfs_set_root_path(const char* root);           /* libs/filesystem/cellFs.c */
void     cellfs_add_path_mapping(const char* ps3_prefix, const char* host_path);
void     cellGame_init_from_paramsfo(const char* sfo_path);  /* libs/system/cellGame.c */
void     cellSysutilQueueEvent(int slot, uint32_t status, uint32_t param);  /* cellSysutil.c */

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
extern "C" { extern uint8_t* vm_base; extern uint32_t ppu_hle_inject_base; }
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
int      cellGcm_rsx_waiters(void);           /* guest threads waiting on the drain */
unsigned cellGcm_flip_request_count(void);
}

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#elif defined(PS3RECOMP_RSX_VULKAN)
/* Linux. The runtime exports PS3RECOMP_RSX_VULKAN as a PUBLIC definition when
 * it is built with the Vulkan backend, the same switch host_posix.c keys on.
 * Guest shader programs are opt-in in the runtime; TJ turns them on in main()
 * (PS3RECOMP_VK_GUEST_PROGRAMS=0 to compare against the fallback). */
extern "C" int  rsx_vulkan_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_vulkan_backend_present(void);
extern "C" int  rsx_vulkan_backend_pump_messages(void);
extern "C" void rsx_vulkan_backend_set_overlay(const char* text);
#  define RSX_BACKEND_OVERLAY rsx_vulkan_backend_set_overlay
#  define rsx_backend_init    rsx_vulkan_backend_init
#  define rsx_backend_present rsx_vulkan_backend_present
#  define rsx_backend_pump    rsx_vulkan_backend_pump_messages
#  define RSX_BACKEND_NAME    "Vulkan"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
static volatile LONG g_frames_presented = 0;

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

static void present_guest_frame(void)
{
    rsx_backend_present();
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    InterlockedIncrement(&g_frames_presented);
}

/* ---------------------------------------------------------------------------
 * Frame dumps
 * -----------------------------------------------------------------------*/
/* What the prior port's CELLMARK_DUMP did on D3D12, for Vulkan: presented
 * frames written to numbered files from inside the process. Screenshots of the
 * window are not evidence -- the prior port's showed only the sky layer while
 * the frame held the whole title -- so this is how to see what is drawn.
 *
 *   TJ_FRAME_DUMP=<dir>        turn it on; frames go to <dir>/frame_NNNNNN_flipNNNNNN.ppm
 *   TJ_FRAME_DUMP_EVERY=<n>    keep every nth present (default 30, ~1 s at 30 fps)
 *   TJ_FRAME_DUMP_SKIP=<n>     drop the first n dumps (e.g. to get past boot)
 *   TJ_FRAME_DUMP_MAX=<n>      stop after keeping n (default 100)
 *
 * The Vulkan backend already reads every present back and can write it out:
 * PS3RECOMP_VK_DUMP=<file> rewrites one PPM every PS3RECOMP_VK_DUMP_EVERY
 * presents (libs/video/rsx_vulkan_backend.c, vk_dump_ppm). It opens the file
 * afresh each time, so the runner points it at a staging file and renames each
 * one it finds to a numbered frame. rename() is atomic, so a dump is either
 * taken whole or left for the next pass. Writing a frame costs more than
 * presenting it, which is why EVERY defaults high.
 *
 * The staging file is collected every ~4 ms. Two dumps closer together than
 * that keep only the second (the smoke title, which presents three frames and
 * exits, keeps two with EVERY=1); at EVERY >= 2 and 30 fps it does not arise. */
static int mkdir_p(const char* path);          /* below, with the VFS setup */

static char     s_dump_dir[PATH_MAX];
static char     s_dump_stage[PATH_MAX + 16];
static unsigned s_dump_skip, s_dump_max = 100, s_dump_seen, s_dump_kept;

static void frame_dump_setup(void)
{
    const char* dir = getenv("TJ_FRAME_DUMP");
    if (!dir || !*dir) return;
#if !defined(PS3RECOMP_RSX_VULKAN) || defined(_WIN32) || defined(__APPLE__)
    fprintf(stderr, "[dump] TJ_FRAME_DUMP needs the Vulkan backend; ignored\n");
    return;
#endif
    if (mkdir_p(dir) != 0) {
        fprintf(stderr, "[dump] cannot create %s: %s\n", dir, strerror(errno));
        return;
    }
    snprintf(s_dump_dir, sizeof s_dump_dir, "%s", dir);
    snprintf(s_dump_stage, sizeof s_dump_stage, "%s/.staging.ppm", dir);
    unlink(s_dump_stage);

    const char* e;
    unsigned every = 30;
    if ((e = getenv("TJ_FRAME_DUMP_EVERY")) && atoi(e) > 0) every = (unsigned)atoi(e);
    if ((e = getenv("TJ_FRAME_DUMP_SKIP"))  && atoi(e) > 0) s_dump_skip = (unsigned)atoi(e);
    if ((e = getenv("TJ_FRAME_DUMP_MAX"))   && atoi(e) > 0) s_dump_max  = (unsigned)atoi(e);

    char n[16];
    snprintf(n, sizeof n, "%u", every);
    setenv("PS3RECOMP_VK_DUMP", s_dump_stage, 1);
    setenv("PS3RECOMP_VK_DUMP_EVERY", n, 1);
    printf("[dump] every %u presents -> %s (skip %u, keep %u)\n",
           every, s_dump_dir, s_dump_skip, s_dump_max);
}

/* Called from the frame clock, the thread that presents. */
static void frame_dump_collect(void)
{
    if (!s_dump_dir[0]) return;
    struct stat st;
    if (stat(s_dump_stage, &st) != 0) return;   /* nothing new */

    unsigned i = s_dump_seen++;
    if (i < s_dump_skip || s_dump_kept >= s_dump_max) {
        unlink(s_dump_stage);
        return;
    }
    char out[PATH_MAX + 64];
    snprintf(out, sizeof out, "%s/frame_%06u_flip%06u.ppm",
             s_dump_dir, i, cellGcm_flip_request_count());
    if (rename(s_dump_stage, out) == 0 && ++s_dump_kept == s_dump_max)
        printf("[dump] kept %u frames; no more will be kept\n", s_dump_kept);
}

/* ---------------------------------------------------------------------------
 * Frame-rate readout (user, 2026-10-16)
 * -----------------------------------------------------------------------*/
/* F1 toggles it, TJ_SHOW_FPS=1 starts with it on: "23.4 FPS" in the window's
 * top-right corner, refreshed once a second. It counts GAME frames: flips the
 * guest finished (cellGcm_flip_request_count), the number that matters for how
 * the game plays, not presents. The backend draws it on the window only (not
 * in frame dumps). F1 is caught with SDL_AddEventWatch, which sees every event
 * the backend's pump takes off the queue on this thread, so the backend's own
 * event loop is untouched. Headless, or a backend without an overlay: no-op. */
#if defined(RSX_BACKEND_OVERLAY) && __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
static int s_show_fps = -1;                  /* -1: TJ_SHOW_FPS not read yet */
static int SDLCALL fps_key_watch(void*, SDL_Event* e)
{
    if (e->type == SDL_KEYDOWN && !e->key.repeat && e->key.keysym.sym == SDLK_F1)
        s_show_fps = !s_show_fps;
    return 1;
}
static void fps_overlay_update(void)
{
    static bool watching = false, shown = false;
    static auto t0 = std::chrono::steady_clock::now();
    static unsigned f0 = 0;
    if (s_show_fps < 0) {
        const char* e = getenv("TJ_SHOW_FPS");
        s_show_fps = (e && *e && *e != '0') ? 1 : 0;
    }
    if (!watching) { SDL_AddEventWatch(fps_key_watch, NULL); watching = true; }
    if (!s_show_fps) {
        if (shown) { RSX_BACKEND_OVERLAY(""); shown = false; }
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double s = std::chrono::duration<double>(now - t0).count();
    if (shown && s < 1.0) return;
    const unsigned f = cellGcm_flip_request_count();
    char buf[32];
    if (shown) snprintf(buf, sizeof buf, "%.1f FPS", (f - f0) / s);
    else       snprintf(buf, sizeof buf, "-- FPS");      /* first second */
    RSX_BACKEND_OVERLAY(buf);
    shown = true; t0 = now; f0 = f;
}
#else
static void fps_overlay_update(void) {}
#endif

static DWORD WINAPI frame_clock(LPVOID)
{
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "Tokyo Jungle (ps3recomp)";

    int rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            !rsx_ok ? "FAILED"
            : getenv("PS3RECOMP_VK_WINDOW") ? "OK -- window open" : "OK");

    unsigned  last_flip = 0;
    /* NTSC vblank, 60000/1001 Hz (59.94), on an absolute schedule: tick k is
     * due at t0 + k * 1001/60000 s, so the rate does not drift with Sleep()'s
     * granularity. It was a 16 ms step (62.5 Hz), and every vblank-counted
     * clock in the game ran 4.3% fast. */
    typedef std::chrono::steady_clock vclock;
    vclock::time_point vt0 = vclock::now();
    unsigned long long vk = 0;    /* ticks fired since vt0 */
    const vclock::time_point loop_start = vt0;
    unsigned long long ticks_total = 0;   /* across resyncs, for TJ_TICK_STATS */
    auto tick_due = [&](unsigned long long k) {
        /* whole 1001 s blocks of 60000 ticks, then the remainder: no overflow */
        return vt0 + std::chrono::nanoseconds((long long)((k / 60000ull) * 1001000000000ull +
                                                          (k % 60000ull) * 1001000000000ull / 60000ull));
    };

    /* Closing the window (or Escape) is the PS3's "Quit Game": the title gets
     * CELL_SYSUTIL_REQUEST_EXITGAME on every registered sysutil slot and a few
     * seconds to save and call sys_process_exit, with the vblank clock and the
     * FIFO drain still running so its exit path is not stuck on a GPU fence.
     * If the process is still here after that -- the title ignored the
     * request, or exit() is stuck tearing something down -- it ends here.
     * Closing used to only stop presenting: the game kept running in a frozen
     * window and the process never ended. TJ_QUIT_GRACE_MS sets the wait. */
    bool quitting = false;
    vclock::time_point quit_deadline;

    /* Drain when the guest queues work, not on a fixed 4 ms beat. The PPU
     * publishes new commands by storing `put` into CellGcmControl (lifted code,
     * no hook), and real hardware starts consuming them at once. Draining only
     * every 4 ms (plus the pass itself) left our RSX up to a frame behind the
     * PPU, and Tokyo Jungle's SPU fur task (image 9) then rewrote vertex data
     * that queued draws had not read yet: the exploding Pomeranian fur whenever
     * that task ran fast (bal-9 vs bal-17 in docs/STATUS.md). So the loop now
     * polls `put` every TJ_DRAIN_POLL_US (default 250 us; 0 = the old fixed
     * Sleep(4)) and wakes as soon as it moved, a vblank tick is due, or 4 ms
     * passed. A walker parked on a segment an SPU hasn't patched yet leaves
     * put != get without being able to progress; that doesn't count as new
     * work, or the loop would spin. */
    static long poll_us = -1;
    if (poll_us < 0) { const char* e = getenv("TJ_DRAIN_POLL_US"); poll_us = e ? atol(e) : 250; if (poll_us < 0) poll_us = 0; }
    auto ctrl_word = [](uint32_t off) -> uint32_t {
        const uint8_t* p = vm_base + ppu_hle_inject_base + 0x2000u + off;   /* CellGcmControl */
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    };
    uint32_t drained_put = 0, drained_get = 0;   /* put/get when the last drain ended */

    for (;;) {
        if (!poll_us || !vm_base) {
            Sleep(4);
        } else {
            const vclock::time_point w0 = vclock::now();
            for (;;) {
                usleep((useconds_t)poll_us);
                const vclock::time_point t = vclock::now();
                if (t - w0 >= std::chrono::milliseconds(4) || t >= tick_due(vk)) break;
                const uint32_t put = ctrl_word(0), get = ctrl_word(4);
                if (put != get && !(put == drained_put && get == drained_get)) break;   /* new work */
                if (cellGcm_rsx_waiters()) break;   /* a guest thread is blocked on the drain */
            }
        }
        vclock::time_point now = vclock::now();

        if (quitting && now >= quit_deadline) {
            const int code = g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
            fprintf(stderr, "[quit] %s; ending the process (code %d)\n",
                    g_sys_process_exit_called ? "sys_process_exit did not finish"
                                              : "the title did not exit after REQUEST_EXITGAME",
                    code);
            fflush(stdout);
            fflush(stderr);
            _exit(code);
        }

        /* A real stall (a debugger, a suspended process): start the schedule
         * again from now. Anything shorter is delivered, never dropped. */
        if (now - tick_due(vk) > std::chrono::seconds(1)) { vt0 = now; vk = 0; }

        /* The vblank clock depends on wall time only, as on hardware. Each
         * elapsed tick does just the cheap part -- count the vblank, mark a flip
         * due -- and the present and FIFO drain below run once per wake. They
         * used to run once per TICK: in gameplay one pass of them can take over
         * 100 ms, so ticks fell behind, and the old 100 ms resync then dropped
         * them -- the game's vblank clock ran at ~22 Hz. (0045 bounds how many
         * of a backlog the guest's handler is given at once.) */
        int fired = 0;
        while (now >= tick_due(vk) && fired < 120) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            vk++;
            ticks_total++;
            fired++;
        }

        /* Present and drain once per wake (every ~4 ms plus the work), not per
         * tick: titles fence every render pass on a label the drain writes, and
         * one wave of those at 16 ms apiece paces the guest into single-figure
         * frame rates. The real RSX writes them in microseconds. Present a
         * pending flip BEFORE draining further: the flip fires at a get==put
         * frame boundary, so the batch held now is exactly the completed frame;
         * presenting after the drain races the guest's next-frame writes. */
        if (rsx_ok) {
            /* TJ_TICK_STATS=1: how long the present + drain take, i.e. how far
             * behind the ticks would fall if they still waited for them. */
            static int tick_stats = -1;
            if (tick_stats < 0) tick_stats = getenv("TJ_TICK_STATS") ? 1 : 0;
            vclock::time_point t0 = tick_stats ? vclock::now() : vclock::time_point();
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            vclock::time_point t1 = tick_stats ? vclock::now() : vclock::time_point();
            cellGcm_rsx_process_fifo();
            if (vm_base) { drained_put = ctrl_word(0); drained_get = ctrl_word(4); }
            if (tick_stats) {
                static double pmax, fmax, psum, fsum; static unsigned long n, slow;
                const double pm = std::chrono::duration<double, std::milli>(t1 - t0).count();
                const double fm = std::chrono::duration<double, std::milli>(vclock::now() - t1).count();
                psum += pm; fsum += fm; n++;
                if (pm > pmax) pmax = pm;
                if (fm > fmax) fmax = fm;
                if (pm + fm > 100.0) slow++;
                if (n % 2000 == 0) {
                    fprintf(stderr, "[tick-stats] wakes=%lu present avg %.2f max %.1f ms, fifo avg %.2f max %.1f ms, >100ms: %lu, vblank ticks %llu in %.1f s\n",
                            n, psum / n, pmax, fsum / n, fmax, slow, ticks_total,
                            std::chrono::duration<double>(vclock::now() - loop_start).count());
                    pmax = fmax = 0;
                }
            }

            /* TJ_QUIT_AFTER_S=<s>: take the window-close path after s seconds,
             * to test quitting headless. */
            static const char* quit_after = getenv("TJ_QUIT_AFTER_S");
            const bool quit_now = quit_after &&
                now - loop_start >= std::chrono::seconds(atoi(quit_after));
            if ((rsx_backend_pump() != 0 || quit_now) && !quitting) {
                const char* g = getenv("TJ_QUIT_GRACE_MS");
                const long grace_ms = g ? strtol(g, NULL, 0) : 3000;
                quitting = true;
                quit_deadline = now + std::chrono::milliseconds(grace_ms > 0 ? grace_ms : 0);
                for (int slot = 0; slot < 4; slot++)     /* CELL_SYSUTIL_MAX_CALLBACKS */
                    cellSysutilQueueEvent(slot, 0x0101u /* REQUEST_EXITGAME */, 0);
                fprintf(stderr, "[quit] window closed: REQUEST_EXITGAME sent, %ld ms to exit\n",
                        grace_ms);
            }
            fps_overlay_update();   /* F1 / TJ_SHOW_FPS: fps in the top-right corner */
            /* Present on a guest flip. A present on a fixed clock can catch the
             * drain mid-frame and flash a partial one. Before the first flip
             * present freely, so the window is not blank during boot. */
            unsigned fc = cellGcm_flip_request_count();
            if (fc != last_flip) {
                present_guest_frame();
                last_flip = fc;
            } else if (fc == 0) {
                rsx_backend_present();
            }
        }
        frame_dump_collect();
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Two filesystem layers read guest paths, and they resolve them differently:
 *
 *   lv2 sys_fs (ppu_fs.cpp)  ppu_vfs_root + <path after the mount point>, except
 *                            /dev_hdd0/... -> $PS3_HDD0_ROOT/... and
 *                            /dev_flash/... -> $PS3_DEV_FLASH/...
 *   cellFs HLE (cellFs.c)    <its root>/<mapping>/<remainder>, longest prefix
 *
 * The prior port lost its sound banks, and with them the whole boot, three
 * separate times to these two disagreeing (docs/PRIOR_PORT.md). So both are
 * pointed at ONE host directory laid out the way a PS3 sees this title:
 *
 *   <run>/USRDIR                          -> <game>/USRDIR    /app_home/USRDIR
 *   <run>/PS3_GAME                        -> <game>           /dev_bdvd/PS3_GAME
 *   <run>/dev_hdd0/game/NPUA80523         -> <game>           the installed title
 *   <run>/dev_hdd0/...                    real directories    saves, trophies
 *   <run>/dev_flash                       -> $PS3_DEV_FLASH, or an empty dir
 *
 * <game> is the directory holding USRDIR/ and PARAM.SFO. The links mean saves
 * and the data-install flow's directories land under <run>, not in <game>,
 * except for writes the title makes into its own /dev_hdd0/game/NPUA80523.
 *
 * $PS3_VFS_ROOT skips all of this and uses that directory as-is, as the
 * template does. */
#define TJ_TITLE_ID "NPUA80523"

static char s_run_dir[1024];

static int mkdir_p(const char* path)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char* p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return (mkdir(buf, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

/* Make <link> a symlink to <target>, replacing a stale link or an EMPTY
 * directory (the placeholder an earlier run made when it had no dev_flash), but
 * never a file or a directory with anything in it -- that would be someone's
 * data. rmdir refuses non-empty directories, which is the guarantee. */
static int ensure_link(const char* link, const char* target)
{
    struct stat st;
    if (lstat(link, &st) == 0) {
        if (S_ISDIR(st.st_mode) && rmdir(link) == 0) {
            /* was an empty placeholder; fall through and link */
        } else if (!S_ISLNK(st.st_mode)) {
            fprintf(stderr, "ERROR: %s exists and is not a symlink; not replacing it\n", link);
            return -1;
        }
        char cur[1024];
        ssize_t n = readlink(link, cur, sizeof cur - 1);
        if (n >= 0) { cur[n] = 0; if (strcmp(cur, target) == 0) return 0; }
        unlink(link);
    }
    if (symlink(target, link) != 0) {
        fprintf(stderr, "ERROR: symlink %s -> %s: %s\n", link, target, strerror(errno));
        return -1;
    }
    return 0;
}

/* <game> from, in order: argv[2], $TJ_GAME_DIR, the ELF's own location when it
 * sits in a USRDIR (<game>/USRDIR/EBOOT.ELF), or this checkout's extracted/,
 * which is where tools/extract/ unpacks the PKGs (TJ_DEFAULT_GAME_DIR, set by
 * CMakeLists.txt). */
static bool find_game_dir(const char* eboot, const char* arg, char* out, size_t cap)
{
    const char* env = getenv("TJ_GAME_DIR");
    if (arg && *arg)      snprintf(out, cap, "%s", arg);
    else if (env && *env) snprintf(out, cap, "%s", env);
    else {
        char buf[1024];
        snprintf(buf, sizeof buf, "%s", eboot);
        char* s = strrchr(buf, '/');
        if (s) *s = 0;                                /* drop EBOOT.ELF */
        s = s ? strrchr(buf, '/') : nullptr;
        const char* dir = s ? s + 1 : buf;
        if (s && strcasecmp(dir, "USRDIR") == 0) {
            *s = 0;
            snprintf(out, cap, "%s", buf);
        } else {
#ifdef TJ_DEFAULT_GAME_DIR
            snprintf(out, cap, "%s", TJ_DEFAULT_GAME_DIR);
#else
            return false;
#endif
        }
    }
    char real[PATH_MAX];
    if (!realpath(out, real)) return false;
    snprintf(out, cap, "%s", real);
    return true;
}

static bool setup_vfs(const char* eboot, const char* game_arg)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) {
        ppu_vfs_root = env;
        cellfs_set_root_path(env);
        printf("[boot] PS3_VFS_ROOT set; using %s as-is\n", env);
        char sfo[PATH_MAX + 16];
        snprintf(sfo, sizeof sfo, "%s/PARAM.SFO", env);
        cellGame_init_from_paramsfo(sfo);
        return true;
    }

    char game[PATH_MAX], probe[PATH_MAX + 16];
    if (!find_game_dir(eboot, game_arg, game, sizeof game)) {
        fprintf(stderr,
            "ERROR: cannot tell where the game data is. Pass the directory holding\n"
            "       USRDIR/ as the second argument, or set TJ_GAME_DIR.\n");
        return false;
    }
    snprintf(probe, sizeof probe, "%s/USRDIR", game);
    struct stat st;
    if (stat(probe, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "ERROR: %s has no USRDIR/\n", game);
        return false;
    }

    /* <run>: $TJ_RUN_DIR, else vfs/ beside the executable (a build dir, so it
     * is gitignored and per-build). */
    const char* run_env = getenv("TJ_RUN_DIR");
    if (run_env && *run_env) {
        snprintf(s_run_dir, sizeof s_run_dir, "%s", run_env);
    } else {
        char exe[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = 0;
            char* s = strrchr(exe, '/');
            if (s) *s = 0;
            snprintf(s_run_dir, sizeof s_run_dir, "%s/vfs", exe);
        } else {
            snprintf(s_run_dir, sizeof s_run_dir, "vfs");
        }
    }

    char path[PATH_MAX + 64], target[PATH_MAX + 16];
    snprintf(path, sizeof path, "%s/dev_hdd0/game", s_run_dir);
    if (mkdir_p(path) != 0) {
        fprintf(stderr, "ERROR: cannot create %s: %s\n", path, strerror(errno));
        return false;
    }

    snprintf(path, sizeof path, "%s/USRDIR", s_run_dir);
    snprintf(target, sizeof target, "%s/USRDIR", game);
    if (ensure_link(path, target) != 0) return false;
    snprintf(path, sizeof path, "%s/PS3_GAME", s_run_dir);
    if (ensure_link(path, game) != 0) return false;
    snprintf(path, sizeof path, "%s/dev_hdd0/game/" TJ_TITLE_ID, s_run_dir);
    if (ensure_link(path, game) != 0) return false;

    /* /dev_flash is firmware: system fonts live there, which cellFont reads.
     * ppu_fs's fallback when $PS3_DEV_FLASH is unset is a Windows path, so pick
     * one here: $PS3_DEV_FLASH, else the dev_flash an RPCS3 firmware install
     * left (flatpak, then native), else an empty directory. */
    const char* flash = getenv("PS3_DEV_FLASH");
    static char rpcs3_flash[PATH_MAX];
    const char* home = getenv("HOME");
    if ((!flash || !*flash) && home) {
        static const char* const candidates[] = {
            "%s/.var/app/net.rpcs3.RPCS3/config/rpcs3/dev_flash",
            "%s/.config/rpcs3/dev_flash",
        };
        for (const char* fmt : candidates) {
            snprintf(rpcs3_flash, sizeof rpcs3_flash, fmt, home);
            char ver[PATH_MAX + 32];
            snprintf(ver, sizeof ver, "%s/vsh/etc/version.txt", rpcs3_flash);
            if (stat(ver, &st) == 0) { flash = rpcs3_flash; break; }
        }
        if (flash) printf("[boot] dev_flash: %s (RPCS3 firmware install)\n", flash);
    }
    snprintf(path, sizeof path, "%s/dev_flash", s_run_dir);
    if (flash && *flash) {
        if (ensure_link(path, flash) != 0) return false;
    } else {
        struct stat fst;
        if (lstat(path, &fst) == 0 && S_ISLNK(fst.st_mode)) unlink(path);
        mkdir_p(path);
        fprintf(stderr, "[boot] no dev_flash found: /dev_flash is an empty directory "
                        "(no system fonts). Set PS3_DEV_FLASH, or install firmware "
                        "into RPCS3 (tools/oracle/README.md)\n");
    }
    setenv("PS3_DEV_FLASH", path, 1);

    snprintf(path, sizeof path, "%s/dev_hdd0", s_run_dir);
    setenv("PS3_HDD0_ROOT", path, 0);   /* an explicit one wins */

    ppu_vfs_root = s_run_dir;
    cellfs_set_root_path(s_run_dir);
    cellfs_add_path_mapping("/app_home/",  "");
    cellfs_add_path_mapping("/dev_bdvd/",  "");
    cellfs_add_path_mapping("/dev_hdd0/",  "dev_hdd0/");
    cellfs_add_path_mapping("/dev_flash/", "dev_flash/");

    printf("[boot] game data: %s\n", game);
    printf("[boot] VFS root:  %s\n", s_run_dir);

    /* cellGame's title id, title and versions come from PARAM.SFO, which only
     * happens if the host asks: the template never did, so BootCheck reported
     * the placeholder 'BLES00000' and every title-id path pointed at the wrong
     * directory. */
    snprintf(probe, sizeof probe, "%s/PARAM.SFO", game);
    cellGame_init_from_paramsfo(probe);
    return true;
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* TJ: the template calloc()s 0xE0000000 bytes here. glibc serves that from
     * mmap but returns it 16 bytes past the page boundary, so vm_base is not
     * page-aligned -- and vm_commit/vm_protect (runtime/memory/vm.h) mprotect
     * from vm_base + addr, which then fails with EINVAL on every call. The
     * first casualty is sys_memory_allocate: all of Tokyo Jungle's 1 MB heap
     * grabs came back ENOMEM, its CRT threw std::bad_alloc and the title
     * aborted before main. (The toolkit's smoke title never allocates, so CI
     * cannot see this.)
     *
     * mmap the whole guest space instead, as the Windows path reserves it:
     * page-aligned, MAP_NORESERVE so only touched pages cost anything, and
     * every 32-bit guest address backed, so the OOB guard is off as there. */
    void* p = mmap(NULL, VM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    vm_base = (p == MAP_FAILED) ? nullptr : (uint8_t*)p;
    ppu_vm_size = 0;
#endif
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: %s <EBOOT.ELF> [<game dir holding USRDIR/>]\n", argv[0]);
        return 2;
    }

    /* SDL (cellPad, cellAudio, the window) otherwise installs SIGINT/SIGTERM
     * handlers that turn Ctrl+C and kill into an SDL_QUIT event. Headless,
     * nothing polls events, so both were ignored and the process never ended.
     * Default signal behaviour; closing the window still quits through the
     * frame clock. Must be set before the first SDL_InitSubSystem. */
    setenv("SDL_NO_SIGNAL_HANDLERS", "1", 0);

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
#endif

    printf("=== ps3recomp game runner ===\n");

    /* TJ: this port is a pure recompilation -- no hardware is emulated in it.
     * ps3recomp ships an SPU interpreter and a lockstep scheduler, each off
     * unless one of these is set; several are tested for PRESENCE, so even
     * "=0" turns them on. Refuse to run rather than quietly interpret: missing
     * SPU code is a "needs lifting" bug (scripts/lift_spu.py), not something to
     * fall back on. */
    static const char* const k_forbidden_env[] = {
        "SPU_INTERP_UNLIFTED",   /* spu_channels.c: interpret runtime-loaded LS code */
        "RD_SPU_INTERP",         /* lv2_register.c: interpret unregistered thread groups */
        "RD_SPU_INTERP_ASYNC",
        "SPU_LOCKSTEP",          /* spu_lockstep.c: round-robin SPU scheduling gate */
        "SPU_LOCKSTEP_QUANTUM",
    };
    for (const char* name : k_forbidden_env) {
        if (getenv(name)) {
            fprintf(stderr, "ERROR: %s is set. This port runs lifted code only; "
                            "unset it. Missing SPU code must be lifted, not "
                            "interpreted.\n", name);
            return 2;
        }
    }

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }

    if (!setup_vfs(argv[1], argc > 2 ? argv[2] : nullptr))
        return 1;

    /* argv[0] for the guest. ppu_run defaults to a /dev_bdvd disc path, which is
     * right for a disc title and wrong for this one: Tokyo Jungle shipped on PSN
     * only. This is what the real game is handed -- the first line of its TTY
     * under RPCS3 is "main /dev_hdd0/game/NPUA80523/USRDIR/EBOOT.BIN", and its
     * sgsfile layer derives boot device 5 (/dev_hdd0/game/NPUA80523/) from it.
     * (The prior port used /app_home/USRDIR/EBOOT.BIN.) */
    setenv("PS3_BOOT_PATH", "/dev_hdd0/game/" TJ_TITLE_ID "/USRDIR/EBOOT.BIN", 0);

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    tj_register_hle_overrides();  /* TJ: stubs.cpp, forwarders for sibling NIDs */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */

#if defined(PS3RECOMP_RSX_VULKAN) && !defined(_WIN32) && !defined(__APPLE__)
    /* The Vulkan backend renders offscreen unless PS3RECOMP_VK_WINDOW is set.
     * Open a window when there is a display to open it on; TJ_HEADLESS=1 (or
     * an explicit PS3RECOMP_VK_WINDOW=0) keeps it offscreen. */
    if (!getenv("TJ_HEADLESS") && (getenv("WAYLAND_DISPLAY") || getenv("DISPLAY")))
        setenv("PS3RECOMP_VK_WINDOW", "1", 0);
    if (const char* w = getenv("PS3RECOMP_VK_WINDOW"); w && strcmp(w, "0") == 0)
        unsetenv("PS3RECOMP_VK_WINDOW");
    /* Draw with the game's own vertex/fragment programs. The backend's other
     * path is ps3recomp's fixed-function test fallback: no blending, opaque
     * texture alpha, no DXT, no indexed draws, no render-to-texture. Tokyo
     * Jungle's screens are composited, so it drew the title screen as a white
     * page with grey bars and put a white box over the disclaimer. =0 keeps
     * the fallback for comparison. */
    setenv("PS3RECOMP_VK_GUEST_PROGRAMS", "1", 0);
#endif
    frame_dump_setup();

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;
    /* SDL's controller subsystem must be up before the frame clock starts
     * pumping window events, or cellPadInit deadlocks against it (cellPad.c). */
    cellPad_host_init();
    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
