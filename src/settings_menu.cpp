/*
 * Tokyo Jungle runner -- settings and the in-game settings menu.
 * See settings_menu.h for what it is; the notes here are about how.
 *
 * Only what this port really has is offered:
 *   Display mode      windowed / borderless fullscreen / exclusive fullscreen
 *                     (SDL window flags; exclusive switches the display mode)
 *   Resolution        the window's size (windowed) or the display mode
 *                     (exclusive); borderless always uses the desktop's
 *   Internal res.     the Vulkan engine's render scale: every colour and
 *                     depth target num/den times the guest's 1280x720
 *                     (rsx_vulkan_backend_set_render_scale)
 *   Aspect ratio      stretch to the window, keep 16:9 (bars), or fill the
 *                     window at 16:9 (cropping what overhangs)
 *   VSync             FIFO present, or MAILBOX/IMMEDIATE
 *   Show FPS          the guest-flip counter in the corner (also F1)
 *
 * The menu is drawn on the CPU into a window-sized premultiplied RGBA image
 * (text with stb_truetype, which the runtime already links for cellFont, in
 * the PS3's own Rodin face from dev_flash when it is there) and handed to the
 * backend, which composites it over the frame. It is redrawn only when
 * something on it changes. Input arrives through an SDL event watch, which
 * SDL may call on whichever thread pumps or pushes events (cellPad updates
 * the controllers on a guest thread), so the watch only queues; everything
 * else happens in tj_settings_frame on the frame-clock thread.
 */
#include "settings_menu.h"

#include <stdlib.h>

#if defined(PS3RECOMP_RSX_VULKAN) && !defined(_WIN32) && !defined(__APPLE__) && \
    __has_include(<SDL2/SDL.h>)

#include <SDL2/SDL.h>
#include "stb_truetype.h"     /* implementation compiled into the runtime (cellFont.c) */

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits.h>
#include <map>
#include <mutex>
#include <set>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" {
void     rsx_vulkan_backend_set_ui(const uint32_t* rgba, uint32_t w, uint32_t h);
void     rsx_vulkan_backend_set_vsync(int on);
void     rsx_vulkan_backend_set_keep_aspect(int mode);   /* 0 stretch, 1 keep, 2 fill */
void     rsx_vulkan_backend_set_escape_quits(int on);
void*    rsx_vulkan_backend_sdl_window(void);
void     rsx_vulkan_backend_window_extent(uint32_t* w, uint32_t* h);
int      rsx_vulkan_backend_set_render_scale(uint32_t num, uint32_t den);
uint32_t rsx_vulkan_backend_max_image_dim(void);
void     cellPad_host_set_blocked(int blocked);
}

/* ---------------------------------------------------------------------------
 * Settings and settings.ini
 * -----------------------------------------------------------------------*/
enum { DM_WINDOWED = 0, DM_BORDERLESS = 1, DM_EXCLUSIVE = 2 };

struct Settings {
    int  display_mode = DM_WINDOWED;
    int  win_w = 1280, win_h = 720;   /* windowed size */
    int  fs_w = 0, fs_h = 0;          /* exclusive display mode; 0 = the desktop's */
    int  scale_pct = 100;             /* internal resolution; 0 = match the window */
    int  aspect = 0;                  /* 0 stretch, 1 keep 16:9 (bars), 2 fill (crop) */
    bool vsync = true;
    bool show_fps = false;
};

static Settings s_set;
static char     s_path[PATH_MAX];      /* settings.ini; empty = do not save */
static bool     s_have_window;

/* The guest renders 1280x720; the largest target Tokyo Jungle makes is its
 * 2560-wide bloom/DOF atlas, which bounds how far the scale can go. */
static const int k_guest_h = 720;
static const int k_widest_target = 2560;

static const struct { int pct; const char* label; } k_scales[] = {
    {   0, "Match window" },
    { 100, "720p (native)" },
    { 150, "1080p (1.5x)" },
    { 200, "1440p (2x)" },
    { 300, "2160p / 4K (3x)" },
    { 400, "2880p (4x)" },
};

static bool scale_allowed(int pct)
{
    const int p = pct ? pct : 400;
    return (uint64_t)k_widest_target * (uint64_t)p / 100u <= rsx_vulkan_backend_max_image_dim();
}

static void settings_save(void)
{
    if (!s_path[0]) return;
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_path);
    FILE* f = fopen(tmp, "w");
    if (!f) { fprintf(stderr, "[settings] cannot write %s\n", tmp); return; }
    static const char* const modes[] = { "windowed", "borderless", "exclusive" };
    fprintf(f, "# Tokyo Jungle runner settings (the in-game menu writes this file; Esc opens it)\n");
    fprintf(f, "display_mode = %s\n", modes[s_set.display_mode]);
    fprintf(f, "window_size = %dx%d\n", s_set.win_w, s_set.win_h);
    if (s_set.fs_w) fprintf(f, "fullscreen_mode = %dx%d\n", s_set.fs_w, s_set.fs_h);
    else            fprintf(f, "fullscreen_mode = desktop\n");
    if (s_set.scale_pct) fprintf(f, "internal_resolution = %d%%\n", s_set.scale_pct);
    else                 fprintf(f, "internal_resolution = window\n");
    static const char* const aspects[] = { "stretch", "keep", "fill" };
    fprintf(f, "aspect_ratio = %s\n", aspects[s_set.aspect]);
    fprintf(f, "vsync = %s\n", s_set.vsync ? "on" : "off");
    fprintf(f, "show_fps = %s\n", s_set.show_fps ? "on" : "off");
    fclose(f);
    rename(tmp, s_path);
}

