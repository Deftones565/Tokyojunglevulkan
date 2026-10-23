/*
 * Tokyo Jungle launcher: builds the port on the player's computer, from the
 * player's own copy of the game, then starts it.
 *
 * Nothing game-derived ships with the port. The player installs and decrypts
 * the game with RPCS3, by hand (docs/SETUP_GUIDE.md); this launcher does
 * everything after that, in order, and only what is not done yet:
 *
 *   1. the game     find the copy in RPCS3 (or a folder the player drops or
 *                   chooses), check it, and put it in the data folder
 *                   (src/game_data.h)
 *   2. recompiler   apply this project's ps3recomp patches
 *                   (scripts/apply_patches.sh)
 *   3. game code    lift the EBOOT's PPU code to C++ (scripts/analyze.py)
 *   4. SPU jobs     cut the game's 18 SPU job images out of the decrypted
 *                   EBOOT (scripts/extract_spu_jobs.py: a table of offsets)
 *                   and lift them (scripts/lift_spu.py). Play runs with
 *                   SPU_DUMP_MISS, so a job no one has met before would be
 *                   saved, and lifted at the next start.
 *   5. build        compile the game (CMake + Ninja, into build/), once,
 *                   with both lifts in it
 *
 * All of it happens before the game runs: the game stays lifted, compiled
 * code only (README.md, "No emulation"). The build tools themselves are checked and installed
 * by ./tokyojungle.sh, which builds this launcher and starts it.
 *
 * The window is SDL2 with the SDL renderer and stb_truetype. Long steps run
 * child processes on a worker thread, their output into <data>/launcher.log;
 * the window shows progress read from them.
 */
#include "game_data.h"

#include <SDL2/SDL.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace fs = std::filesystem;
using namespace tjgd;

namespace {

#ifndef TJ_SOURCE_DIR
#define TJ_SOURCE_DIR "."
#endif
#ifndef TJ_GUIDE_URL
#define TJ_GUIDE_URL "https://github.com/Deftones565/tokyojunglelinux/blob/main/docs/SETUP_GUIDE.md"
#endif

/* The SPU job images scripts/extract_spu_jobs.py cuts out of the EBOOT. */
const int kSpuJobs = 18;

std::string pathstr(const fs::path& p) { return p.string(); }

/* ---------------------------------------------------------------------------
 * The source tree
 * -----------------------------------------------------------------------*/
struct Tree {
    fs::path src, build, ppu, spu, dumps, venv_py, data, log;
};

Tree tree()
{
    Tree t;
    /* $TJ_SOURCE, else the tree this launcher sits in (build-launcher/..),
     * else the one it was built from. */
    const char* env = getenv("TJ_SOURCE");
    std::error_code ec;
    const fs::path beside = fs::read_symlink("/proc/self/exe", ec).parent_path().parent_path();
    t.src = fs::weakly_canonical(env && *env ? fs::path(env)
                                 : !ec && present(beside / "src/main.cpp") ? beside : fs::path(TJ_SOURCE_DIR));
    t.build = t.src / "build";
    t.ppu = t.src / "generated/ppu";
    t.spu = t.src / "generated/spu";
    t.dumps = t.spu / "dumps";
    t.venv_py = t.src / ".venv/bin/python";
    t.data = tj_data_root();
    t.log = t.data / "launcher.log";
    return t;
}

int count_files(const fs::path& dir, const char* prefix, const char* suffix)
{
    int n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string f = it->path().filename().string();
        if (f.rfind(prefix, 0) == 0 && f.size() >= strlen(suffix) &&
            f.compare(f.size() - strlen(suffix), strlen(suffix), suffix) == 0)
            n++;
    }
    return n;
}

/* SPU jobs captured (dumps) that have no lifted image yet. */
int unlifted_jobs(const Tree& t)
{
    int n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(t.dumps, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string f = it->path().filename().string();   /* spujob_<FP16>_<size>.bin */
        if (f.rfind("spujob_", 0) != 0 || f.size() < 24) continue;
        if (!present(t.spu / ("j" + f.substr(7, 16)) / "spu_recomp.c")) n++;
    }
    return n;
}

int lifted_jobs(const Tree& t)
{
    int n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(t.spu, ec), end; !ec && it != end; it.increment(ec))
        if (present(it->path() / "spu_recomp.c")) n++;
    return n;
}

/* ---------------------------------------------------------------------------
 * Where things stand
 * -----------------------------------------------------------------------*/
struct Status {
    Source src;                  /* the copy of the game found */
    bool imported = false;       /* the data folder is complete */
    int patches = 0, applied = 0;
    bool lifted = false;         /* generated/ppu */
    bool built = false;          /* build/tokyojungle */
    int spu_lifted = 0, spu_new = 0;

    bool spu_done() const { return spu_new == 0 && spu_lifted >= kSpuJobs; }
    bool ready() const { return imported && applied >= patches && lifted && spu_done() && built; }
};