static bool parse_on(const char* v) { return !strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true"); }

static void settings_parse(FILE* f)
{
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || !*p) continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char* k = p; char* v = eq + 1;
        for (char* e = eq - 1; e >= k && (*e == ' ' || *e == '\t'); e--) *e = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (char* e = v + strlen(v) - 1; e >= v && (*e == '\n' || *e == '\r' || *e == ' '); e--) *e = 0;
        int a, b;
        if (!strcmp(k, "display_mode")) {
            if (!strcmp(v, "windowed")) s_set.display_mode = DM_WINDOWED;
            else if (!strcmp(v, "borderless")) s_set.display_mode = DM_BORDERLESS;
            else if (!strcmp(v, "exclusive")) s_set.display_mode = DM_EXCLUSIVE;
        } else if (!strcmp(k, "window_size") && sscanf(v, "%dx%d", &a, &b) == 2 && a >= 320 && b >= 180) {
            s_set.win_w = a; s_set.win_h = b;
        } else if (!strcmp(k, "fullscreen_mode")) {
            if (sscanf(v, "%dx%d", &a, &b) == 2 && a >= 320 && b >= 180) { s_set.fs_w = a; s_set.fs_h = b; }
            else { s_set.fs_w = s_set.fs_h = 0; }
        } else if (!strcmp(k, "internal_resolution")) {
            if (!strcmp(v, "window")) s_set.scale_pct = 0;
            else if (sscanf(v, "%d", &a) == 1 && a >= 100 && a <= 800) s_set.scale_pct = a;
        } else if (!strcmp(k, "aspect_ratio")) {
            s_set.aspect = !strcmp(v, "keep") ? 1 : !strcmp(v, "fill") ? 2 : 0;
        } else if (!strcmp(k, "vsync")) {
            s_set.vsync = parse_on(v);
        } else if (!strcmp(k, "show_fps")) {
            s_set.show_fps = parse_on(v);
        }
    }
}

void tj_settings_load(void)
{
    const char* explicit_path = getenv("TJ_SETTINGS");
    const bool headless = getenv("TJ_HEADLESS") || !(getenv("WAYLAND_DISPLAY") || getenv("DISPLAY"));
    if (explicit_path && *explicit_path) {
        snprintf(s_path, sizeof s_path, "%s", explicit_path);
    } else if (!headless) {
        char exe[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = 0;
            char* s = strrchr(exe, '/');
            if (s) *s = 0;
            snprintf(s_path, sizeof s_path, "%s/settings.ini", exe);
        }
    }
    if (s_path[0]) {
        if (FILE* f = fopen(s_path, "r")) {
            settings_parse(f);
            fclose(f);
            printf("[settings] %s\n", s_path);
        }
    }
    /* Environment overrides, for scripted runs. */
    if (const char* e = getenv("TJ_SHOW_FPS"); e && *e) s_set.show_fps = *e != '0';
    if (const char* e = getenv("TJ_RENDER_SCALE"); e && *e) {
        const int p = atoi(e);
        if (p >= 100 && p <= 800) s_set.scale_pct = p;
    }
}

bool tj_settings_show_fps(void) { return s_set.show_fps; }

/* ---------------------------------------------------------------------------
 * Applying
 * -----------------------------------------------------------------------*/
static SDL_Window* window(void) { return (SDL_Window*)rsx_vulkan_backend_sdl_window(); }
static uint32_t s_applied_num = 1, s_applied_den = 1;

static void scale_of(int pct, uint32_t* num, uint32_t* den)
{
    if (pct) { *num = (uint32_t)pct; *den = 100; return; }
    uint32_t w = 0, h = 0;
    rsx_vulkan_backend_window_extent(&w, &h);
    if (!h) { *num = *den = 1; return; }
    /* Match the window: its height over the guest's, between 1x and 4x. */
    uint32_t n = h, d = (uint32_t)k_guest_h;
    if (n < d) n = d;
    if (n > 4 * d) n = 4 * d;
    *num = n; *den = d;
}

static void apply_scale(void)
{
    uint32_t n, d;
    scale_of(s_set.scale_pct, &n, &d);
    if ((uint64_t)n * s_applied_den == (uint64_t)s_applied_num * d) return;
    if (rsx_vulkan_backend_set_render_scale(n, d) == 0) { s_applied_num = n; s_applied_den = d; }
}

/* The space a window can use on its display (the desktop minus panels). */
static void usable_size(int* w, int* h)
{
    SDL_Window* win = window();
    const int disp = win ? std::max(0, SDL_GetWindowDisplayIndex(win)) : 0;
    SDL_Rect r;
    if (SDL_GetDisplayUsableBounds(disp, &r) == 0 && r.w > 0 && r.h > 0) { *w = r.w; *h = r.h; }
    else { *w = 1920; *h = 1080; }
}

static void desktop_size(int* w, int* h)
{
    SDL_DisplayMode m;
    SDL_Window* win = window();
    const int disp = win ? SDL_GetWindowDisplayIndex(win) : 0;
    if (SDL_GetDesktopDisplayMode(disp < 0 ? 0 : disp, &m) == 0) { *w = m.w; *h = m.h; }
    else { *w = 1920; *h = 1080; }
}

static void apply_display(void)
{
    SDL_Window* w = window();
    if (!w) return;
    const int disp = std::max(0, SDL_GetWindowDisplayIndex(w));
    switch (s_set.display_mode) {
    case DM_WINDOWED: {
        SDL_SetWindowFullscreen(w, 0);
        SDL_SetWindowBordered(w, SDL_TRUE);
        SDL_SetWindowResizable(w, SDL_TRUE);
        /* A size bigger than the usable area would be squeezed by the window
         * manager into another shape (1920x1080 became 1920x1008 under a
         * panel): shrink it to fit, keeping its shape, so the frame fills it. */
        int ww = s_set.win_w, wh = s_set.win_h, uw, uh;
        usable_size(&uw, &uh);
        int top = 0, left = 0, bottom = 0, right = 0;
        SDL_GetWindowBordersSize(w, &top, &left, &bottom, &right);
        uw -= left + right; uh -= top + bottom;
        if (ww > uw || wh > uh) {
            const double k = std::min((double)uw / ww, (double)uh / wh);
            ww = std::max(320, (int)(ww * k) & ~1); wh = std::max(180, (int)(wh * k) & ~1);
        }
        SDL_SetWindowSize(w, ww, wh);
        SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED_DISPLAY(disp), SDL_WINDOWPOS_CENTERED_DISPLAY(disp));
        break;
    }
    case DM_BORDERLESS:
        SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN_DESKTOP);
        break;
    case DM_EXCLUSIVE: {
        SDL_DisplayMode want, got;
        if (SDL_GetDesktopDisplayMode(disp, &want) != 0) memset(&want, 0, sizeof want);
        if (s_set.fs_w) { want.w = s_set.fs_w; want.h = s_set.fs_h; }
        if (!SDL_GetClosestDisplayMode(disp, &want, &got)) got = want;
        SDL_SetWindowFullscreen(w, 0);
        SDL_SetWindowDisplayMode(w, &got);
        SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN);
        break;
    }
    }
}

/* Sizes offered for the Resolution row: the display's own modes for
 * exclusive fullscreen; common 16:9 sizes that fit the desktop for a window
 * (plus the window's current size, if the user dragged it to another). */
static std::vector<std::pair<int, int>> resolutions(void)
{
    std::vector<std::pair<int, int>> out;
    SDL_Window* win = window();
    const int disp = win ? std::max(0, SDL_GetWindowDisplayIndex(win)) : 0;
    if (s_set.display_mode == DM_EXCLUSIVE) {
        const int n = SDL_GetNumDisplayModes(disp);
        for (int i = 0; i < n; i++) {
            SDL_DisplayMode m;
            if (SDL_GetDisplayMode(disp, i, &m) == 0 && m.w >= 640 && m.h >= 360)
                out.emplace_back(m.w, m.h);
        }
    } else {
        int dw, dh;
        usable_size(&dw, &dh);
        static const int common[][2] = { {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440},
                                         {3200, 1800}, {3840, 2160}, {5120, 2880} };
        for (auto& c : common)
            if (c[0] <= dw && c[1] <= dh) out.emplace_back(c[0], c[1]);
        if (out.empty()) out.emplace_back(1280, 720);
        out.emplace_back(s_set.win_w, s_set.win_h);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (out.empty()) out.emplace_back(1280, 720);
    return out;
}

/* ---------------------------------------------------------------------------
 * Fonts and drawing
 * -----------------------------------------------------------------------*/
struct Font {
    std::vector<unsigned char> data;
    stbtt_fontinfo info;
    bool ok = false;
    std::map<uint64_t, std::vector<unsigned char>> cache;   /* (px << 32 | cp) -> bitmap */
    std::map<uint64_t, std::array<int, 4>> meta;             /* w, h, xoff, yoff */
};

static Font s_regular, s_bold;

static bool font_load(Font& f, const char* path)
{
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    const long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0) { fclose(fp); return false; }
    f.data.resize((size_t)n);
    const size_t got = fread(f.data.data(), 1, (size_t)n, fp);
    fclose(fp);
    if (got != (size_t)n) return false;
    f.ok = stbtt_InitFont(&f.info, f.data.data(), stbtt_GetFontOffsetForIndex(f.data.data(), 0)) != 0;
    return f.ok;
}