Status survey(const Tree& t, const Pointed& pt)
{
    Status s;
    s.src = find_source(pt);
    TjGamePaths p;
    s.imported = tj_data_ready(&p, nullptr);
    std::error_code ec;
    std::vector<std::string> names;
    for (fs::directory_iterator it(t.src / "patches/ps3recomp", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string f = it->path().filename().string();
        if (f.size() > 6 && isdigit((unsigned char)f[0]) && f.compare(f.size() - 6, 6, ".patch") == 0) names.push_back(f);
    }
    s.patches = (int)names.size();
    std::string rec;
    if (FILE* f = fopen((t.src / "third_party/ps3recomp/.tj-applied-patches").c_str(), "r")) {
        char line[512];
        while (fgets(line, sizeof line, f)) rec += line;
        fclose(f);
    }
    for (const std::string& n : names)
        if (rec.find(n + "\n") != std::string::npos) s.applied++;
    s.lifted = present(t.ppu / "ppu_recomp.h");
    s.built = present(t.build / "tokyojungle");
    s.spu_lifted = lifted_jobs(t);
    s.spu_new = unlifted_jobs(t);
    return s;
}

enum class Mark { Ok, Todo, Need };
struct Row { const char* title; Mark mark; std::string detail; };

std::vector<Row> checklist(const Status& s)
{
    std::vector<Row> r;
    if (s.imported) r.push_back({"Your game", Mark::Ok, "In the data folder"});
    else if (s.src.complete()) r.push_back({"Your game", Mark::Todo, "Found in " + s.src.label + ": will be taken from there"});
    else r.push_back({"Your game", Mark::Need, what_is_missing(s.src)});

    if (s.applied >= s.patches) r.push_back({"Recompiler", Mark::Ok, "ps3recomp with this port's " + std::to_string(s.patches) + " fixes"});
    else r.push_back({"Recompiler", Mark::Todo, "Will apply " + std::to_string(s.patches - s.applied) + " fixes to ps3recomp"});

    if (s.lifted) r.push_back({"Game code", Mark::Ok, "Recompiled from your EBOOT"});
    else r.push_back({"Game code", Mark::Todo, "Will recompile your EBOOT to C++ (a few minutes)"});

    if (s.spu_new && s.spu_lifted >= kSpuJobs)
        r.push_back({"SPU jobs", Mark::Todo, std::to_string(s.spu_new) + " new job(s) met while playing: will recompile them"});
    else if (!s.spu_done()) r.push_back({"SPU jobs", Mark::Todo, "Will take the game's SPU jobs from your EBOOT and recompile them"});
    else r.push_back({"SPU jobs", Mark::Ok, std::to_string(s.spu_lifted) + " jobs recompiled"});

    if (s.built && s.ready()) r.push_back({"Build", Mark::Ok, "Compiled for this computer"});
    else r.push_back({"Build", Mark::Todo, "Will compile Tokyo Jungle for this computer (about 10 minutes)"});
    return r;
}

/* ---------------------------------------------------------------------------
 * Running programs
 * -----------------------------------------------------------------------*/
std::atomic<bool> g_cancel{false};

/* Run `args` in `cwd` with `env` added, output into the log. `poll` is
 * called every 200 ms while it runs; true from it (or a cancel) kills the
 * program and everything it started. Returns the exit status, 128 + signal
 * when killed. */
int run(const Tree& t, const std::vector<std::string>& args, const fs::path& cwd,
        const std::vector<std::string>& env = {}, const std::function<bool()>& poll = nullptr)
{
    std::vector<std::string> envs;
    for (char** e = environ; *e; e++) {
        const char* eq = strchr(*e, '=');
        const std::string key(*e, eq ? (size_t)(eq - *e) + 1 : strlen(*e));
        bool replaced = false;
        for (const std::string& add : env) replaced |= add.rfind(key, 0) == 0;
        if (!replaced) envs.emplace_back(*e);
    }
    envs.insert(envs.end(), env.begin(), env.end());
    std::vector<char*> argv, envp;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    for (const std::string& e : envs) envp.push_back(const_cast<char*>(e.c_str()));
    argv.push_back(nullptr);
    envp.push_back(nullptr);

    std::error_code ec;
    fs::create_directories(t.log.parent_path(), ec);
    const int log = open(t.log.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (log >= 0) {
        std::string line = "\n$ (cd " + pathstr(cwd) + " &&";
        for (const std::string& e : env) line += " " + e.substr(0, 120);
        for (const std::string& a : args) line += " " + a;
        line += ")\n";
        if (write(log, line.data(), line.size()) < 0) {}
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (log >= 0) {
        posix_spawn_file_actions_adddup2(&fa, log, 1);
        posix_spawn_file_actions_adddup2(&fa, log, 2);
    }
    posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    const int err = posix_spawnp(&pid, argv[0], &fa, &at, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (log >= 0) close(log);
    if (err != 0) {
        say("could not start %s: %s", args[0].c_str(), strerror(err));
        return 127;
    }
    int st = 0;
    bool killed = false;
    for (;;) {
        const pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) return 127;
        if (!killed && (g_cancel || (poll && poll()))) {
            kill(-pid, SIGKILL);
            killed = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (killed) kill(-pid, SIGKILL);   /* anything it left behind in its group */
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

/* The last "[done/total]" Ninja has printed into the log. */
bool ninja_progress(const Tree& t, int* done, int* total)
{
    FILE* f = fopen(t.log.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f), n = std::min(size, 16384L);
    fseek(f, size - n, SEEK_SET);
    std::string tail((size_t)n, '\0');
    tail.resize(fread(tail.data(), 1, (size_t)n, f));
    fclose(f);
    for (size_t at = tail.rfind('['); at != std::string::npos; at = at ? tail.rfind('[', at - 1) : std::string::npos) {
        if (sscanf(tail.c_str() + at, "[%d/%d]", done, total) == 2 && *total > 0) return true;
        if (!at) break;
    }
    return false;
}

/* Jobs for the compiler: the lifted PPU sources need several GB each. */
int build_jobs()
{
    long avail_kb = 0;
    if (FILE* f = fopen("/proc/meminfo", "r")) {
        char line[256];
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "MemAvailable: %ld kB", &avail_kb) == 1) break;
        fclose(f);
    }
    const int cpus = std::max(1u, std::thread::hardware_concurrency());
    const int by_mem = avail_kb ? (int)(avail_kb / (3L * 1024 * 1024)) : cpus;
    return std::clamp(by_mem, 1, cpus);
}

std::string mmss(int s)
{
    char b[32];
    snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    return b;
}

/* ---------------------------------------------------------------------------
 * The steps
 * -----------------------------------------------------------------------*/
bool step_patches(const Tree& t)
{
    set_step("Applying this port's fixes to the recompiler");
    if (!present(t.src / "third_party/ps3recomp/CMakeLists.txt"))
        return fail("third_party/ps3recomp is missing: start the launcher with ./tokyojungle.sh, which fetches it");
    if (run(t, {"bash", "scripts/apply_patches.sh"}, t.src) != 0)
        return fail("a ps3recomp patch did not apply (see launcher.log)");
    return true;
}

bool python_env(const Tree& t)
{
    const std::string check = "import capstone, construct, Crypto, tabulate, tqdm";
    if (present(t.venv_py) && run(t, {pathstr(t.venv_py), "-c", check}, t.src) == 0) return true;
    set_step("Setting up the recompiler's Python tools");
    if (!present(t.venv_py) && run(t, {"python3", "-m", "venv", ".venv"}, t.src) != 0)
        return fail("could not make a Python environment (python3 -m venv)");
    if (run(t, {pathstr(t.venv_py), "-m", "pip", "install", "-q", "-r", "third_party/ps3recomp/requirements.txt"}, t.src) != 0)
        return fail("could not install the recompiler's Python modules (is the internet reachable? see launcher.log)");
    return true;
}

bool step_lift(const Tree& t)
{
    if (!python_env(t)) return false;
    set_step("Recompiling the game's code from your EBOOT");
    const auto t0 = std::chrono::steady_clock::now();
    const int rc = run(t, {pathstr(t.venv_py), "scripts/analyze.py", "--clean", "--elf", pathstr(t.data / "EBOOT.ELF"),
                           "-j", std::to_string(std::max(1u, std::thread::hardware_concurrency()))},
                       t.src, {}, [&] {
                           const int s = (int)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
                           set_frac(-1, "Working for " + mmss(s) + " (a few minutes)");
                           return false;
                       });
    if (rc != 0 || !present(t.ppu / "ppu_recomp.h")) return fail("recompiling the game's code failed (see launcher.log)");
    return true;
}

bool step_build(const Tree& t, const std::string& what)
{
    if (!present(t.build / "build.ninja")) {
        set_step("Preparing the build");
        if (run(t, {"cmake", "-S", ".", "-B", "build", "-G", "Ninja", "-DCMAKE_C_COMPILER=clang",
                    "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_BUILD_TYPE=RelWithDebInfo"}, t.src) != 0)
            return fail("CMake could not configure the build (see launcher.log)");
    }
    set_step(what, 0);
    const int jobs = build_jobs();
    const int rc = run(t, {"ninja", "-C", "build", "-j", std::to_string(jobs)}, t.src, {}, [&] {
        int d, n;
        if (ninja_progress(t, &d, &n))
            set_frac((double)d / n, std::to_string(d) + " of " + std::to_string(n) + " files, " + std::to_string(jobs) + " at a time");
        return false;
    });
    if (rc != 0 || !present(t.build / "tokyojungle")) return fail("the build failed (see launcher.log)");
    return true;
}

/* The SPU jobs: cut out of the EBOOT when they aren't there yet, then every
 * image without lifted code lifted (a job met while playing included). */
bool step_spu(const Tree& t)
{
    if (!python_env(t)) return false;
    if (count_files(t.dumps, "spujob_", ".bin") < kSpuJobs) {
        set_step("Taking the game's SPU jobs from your EBOOT");
        if (run(t, {pathstr(t.venv_py), "scripts/extract_spu_jobs.py", pathstr(t.data / "EBOOT.ELF"), pathstr(t.dumps)}, t.src) != 0)
            return fail("the SPU jobs are not where they should be in your EBOOT (see launcher.log)");
    }
    const int n = unlifted_jobs(t);
    if (!n) return true;
    set_step("Recompiling " + std::to_string(n) + " SPU job(s)");
    if (run(t, {pathstr(t.venv_py), "scripts/lift_spu.py"}, t.src) != 0 || unlifted_jobs(t))
        return fail("recompiling the SPU jobs failed (see launcher.log)");
    /* the geometry job's inner loop, fused (tools/spu-opt; again after every lift) */
    if (present(t.spu / "jE7517A8D6D4308F3"))
        run(t, {pathstr(t.venv_py), "tools/spu-opt/fuse_geometry_loop.py", pathstr(t.spu)}, t.src);
    if (present(t.build / "build.ninja")) run(t, {"cmake", "-S", ".", "-B", "build"}, t.src);   /* picks up the new images */
    return true;
}

/* Everything not done yet, in order. True when the game is ready. */
bool setup_all(const Tree& t, const Source& src)
{
    {
        std::lock_guard<std::mutex> lk(progress().mu);
        progress().error.clear();
    }
    g_cancel = false;
    std::error_code ec;
    fs::create_directories(t.data, ec);
    say("Setting up in %s (source %s)", pathstr(t.data).c_str(), pathstr(t.src).c_str());
    Status s = survey(t, {});
    bool ok = true;
    if (!s.imported) ok = import(src);
    if (ok && s.applied < s.patches) ok = step_patches(t);
    if (ok && !s.lifted) ok = step_lift(t);
    if (ok) ok = step_spu(t);
    if (ok) ok = step_build(t, "Compiling Tokyo Jungle for this computer");
    if (ok) set_step("Done. Tokyo Jungle is ready to play.", 1);
    else if (g_cancel) fail("stopped");
    return ok;
}

/* Start the game in place of the launcher. */
[[noreturn]] void play(const Tree& t)
{
    std::error_code ec;
    fs::current_path(t.data, ec);           /* saves go to <data>/gamedata */
    setenv("TJ_DATA", pathstr(t.data).c_str(), 1);
    setenv("SPU_DUMP_MISS", pathstr(t.dumps).c_str(), 1);   /* a job first met while playing */
    const std::string exe = pathstr(t.build / "tokyojungle");
    execl(exe.c_str(), exe.c_str(), (char*)nullptr);
    fprintf(stderr, "could not start %s: %s\n", exe.c_str(), strerror(errno));
    _exit(127);
}

/* ---------------------------------------------------------------------------
 * The window
 * -----------------------------------------------------------------------*/
/* Colours (0xRRGGBB): night-time Shibuya. */
const uint32_t kBg = 0x14161a, kPanel = 0x1d2026, kLine = 0x343943, kText = 0xeceae4,
               kDim = 0x8c919b, kAccent = 0xe8a33d, kOk = 0x5cc77a, kNeed = 0xe5534b;

struct Glyph { SDL_Texture* tex = nullptr; int w = 0, h = 0, x0 = 0, y0 = 0; float adv = 0; };

struct Ui {
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    float s = 1;                       /* pixels per layout unit */
    float w = 0, h = 0;                /* the window in layout units */
    float mx = -1, my = -1;            /* the mouse, in layout units */
    bool click = false;
    std::vector<unsigned char> font_data;
    stbtt_fontinfo font;
    bool font_ok = false;
    std::map<uint64_t, Glyph> glyphs;

    void color(uint32_t c, float a = 1)
    {
        SDL_SetRenderDrawColor(ren, (Uint8)(c >> 16), (Uint8)(c >> 8), (Uint8)c, (Uint8)(a * 255));
    }
    void fill(float x, float y, float ww, float hh, uint32_t c, float a = 1)
    {
        color(c, a);
        SDL_FRect r = {x * s, y * s, ww * s, hh * s};
        SDL_RenderFillRectF(ren, &r);
    }
    void frame(float x, float y, float ww, float hh, uint32_t c, float t = 1)
    {
        fill(x, y, ww, t, c); fill(x, y + hh - t, ww, t, c);
        fill(x, y, t, hh, c); fill(x + ww - t, y, t, hh, c);
    }
    void dashed(float x, float y, float ww, float hh, uint32_t c)
    {
        for (float i = 0; i < ww; i += 12) { fill(x + i, y, std::min(6.f, ww - i), 1.5f, c); fill(x + i, y + hh - 1.5f, std::min(6.f, ww - i), 1.5f, c); }
        for (float i = 0; i < hh; i += 12) { fill(x, y + i, 1.5f, std::min(6.f, hh - i), c); fill(x + ww - 1.5f, y + i, 1.5f, std::min(6.f, hh - i), c); }
    }
    void dot(float cx, float cy, float r, uint32_t c)
    {
        color(c);
        const float R = r * s, X = cx * s, Y = cy * s;
        for (float dy = -R; dy <= R; dy += 1) {
            const float dx = sqrtf(std::max(0.f, R * R - dy * dy));
            SDL_RenderDrawLineF(ren, X - dx, Y + dy, X + dx, Y + dy);
        }
    }
    bool hover(float x, float y, float ww, float hh) const
    {
        return mx >= x && mx < x + ww && my >= y && my < y + hh;
    }

    const Glyph& glyph(int px, uint32_t cp)
    {
        const uint64_t key = (uint64_t)px << 32 | cp;
        auto it = glyphs.find(key);
        if (it != glyphs.end()) return it->second;
        Glyph g;
        const float sc = stbtt_ScaleForPixelHeight(&font, (float)px);
        int adv, lsb, x1, y1;
        stbtt_GetCodepointHMetrics(&font, (int)cp, &adv, &lsb);
        g.adv = adv * sc;
        stbtt_GetCodepointBitmapBox(&font, (int)cp, sc, sc, &g.x0, &g.y0, &x1, &y1);
        g.w = x1 - g.x0;
        g.h = y1 - g.y0;
        if (g.w > 0 && g.h > 0) {
            std::vector<unsigned char> a((size_t)g.w * g.h);
            stbtt_MakeCodepointBitmap(&font, a.data(), g.w, g.h, g.w, sc, sc, (int)cp);
            std::vector<uint32_t> px32(a.size());
            for (size_t i = 0; i < a.size(); i++) px32[i] = 0x00FFFFFFu | (uint32_t)a[i] << 24;
            g.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, g.w, g.h);
            if (g.tex) {
                SDL_UpdateTexture(g.tex, nullptr, px32.data(), g.w * 4);
                SDL_SetTextureBlendMode(g.tex, SDL_BLENDMODE_BLEND);
            }
        }
        return glyphs.emplace(key, g).first->second;
    }

    static std::vector<uint32_t> decode(const std::string& t)
    {
        std::vector<uint32_t> cps;
        for (size_t i = 0; i < t.size();) {
            const unsigned char c = (unsigned char)t[i];
            const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
            uint32_t cp = n == 1 ? c : c & (0x3F >> (n - 1));
            for (int k = 1; k < n && i + k < t.size(); k++) cp = cp << 6 | ((unsigned char)t[i + k] & 0x3F);
            cps.push_back(cp);
            i += n;
        }
        return cps;
    }

    static std::string encode(const std::vector<uint32_t>& v, size_t a, size_t b)
    {
        std::string o;
        for (size_t i = a; i < b; i++) {
            const uint32_t cp = v[i];
            if (cp < 0x80) o += (char)cp;
            else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
            else { o += (char)(0xF0 | cp >> 18); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        }
        return o;
    }

    float measure(const std::string& t, float size)
    {
        if (!font_ok) return 0;
        const int px = std::max(6, (int)lroundf(size * s));
        float x = 0;
        for (uint32_t cp : decode(t)) x += glyph(px, cp).adv;
        return x / s;
    }

    /* `t` with its top-left at (x, y); align 0 left, 1 centre, 2 right. */
    void text(float x, float y, const std::string& t, float size, uint32_t c, int align = 0)
    {
        if (!font_ok) return;
        if (align) x -= measure(t, size) * (align == 1 ? 0.5f : 1.f);
        const int px = std::max(6, (int)lroundf(size * s));
        int asc, desc, gap;
        stbtt_GetFontVMetrics(&font, &asc, &desc, &gap);
        const float base = y * s + asc * stbtt_ScaleForPixelHeight(&font, (float)px);
        float pen = x * s;
        for (uint32_t cp : decode(t)) {
            const Glyph& g = glyph(px, cp);
            if (g.tex) {
                SDL_SetTextureColorMod(g.tex, (Uint8)(c >> 16), (Uint8)(c >> 8), (Uint8)c);
                SDL_FRect r = {floorf(pen) + g.x0, floorf(base) + g.y0, (float)g.w, (float)g.h};
                SDL_RenderCopyF(ren, g.tex, nullptr, &r);
            }
            pen += g.adv;
        }
    }

    /* `t` shortened in the middle to fit `maxw`. */
    std::string fit(const std::string& t, float size, float maxw)
    {
        if (measure(t, size) <= maxw) return t;
        const std::vector<uint32_t> cps = decode(t);
        for (size_t keep = cps.size(); keep > 4; keep--) {
            const size_t a = keep / 2, b = cps.size() - (keep - a);
            const std::string o = encode(cps, 0, a) + "\xE2\x80\xA6" + encode(cps, b, cps.size());
            if (measure(o, size) <= maxw) return o;
        }
        return "\xE2\x80\xA6";
    }

    bool button(float x, float y, float ww, float hh, const std::string& label, bool enabled, bool primary)
    {
        const bool over = enabled && hover(x, y, ww, hh);
        fill(x, y, ww, hh, enabled && primary ? kAccent : kPanel, over ? 0.85f : 1.f);
        frame(x, y, ww, hh, primary && enabled ? kAccent : kLine);
        text(x + ww / 2, y + (hh - 17) / 2 - 1, label, 17, !enabled ? kDim : primary ? kBg : kText, 1);
        return over && click;
    }
};

bool load_font(Ui& ui, const Source& src)
{
    /* The PS3's own UI face (Rodin), then common Linux fonts, then any. */
    const char* rodin = "data/font/SCE-PS3-RD-R-LATIN.TTF";
    std::vector<std::string> cands = {pathstr(fs::path(tj_data_root()) / "dev_flash" / rodin)};
    if (!src.flash.empty()) cands.push_back(pathstr(src.flash / rodin));
    for (const char* p : {"/usr/share/fonts/noto/NotoSans-Regular.ttf",
                          "/usr/share/fonts/TTF/DejaVuSans.ttf",
                          "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
                          "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                          "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                          "/usr/share/fonts/google-noto/NotoSans-Regular.ttf"})
        cands.push_back(p);
    std::error_code ec;
    int budget = 4000;
    for (fs::recursive_directory_iterator it("/usr/share/fonts", ec), end; !ec && it != end && budget-- > 0; it.increment(ec))
        if (lower(it->path().extension().string()) == ".ttf") { cands.push_back(pathstr(it->path())); break; }
    for (const std::string& c : cands) {
        FILE* f = fopen(c.c_str(), "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        ui.font_data.resize(n > 0 ? (size_t)n : 0);
        const bool got = n > 0 && fread(ui.font_data.data(), 1, (size_t)n, f) == (size_t)n;
        fclose(f);
        if (got && stbtt_InitFont(&ui.font, ui.font_data.data(), stbtt_GetFontOffsetForIndex(ui.font_data.data(), 0))) {
            ui.font_ok = true;
            return true;
        }
    }
    fprintf(stderr, "[launcher] no font found; the window will show no text\n");
    return false;
}

bool on_path(const char* name)
{
    const char* path = getenv("PATH");
    const std::string all = path ? path : "/usr/bin:/bin";
    for (size_t a = 0; a <= all.size();) {
        const size_t b = std::min(all.find(':', a), all.size());
        if (b > a && access((all.substr(a, b - a) + "/" + name).c_str(), X_OK) == 0) return true;
        a = b + 1;
    }
    return false;
}

/* A program's standard output (the folder picker's answer). */
int capture(const std::vector<std::string>& args, std::string* out)
{
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    int fds[2];
    if (pipe(fds) != 0) return 127;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    pid_t pid;
    const int err = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (err != 0) { close(fds[0]); return 127; }
    char b[4096];
    ssize_t k;
    while ((k = read(fds[0], b, sizeof b)) > 0 || (k < 0 && errno == EINTR))
        if (k > 0) out->append(b, (size_t)k);
    close(fds[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

void open_guide()
{
    std::thread([] { std::string o; capture({"xdg-open", TJ_GUIDE_URL}, &o); }).detach();
}

/* The desktop's folder picker, when it has one: KDE's on KDE (the Steam
 * Deck's desktop), else zenity, else kdialog. */
std::vector<std::string> folder_picker()
{
    const std::string start = pathstr(home_dir());
    const std::vector<std::string> kde = {"kdialog", "--title", "Tokyo Jungle: the game's folder",
                                          "--getexistingdirectory", start};
    if (getenv("KDE_FULL_SESSION") && on_path("kdialog")) return kde;
    if (on_path("zenity"))
        return {"zenity", "--file-selection", "--directory", "--title=Tokyo Jungle: the game's folder",
                "--filename=" + start + "/"};
    if (on_path("kdialog")) return kde;
    return {};
}

/* 1 = Play, 0 = closed, -1 = no window. */
int window(const Tree& t)
{
    if (!getenv("WAYLAND_DISPLAY") && !getenv("DISPLAY")) return -1;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[launcher] no window: %s\n", SDL_GetError());
        return -1;
    }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    Ui ui;
    ui.win = SDL_CreateWindow("Tokyo Jungle", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 640,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (ui.win) {
        ui.ren = SDL_CreateRenderer(ui.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!ui.ren) ui.ren = SDL_CreateRenderer(ui.win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!ui.win || !ui.ren) {
        fprintf(stderr, "[launcher] no window: %s\n", SDL_GetError());
        if (ui.win) SDL_DestroyWindow(ui.win);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
        return -1;
    }
    SDL_SetWindowMinimumSize(ui.win, 760, 560);
    SDL_SetRenderDrawBlendMode(ui.ren, SDL_BLENDMODE_BLEND);

    Pointed pt;
    Status st = survey(t, pt);
    load_font(ui, st.src);

    /* The folder picker's state outlives the window when the player closes
     * it with a picker still open (its thread is then let go). */
    struct Pick { std::vector<std::string> cmd; std::string path; std::atomic<bool> done{false}; };
    const auto pick = std::make_shared<Pick>();
    pick->cmd = folder_picker();

    std::thread worker, picking;
    std::atomic<bool> busy{false}, finished{false};
    bool quit = false, play_now = false, asked_stop = false;
    std::vector<SDL_GameController*> pads;
    uint32_t started = 0;

    auto point_at = [&](const fs::path& p) {
        std::string msg;
        Pointed np = pt;
        if (locate(p, &np, &msg)) {
            pt = np;
            st = survey(t, pt);
            if (!st.src.complete()) say("%s: %s", pathstr(p).c_str(), what_is_missing(st.src).c_str());
        }
        if (!msg.empty()) say("%s", msg.c_str());
    };

    while (!quit && !play_now) {
        ui.click = false;
        bool primary = false;
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, busy ? 100 : 250)) {
            do {
                switch (e.type) {
                case SDL_QUIT:
                    if (!busy) quit = true;
                    else if (asked_stop) { g_cancel = true; quit = true; }
                    else { asked_stop = true; say("The setup is still running. Close the window again to stop it."); }
                    break;
                case SDL_DROPFILE: {
                    const fs::path p = e.drop.file;
                    SDL_free(e.drop.file);
                    if (busy) say("Wait for the setup to finish.");
                    else point_at(p);
                    break;
                }
                case SDL_KEYDOWN:
                    if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) primary = true;
                    else if (e.key.keysym.sym == SDLK_ESCAPE && !busy) quit = true;
                    break;
                case SDL_CONTROLLERDEVICEADDED:
                    if (SDL_GameController* c = SDL_GameControllerOpen(e.cdevice.which)) pads.push_back(c);
                    break;
                case SDL_CONTROLLERBUTTONDOWN:
                    if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A) primary = true;
                    else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_Y) open_guide();
                    else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_B && !busy) quit = true;
                    break;
                case SDL_MOUSEMOTION:
                    ui.mx = (float)e.motion.x; ui.my = (float)e.motion.y;
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (e.button.button == SDL_BUTTON_LEFT) { ui.mx = (float)e.button.x; ui.my = (float)e.button.y; ui.click = true; }
                    break;
                }
            } while (SDL_PollEvent(&e));
        }
        if (finished.exchange(false)) {
            worker.join();
            busy = false;
            asked_stop = false;
            st = survey(t, pt);
        }
        if (pick->done.exchange(false)) {
            picking.join();
            if (!pick->path.empty()) point_at(pick->path);
        }

        /* Layout: the window in layout units (960 x 640 at scale 1). */
        int ww, wh, pw, ph;
        SDL_GetWindowSize(ui.win, &ww, &wh);
        SDL_GetRendererOutputSize(ui.ren, &pw, &ph);
        const float dpi = ww > 0 ? (float)pw / ww : 1;
        ui.s = dpi * std::clamp(std::min(ww / 960.f, wh / 640.f), 0.75f, 2.5f);
        ui.w = pw / ui.s;
        ui.h = ph / ui.s;
        const float wmx = ui.mx, wmy = ui.my;
        ui.mx = wmx * dpi / ui.s;
        ui.my = wmy * dpi / ui.s;

        ui.fill(0, 0, ui.w, ui.h, kBg);
        const float M = 32, W = ui.w - 2 * M;
        ui.text(M, 24, "TOKYO JUNGLE", 32, kText);
        ui.text(M + ui.measure("TOKYO JUNGLE", 32) + 14, 37, "Launcher", 18, kAccent);

        std::string step, detail, error;
        double frac;
        std::vector<std::string> tail;
        {
            std::lock_guard<std::mutex> lk(progress().mu);
            step = progress().step;
            detail = progress().detail;
            frac = progress().frac;
            error = progress().error;
            for (size_t i = progress().log.size() > 3 ? progress().log.size() - 3 : 0; i < progress().log.size(); i++)
                tail.push_back(progress().log[i]);
        }

        const std::vector<Row> rows = checklist(st);
        const bool need = std::any_of(rows.begin(), rows.end(), [](const Row& r) { return r.mark == Mark::Need; });
        const bool ready = st.ready();

        /* The top panel: progress, ready, what to do, or where to drop. */
        const float zy = 78, zh = 100;
        ui.fill(M, zy, W, zh, kPanel);
        if (busy) {
            ui.frame(M, zy, W, zh, kAccent);
            const int el = (int)((SDL_GetTicks() - started) / 1000);
            ui.text(M + 20, zy + 14, ui.fit(step, 17, W - 140), 17, kText);
            ui.text(M + W - 20, zy + 16, mmss(el), 15, kDim, 2);
            const float bx = M + 20, by = zy + 46, bw = W - 40, bh = 10;
            ui.fill(bx, by, bw, bh, kLine);
            if (frac >= 0) ui.fill(bx, by, (float)(bw * std::clamp(frac, 0.0, 1.0)), bh, kAccent);
            else {
                const float x = (float)((SDL_GetTicks() - started) % 1600) / 1600.f;
                ui.fill(bx + (bw - bw / 4) * (x < .5f ? 2 * x : 2 - 2 * x), by, bw / 4, bh, kAccent);
            }
            ui.text(M + 20, zy + 68, ui.fit(detail, 14, W - 40), 14, kDim);
        } else if (ready) {
            ui.frame(M, zy, W, zh, kOk);
            ui.text(M + W / 2, zy + 26, "Tokyo Jungle is ready to play", 22, kText, 1);
            ui.text(M + W / 2, zy + 62, "Built on this computer from your own copy", 14, kDim, 1);
        } else if (need) {
            ui.dashed(M, zy, W, zh, kAccent);
            ui.text(M + W / 2, zy + 22, "Drop the game's folder here", 22, kText, 1);
            ui.text(M + W / 2, zy + 60,
                    ui.fit("Install and decrypt Tokyo Jungle in RPCS3 first (see the Guide). It is found there by itself; "
                           "a copy kept elsewhere can be dropped here, or chosen.", 14, W - 40),
                    14, kDim, 1);
        } else {
            ui.frame(M, zy, W, zh, kAccent);
            ui.text(M + W / 2, zy + 22, "Everything is here", 22, kText, 1);
            ui.text(M + W / 2, zy + 60,
                    ui.fit("Set up builds Tokyo Jungle for this computer from your copy. The first time takes "
                           "about 15 minutes; you can leave it running.", 14, W - 40),
                    14, kDim, 1);
        }

        /* The steps. */
        float y = zy + zh + 16;
        const float rh = 46;
        for (const Row& r : rows) {
            const uint32_t c = r.mark == Mark::Ok ? kOk : r.mark == Mark::Todo ? kAccent : kNeed;
            ui.dot(M + 8, y + rh / 2, 6, c);
            ui.text(M + 28, y + (rh - 17) / 2, r.title, 17, kText);
            ui.text(M + 190, y + (rh - 15) / 2 + 1, ui.fit(r.detail, 15, W - 190), 15, r.mark == Mark::Need ? kNeed : kDim);
            ui.fill(M, y + rh - 1, W, 1, kLine);
            y += rh;
        }

        /* The last things said, the error first. */
        y += 10;
        if (!error.empty() && !busy) {
            ui.text(M, y, ui.fit(error, 15, W), 15, kNeed);
            y += 22;
        }
        for (const std::string& line : tail) {
            if (y > ui.h - 92) break;
            ui.text(M, y, ui.fit(line, 13, W), 13, kDim);
            y += 18;
        }

        /* Buttons, right to left: Set up / Play, Choose folder, Guide. */
        const float by = ui.h - 68, bw = 170, bh = 44, gap = 14;
        float bx = ui.w - M - bw;
        const std::string label = busy ? "Setting up..." : ready ? "Play" : "Set up";
        const bool enabled = !busy && (ready || !need);
        if (ui.button(bx, by, bw, bh, label, enabled, true) || (primary && enabled)) {
            if (ready) {
                play_now = true;
            } else {
                busy = true;
                started = SDL_GetTicks();
                const Source src = st.src;
                worker = std::thread([&t, src, &finished] {
                    setup_all(t, src);
                    finished = true;
                });
            }
        }
        if (!pick->cmd.empty()) {
            bx -= bw + gap;
            if (ui.button(bx, by, bw, bh, "Choose folder", !busy && !picking.joinable(), false)) {
                picking = std::thread([pick] {
                    std::string got;
                    pick->path.clear();
                    if (capture(pick->cmd, &got) == 0) {
                        while (!got.empty() && (got.back() == '\n' || got.back() == '\r')) got.pop_back();
                        pick->path = got;
                    }
                    pick->done = true;
                });
            }
        }
        bx -= bw + gap;
        if (ui.button(bx, by, bw, bh, "Guide", true, false)) open_guide();
        ui.text(M, by + 2, "Data folder", 13, kDim);
        ui.text(M, by + 22, ui.fit(pathstr(t.data), 13, bx - M - gap), 13, kText);

        SDL_RenderPresent(ui.ren);
        ui.mx = wmx;
        ui.my = wmy;
    }

    if (worker.joinable()) worker.join();
    if (picking.joinable()) picking.detach();
    for (auto& [k, g] : ui.glyphs) if (g.tex) SDL_DestroyTexture(g.tex);
    for (SDL_GameController* c : pads) SDL_GameControllerClose(c);
    SDL_DestroyRenderer(ui.ren);
    SDL_DestroyWindow(ui.win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
    return play_now ? 1 : 0;
}

void print_status(const Status& s)
{
    static const char* const marks[] = {"ok  ", "todo", "NEED"};
    for (const Row& r : checklist(s))
        printf("  [%s] %-11s %s\n", marks[(int)r.mark], r.title, r.detail.c_str());
}

}  // namespace

int main(int argc, char** argv)
{
    const Tree t = tree();
    if (!present(t.src / "src/main.cpp")) {
        fprintf(stderr, "tokyojungle-launcher: no Tokyo Jungle source tree at %s (set TJ_SOURCE)\n", pathstr(t.src).c_str());
        return 2;
    }
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "-h" || mode == "--help") {
        printf("usage: %s                    the launcher window\n"
               "       %s --status           what is done and what isn't\n"
               "       %s --setup [<folder>] everything not done yet, without a window\n"
               "       %s --play             start the game when it's ready\n"
               "data folder: %s ($TJ_DATA); source: %s ($TJ_SOURCE)\n",
               argv[0], argv[0], argv[0], argv[0], pathstr(t.data).c_str(), pathstr(t.src).c_str());
        return 0;
    }
    if (mode == "--status" || mode == "--setup") {
        Pointed pt;
        for (int i = 2; i < argc; i++) {
            std::string msg;
            locate(fs::absolute(argv[i]), &pt, &msg);
            if (!msg.empty()) printf("[launcher] %s\n", msg.c_str());
        }
        Status s = survey(t, pt);
        print_status(s);
        if (mode == "--status") return s.ready() ? 0 : 1;
        if (!s.imported && !s.src.complete()) {
            printf("[launcher] Do the RPCS3 steps first (docs/SETUP_GUIDE.md), then run this again.\n");
            return 1;
        }
        const bool ok = setup_all(t, s.src);
        print_status(survey(t, pt));
        return ok ? 0 : 1;
    }
    if (mode == "--play") {
        if (!survey(t, {}).ready()) {
            fprintf(stderr, "Not set up yet: run %s --setup\n", argv[0]);
            return 1;
        }
        play(t);
    }
    const int r = window(t);
    if (r < 0) {
        fprintf(stderr, "No window could be opened. Without one: %s --setup, then %s --play\n", argv[0], argv[0]);
        return 1;
    }
    if (r == 1) play(t);
    return 0;
}