static void fonts_load(void)
{
    static bool tried = false;
    if (tried) return;
    tried = true;
    /* The PS3's own UI face (Rodin) from dev_flash, then common Linux fonts. */
    std::vector<std::string> reg, bold;
    if (const char* fl = getenv("PS3_DEV_FLASH")) {
        reg.push_back(std::string(fl) + "/data/font/SCE-PS3-RD-R-LATIN.TTF");
        bold.push_back(std::string(fl) + "/data/font/SCE-PS3-RD-B-LATIN.TTF");
    }
    for (const char* p : { "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf",
                           "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" }) reg.push_back(p);
    for (const char* p : { "/usr/share/fonts/noto/NotoSans-Bold.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                           "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" }) bold.push_back(p);
    for (auto& p : reg)  if (font_load(s_regular, p.c_str())) { printf("[settings] menu font %s\n", p.c_str()); break; }
    for (auto& p : bold) if (font_load(s_bold, p.c_str())) break;
    if (!s_bold.ok && s_regular.ok) { s_bold.data = s_regular.data;
        s_bold.ok = stbtt_InitFont(&s_bold.info, s_bold.data.data(), 0) != 0; }
    if (!s_regular.ok) fprintf(stderr, "[settings] no font found for the menu; it will show no text\n");
}

struct Canvas {
    int w = 0, h = 0;
    std::vector<uint32_t> px;     /* premultiplied R,G,B,A bytes, little-endian u32 */
    void resize(int W, int H) { w = W; h = H; px.assign((size_t)W * H, 0u); }
    void clear() { std::fill(px.begin(), px.end(), 0u); }

    /* Source over, colour c (0xRRGGBB) at coverage a (0..1). */
    inline void blend(int x, int y, uint32_t c, float a)
    {
        if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h || a <= 0.0f) return;
        if (a > 1.0f) a = 1.0f;
        uint32_t& d = px[(size_t)y * w + x];
        const float sr = (float)((c >> 16) & 255) * a, sg = (float)((c >> 8) & 255) * a,
                    sb = (float)(c & 255) * a, sa = 255.0f * a;
        const float k = 1.0f - a;
        const float dr = (float)(d & 255), dg = (float)((d >> 8) & 255),
                    db = (float)((d >> 16) & 255), da = (float)(d >> 24);
        const uint32_t r = (uint32_t)(sr + dr * k + 0.5f), g = (uint32_t)(sg + dg * k + 0.5f),
                       b = (uint32_t)(sb + db * k + 0.5f), al = (uint32_t)(sa + da * k + 0.5f);
        d = r | (g << 8) | (b << 16) | (al << 24);
    }

    void fill(int x0, int y0, int ww, int hh, uint32_t c, float a)
    {
        for (int y = std::max(0, y0); y < std::min(h, y0 + hh); y++)
            for (int x = std::max(0, x0); x < std::min(w, x0 + ww); x++) blend(x, y, c, a);
    }

    /* Anti-aliased rounded rectangle. */
    void rrect(float x0, float y0, float ww, float hh, float r, uint32_t c, float a)
    {
        const int ix0 = (int)floorf(x0), iy0 = (int)floorf(y0);
        const int ix1 = (int)ceilf(x0 + ww), iy1 = (int)ceilf(y0 + hh);
        for (int y = std::max(0, iy0); y < std::min(h, iy1); y++)
            for (int x = std::max(0, ix0); x < std::min(w, ix1); x++) {
                const float px_ = x + 0.5f, py_ = y + 0.5f;
                const float cx = std::min(std::max(px_, x0 + r), x0 + ww - r);
                const float cy = std::min(std::max(py_, y0 + r), y0 + hh - r);
                const float dx = px_ - cx, dy = py_ - cy;
                const float dist = sqrtf(dx * dx + dy * dy) - r;
                float cov = 0.5f - dist;
                cov = std::min(1.0f, std::max(0.0f, cov));
                /* edges of the straight sides */
                cov *= std::min(1.0f, std::max(0.0f, std::min(px_ - x0, x0 + ww - px_) + 0.5f));
                cov *= std::min(1.0f, std::max(0.0f, std::min(py_ - y0, y0 + hh - py_) + 0.5f));
                blend(x, y, c, a * cov);
            }
    }

    /* A small solid triangle pointing left (dir < 0) or right, centred on
     * (cx, cy), 4x4 supersampled. */
    void arrow(float cx, float cy, float size, int dir, uint32_t c, float a)
    {
        const float hw = size * 0.5f, hh = size * 0.6f;
        for (int y = (int)(cy - hh) - 1; y <= (int)(cy + hh) + 1; y++)
            for (int x = (int)(cx - hw) - 1; x <= (int)(cx + hw) + 1; x++) {
                int in = 0;
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++) {
                        const float px_ = x + (sx + 0.5f) / 4.0f - cx;
                        const float py_ = y + (sy + 0.5f) / 4.0f - cy;
                        const float u = dir < 0 ? (hw - px_) : (px_ + hw);   /* 0 at the base */
                        if (u < 0 || u > 2 * hw) continue;
                        if (fabsf(py_) <= hh * (1.0f - u / (2 * hw))) in++;
                    }
                blend(x, y, c, a * (float)in / 16.0f);
            }
    }
};

static int glyph(Font& f, int px_size, int cp, const unsigned char** bmp, std::array<int, 4>* m)
{
    const uint64_t key = ((uint64_t)px_size << 32) | (uint32_t)cp;
    auto it = f.meta.find(key);
    if (it == f.meta.end()) {
        const float sc = stbtt_ScaleForPixelHeight(&f.info, (float)px_size);
        int w = 0, h = 0, xo = 0, yo = 0;
        unsigned char* b = stbtt_GetCodepointBitmap(&f.info, sc, sc, cp, &w, &h, &xo, &yo);
        std::vector<unsigned char> v;
        if (b && w > 0 && h > 0) v.assign(b, b + (size_t)w * h);
        if (b) stbtt_FreeBitmap(b, NULL);
        f.cache[key] = std::move(v);
        it = f.meta.emplace(key, std::array<int, 4>{ w, h, xo, yo }).first;
    }
    *m = it->second;
    *bmp = f.cache[key].data();
    return 0;
}

static float text_width(Font& f, int px_size, const std::string& s)
{
    if (!f.ok) return 0.0f;
    const float sc = stbtt_ScaleForPixelHeight(&f.info, (float)px_size);
    float x = 0.0f;
    for (size_t i = 0; i < s.size(); i++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&f.info, (unsigned char)s[i], &adv, &lsb);
        x += adv * sc;
        if (i + 1 < s.size()) x += sc * stbtt_GetCodepointKernAdvance(&f.info, (unsigned char)s[i], (unsigned char)s[i + 1]);
    }
    return x;
}

/* Text with its baseline at y; returns the width drawn. */
static float draw_text(Canvas& cv, Font& f, int px_size, float x, float y, const std::string& s,
                       uint32_t c, float a)
{
    if (!f.ok) return 0.0f;
    const float sc = stbtt_ScaleForPixelHeight(&f.info, (float)px_size);
    float pen = x;
    for (size_t i = 0; i < s.size(); i++) {
        const int cp = (unsigned char)s[i];
        const unsigned char* b; std::array<int, 4> m;
        glyph(f, px_size, cp, &b, &m);
        const int gx = (int)lroundf(pen) + m[2], gy = (int)lroundf(y) + m[3];
        for (int r = 0; r < m[1]; r++)
            for (int q = 0; q < m[0]; q++)
                if (const unsigned char v = b[(size_t)r * m[0] + q]) cv.blend(gx + q, gy + r, c, a * v / 255.0f);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&f.info, cp, &adv, &lsb);
        pen += adv * sc;
        if (i + 1 < s.size()) pen += sc * stbtt_GetCodepointKernAdvance(&f.info, cp, (unsigned char)s[i + 1]);
    }
    return pen - x;
}

/* ---------------------------------------------------------------------------
 * The menu
 * -----------------------------------------------------------------------*/
enum RowId { R_MODE, R_RES, R_SCALE, R_ASPECT, R_VSYNC, R_FPS, R_RESUME, R_QUIT };

struct Row {
    RowId id;
    const char* section;          /* non-null: a section header goes above it */
    std::string label, value;
    bool enabled;
    bool choice;                  /* has a value to step through */
};

static bool   s_open = false;
static int    s_sel = 0;
static bool   s_dirty = true;
static bool   s_quit = false;
static bool   s_unblock_pending = false;
static Canvas s_cv;

static std::string fmt_wh(int w, int h) { char b[32]; snprintf(b, sizeof b, "%d x %d", w, h); return b; }

static std::vector<Row> rows(void)
{
    static const char* const modes[] = { "Windowed", "Borderless fullscreen", "Exclusive fullscreen" };
    std::vector<Row> r;
    r.push_back({ R_MODE, "DISPLAY", "Display mode", modes[s_set.display_mode], s_have_window, true });
    std::string res;
    bool res_on = s_have_window;
    if (s_set.display_mode == DM_BORDERLESS) {
        int dw, dh; desktop_size(&dw, &dh);
        res = fmt_wh(dw, dh) + " (desktop)"; res_on = false;
    } else if (s_set.display_mode == DM_EXCLUSIVE) {
        if (s_set.fs_w) res = fmt_wh(s_set.fs_w, s_set.fs_h);
        else { int dw, dh; desktop_size(&dw, &dh); res = fmt_wh(dw, dh); }
    } else {
        res = fmt_wh(s_set.win_w, s_set.win_h);
    }
    r.push_back({ R_RES, nullptr, "Resolution", res, res_on, true });
    std::string sc = "?";
    for (auto& k : k_scales) if (k.pct == s_set.scale_pct) sc = k.label;
    if (s_set.scale_pct == 0) {
        uint32_t n, d; scale_of(0, &n, &d);
        char b[48]; snprintf(b, sizeof b, "Match window (%up)", (unsigned)(k_guest_h * n / d));
        sc = b;
    } else if (sc == "?") {
        char b[32]; snprintf(b, sizeof b, "%d%%", s_set.scale_pct); sc = b;
    }
    r.push_back({ R_SCALE, "GRAPHICS", "Internal resolution", sc, true, true });
    static const char* const aspects[] = { "Stretch to window", "Keep 16:9 (bars)", "Fill window (crop)" };
    r.push_back({ R_ASPECT, nullptr, "Aspect ratio", aspects[s_set.aspect], s_have_window, true });
    r.push_back({ R_VSYNC, nullptr, "VSync", s_set.vsync ? "On" : "Off", s_have_window, true });
    r.push_back({ R_FPS, "INTERFACE", "Show FPS (F1)", s_set.show_fps ? "On" : "Off", s_have_window, true });
    r.push_back({ R_RESUME, "", "Resume", "", true, false });
    r.push_back({ R_QUIT, nullptr, "Quit game", "", true, false });
    return r;
}

template <typename T> static int step_index(const std::vector<T>& v, int cur, int dir)
{
    const int n = (int)v.size();
    return ((cur + dir) % n + n) % n;
}

static void change(RowId id, int dir)
{
    switch (id) {
    case R_MODE:
        s_set.display_mode = ((s_set.display_mode + dir) % 3 + 3) % 3;
        apply_display();
        break;
    case R_RES: {
        auto list = resolutions();
        int cur = -1;
        const int cw = s_set.display_mode == DM_EXCLUSIVE ? s_set.fs_w : s_set.win_w;
        const int ch = s_set.display_mode == DM_EXCLUSIVE ? s_set.fs_h : s_set.win_h;
        for (int i = 0; i < (int)list.size(); i++) if (list[i].first == cw && list[i].second == ch) cur = i;
        if (cur < 0) {                      /* desktop default: start from the largest */
            cur = dir > 0 ? -1 : (int)list.size();
        }
        int nx = cur + dir;
        if (nx < 0) nx = (int)list.size() - 1;
        if (nx >= (int)list.size()) nx = 0;
        if (s_set.display_mode == DM_EXCLUSIVE) { s_set.fs_w = list[nx].first; s_set.fs_h = list[nx].second; }
        else { s_set.win_w = list[nx].first; s_set.win_h = list[nx].second; }
        apply_display();
        break;
    }
    case R_SCALE: {
        std::vector<int> opts;
        for (auto& k : k_scales) if (scale_allowed(k.pct)) opts.push_back(k.pct);
        int cur = 0;
        for (int i = 0; i < (int)opts.size(); i++) if (opts[i] == s_set.scale_pct) cur = i;
        s_set.scale_pct = opts[step_index(opts, cur, dir)];
        apply_scale();
        break;
    }
    case R_ASPECT:
        s_set.aspect = ((s_set.aspect + dir) % 3 + 3) % 3;
        rsx_vulkan_backend_set_keep_aspect(s_set.aspect);
        break;
    case R_VSYNC:
        s_set.vsync = !s_set.vsync;
        rsx_vulkan_backend_set_vsync(s_set.vsync);
        break;
    case R_FPS:
        s_set.show_fps = !s_set.show_fps;
        break;
    default:
        return;
    }
    settings_save();
    s_dirty = true;
}

static void set_open(bool open)
{
    if (open == s_open) return;
    s_open = open;
    s_dirty = true;
    if (open) {
        s_sel = 0;
        s_unblock_pending = false;
        cellPad_host_set_blocked(1);
    } else {
        rsx_vulkan_backend_set_ui(NULL, 0, 0);
        s_unblock_pending = true;          /* once the buttons that closed it are up */
    }
}

static void activate(void)
{
    auto r = rows();
    if (s_sel < 0 || s_sel >= (int)r.size()) return;
    switch (r[s_sel].id) {
    case R_RESUME: set_open(false); break;
    case R_QUIT:   s_quit = true; set_open(false); break;
    default:       if (r[s_sel].enabled) change(r[s_sel].id, +1); break;
    }
}

static void move(int dir)
{
    auto r = rows();
    const int n = (int)r.size();
    for (int k = 0; k < n; k++) {
        s_sel = ((s_sel + dir) % n + n) % n;
        if (r[s_sel].enabled) break;
    }
    s_dirty = true;
}

static void draw(int W, int H)
{
    fonts_load();
    if (s_cv.w != W || s_cv.h != H) s_cv.resize(W, H);
    else s_cv.clear();
    Canvas& cv = s_cv;
    const float u = std::max(0.55f, (float)H / 1080.0f);

    const uint32_t panel = 0x15171C, text = 0xF2F2F2, dim = 0x8A8F99, accent = 0xF2A33A, off = 0x5A5E66;
    cv.fill(0, 0, W, H, 0x000000, 0.55f);                     /* the game, dimmed */

    auto r = rows();
    const float pw = std::min((float)W - 40.0f, 860.0f * u);
    const float row_h = 54.0f * u, sec_h = 44.0f * u, head_h = 118.0f * u, foot_h = 64.0f * u;
    float ph = head_h + foot_h;
    for (auto& x : r) ph += row_h + (x.section ? sec_h : 0.0f);
    const float px = ((float)W - pw) * 0.5f, py = std::max(20.0f, ((float)H - ph) * 0.5f);
    cv.rrect(px, py, pw, ph, 18.0f * u, panel, 0.96f);
    cv.rrect(px, py, pw, 5.0f * u, 2.5f * u, accent, 1.0f);

    const float pad = 40.0f * u;
    draw_text(cv, s_bold, (int)(46 * u), px + pad, py + 66 * u, "Settings", text, 1.0f);
    draw_text(cv, s_regular, (int)(22 * u), px + pad, py + 98 * u, "Tokyo Jungle  -  recompiled", dim, 1.0f);

    float y = py + head_h;
    for (int i = 0; i < (int)r.size(); i++) {
        const Row& row = r[i];
        if (row.section) {
            if (*row.section)
                draw_text(cv, s_bold, (int)(19 * u), px + pad, y + 32 * u, row.section, accent, 1.0f);
            else
                cv.fill((int)(px + pad), (int)(y + 22 * u), (int)(pw - 2 * pad), std::max(1, (int)(1.5f * u)), dim, 0.35f);
            y += sec_h;
        }
        const bool sel = i == s_sel;
        if (sel) {
            cv.rrect(px + 16 * u, y + 2 * u, pw - 32 * u, row_h - 4 * u, 10 * u, 0xFFFFFF, 0.08f);
            cv.rrect(px + 16 * u, y + 10 * u, 5 * u, row_h - 20 * u, 2.5f * u, accent, 1.0f);
        }
        const uint32_t lc = row.enabled ? text : off;
        const float base = y + row_h * 0.5f + 9 * u;
        draw_text(cv, s_regular, (int)(27 * u), px + pad, base, row.label, lc, 1.0f);
        if (row.choice) {
            const float vw = text_width(s_regular, (int)(25 * u), row.value);
            const float right = px + pw - pad;
            const uint32_t vc = row.enabled ? (sel ? text : 0xC8CCD3) : off;
            const float ax = 9 * u;
            if (row.enabled && sel) {
                cv.arrow(right - 6 * u, y + row_h * 0.5f, ax, +1, accent, 1.0f);
                cv.arrow(right - vw - 34 * u, y + row_h * 0.5f, ax, -1, accent, 1.0f);
            }
            draw_text(cv, s_regular, (int)(25 * u), right - vw - 20 * u, base, row.value, vc, 1.0f);
        }
        y += row_h;
    }
    /* The key hints, shrunk to fit the panel when it is narrow. */
    static const char* const hint =
        "Up / Down: choose     Left / Right: change     Enter / A: select     Esc / B: close";
    int hs = (int)(19 * u);
    const float avail = pw - 2 * pad;
    const float hw = text_width(s_regular, hs, hint);
    if (hw > avail && hw > 0) hs = std::max(8, (int)(hs * avail / hw));
    draw_text(cv, s_regular, hs, px + pad, py + ph - 26 * u, hint, dim, 1.0f);
    rsx_vulkan_backend_set_ui(cv.px.data(), (uint32_t)W, (uint32_t)H);
}

/* ---------------------------------------------------------------------------
 * Input: queued by the watch, handled on the frame clock
 * -----------------------------------------------------------------------*/
enum { EV_KEY, EV_PAD_DOWN, EV_PAD_UP, EV_RESIZE };
struct Ev { int kind, code; };
static std::mutex       s_q_mu;
static std::vector<Ev>  s_q;
static std::set<int>    s_pad_down;    /* (instance << 8 | button) held, frame-clock side */

static int SDLCALL watch(void*, SDL_Event* e)
{
    Ev ev{ -1, 0 };
    switch (e->type) {
    case SDL_KEYDOWN:             ev = { EV_KEY, (int)e->key.keysym.sym }; break;
    case SDL_CONTROLLERBUTTONDOWN: ev = { EV_PAD_DOWN, (int)(e->cbutton.which << 8 | e->cbutton.button) }; break;
    case SDL_CONTROLLERBUTTONUP:   ev = { EV_PAD_UP,   (int)(e->cbutton.which << 8 | e->cbutton.button) }; break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) ev = { EV_RESIZE, 0 };
        break;
    default: break;
    }
    if (ev.kind >= 0) { std::lock_guard<std::mutex> g(s_q_mu); s_q.push_back(ev); }
    return 1;
}

static bool pad_held(int button)
{
    for (int k : s_pad_down) if ((k & 0xFF) == button) return true;
    return false;
}

static void handle(const Ev& ev)
{
    if (ev.kind == EV_RESIZE) {
        s_dirty = true;
        SDL_Window* w = window();
        if (w && s_set.display_mode == DM_WINDOWED && !(SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN)) {
            int ww, hh;
            SDL_GetWindowSize(w, &ww, &hh);
            if (ww != s_set.win_w || hh != s_set.win_h) { s_set.win_w = ww; s_set.win_h = hh; settings_save(); }
        }
        if (s_set.scale_pct == 0) apply_scale();
        return;
    }
    if (ev.kind == EV_PAD_UP) { s_pad_down.erase(ev.code); return; }
    if (ev.kind == EV_PAD_DOWN) {
        s_pad_down.insert(ev.code);
        const int b = ev.code & 0xFF;
        if (b == SDL_CONTROLLER_BUTTON_GUIDE ||
            ((b == SDL_CONTROLLER_BUTTON_START && pad_held(SDL_CONTROLLER_BUTTON_BACK)) ||
             (b == SDL_CONTROLLER_BUTTON_BACK && pad_held(SDL_CONTROLLER_BUTTON_START)))) {
            set_open(!s_open);
            return;
        }
        if (!s_open) return;
        switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    move(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  move(+1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  { auto r = rows(); if (r[s_sel].enabled) change(r[s_sel].id, -1); break; }
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: { auto r = rows(); if (r[s_sel].enabled) change(r[s_sel].id, +1); break; }
        case SDL_CONTROLLER_BUTTON_A:          activate(); break;
        case SDL_CONTROLLER_BUTTON_B:          set_open(false); break;
        default: break;
        }
        return;
    }
    /* keys */
    const int k = ev.code;
    if (k == SDLK_F1) { s_set.show_fps = !s_set.show_fps; settings_save(); s_dirty = true; return; }
    if (k == SDLK_ESCAPE) { set_open(!s_open); return; }
    if (!s_open) return;
    switch (k) {
    case SDLK_UP:        move(-1); break;
    case SDLK_DOWN:      move(+1); break;
    case SDLK_LEFT:      { auto r = rows(); if (r[s_sel].enabled) change(r[s_sel].id, -1); break; }
    case SDLK_RIGHT:     { auto r = rows(); if (r[s_sel].enabled) change(r[s_sel].id, +1); break; }
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: activate(); break;
    case SDLK_BACKSPACE: set_open(false); break;
    default: break;
    }
}

void tj_settings_backend_ready(void)
{
    s_have_window = window() != nullptr;
    rsx_vulkan_backend_set_keep_aspect(s_set.aspect);
    rsx_vulkan_backend_set_vsync(s_set.vsync);
    if (s_have_window) {
        rsx_vulkan_backend_set_escape_quits(0);   /* Escape opens the menu; Quit is in it */
        apply_display();
        SDL_AddEventWatch(watch, NULL);
    }
    apply_scale();
    if (s_set.scale_pct != 100)
        printf("[settings] internal resolution %s\n", s_set.scale_pct ? "scaled" : "matches the window");
}

void tj_settings_frame(void)
{
    if (!s_have_window) return;
    /* TJ_MENU_OPEN_AFTER_S=<s>: open the menu once, s seconds in (scripted
     * screenshots of it); TJ_MENU_SELECT=<row> moves to that row then. */
    static const char* open_after = getenv("TJ_MENU_OPEN_AFTER_S");
    if (open_after && *open_after) {
        static const auto t0 = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(atoi(open_after))) {
            open_after = nullptr;
            set_open(true);
            if (const char* sel = getenv("TJ_MENU_SELECT")) s_sel = atoi(sel);
        }
    }
    std::vector<Ev> q;
    { std::lock_guard<std::mutex> g(s_q_mu); q.swap(s_q); }
    for (const Ev& e : q) handle(e);
    if (s_unblock_pending && s_pad_down.empty()) {
        s_unblock_pending = false;
        cellPad_host_set_blocked(0);
    }
    if (!s_open) return;
    uint32_t w = 0, h = 0;
    rsx_vulkan_backend_window_extent(&w, &h);
    if (!w || !h) return;
    if (s_dirty || (int)w != s_cv.w || (int)h != s_cv.h) {
        draw((int)w, (int)h);
        s_dirty = false;
    }
}

bool tj_settings_quit_requested(void) { return s_quit; }

#else   /* no Vulkan window: settings are the defaults, there is no menu */

void tj_settings_load(void) {}
void tj_settings_backend_ready(void) {}
void tj_settings_frame(void) {}
bool tj_settings_show_fps(void) { const char* e = getenv("TJ_SHOW_FPS"); return e && *e && *e != '0'; }
bool tj_settings_quit_requested(void) { return false; }

#endif
