/*
 * Tokyo Jungle -- cellFont / cellFontFT, reimplemented natively (HLE) with
 * stb_truetype.
 *
 * The game draws all of its text through cellFont: its PFontThread renders
 * strings glyph by glyph into CPU-side "name sheets" that it then uploads as
 * textures (the pause map's area tags come from one, a 368x832 sheet with one
 * name per 32-pixel row). Until 2026-10-21 the firmware's own libfont,
 * libfontFT and libfreetype ran here statically lifted (docs/FONT_LLE_PLAN.md),
 * and some of those rows came out empty: three of the nine map tags had a bar
 * and no name. The other Tokyo Jungle port (/opt/git/tokyojungle) answers the
 * same 31 imports with an stb_truetype cellFont and its tags are complete, so
 * this port does the same.
 *
 * The faces are the console's own: /dev_flash/data/font/SCE-PS3-*.TTF from the
 * installed system software, read through the VFS, so the text looks the
 * same as on a PS3. A fontset is a chain of them (Latin, Japanese, Korean, as
 * the firmware opens for the game); a glyph comes from the first face that has
 * it. Layout, metrics, slant and bold follow the lifted libfont, checked
 * against it with TJ_FONT_ORACLE=1.
 *
 * The lifted libfont was removed on 2026-10-22 (no firmware code in the port,
 * user decision); TJ_FONT_ORACLE logs are what it was matched against.
 * TJ_FONT_LOG=1 logs every call. TJ_FONT_ORACLE=1 logs a few glyphs' metrics
 * and images.
 *
 * Guest ABI notes. Floating-point arguments still take a GPR slot (64-bit
 * PowerPC ELF), so cellFontRenderCharGlyphImage(font, code, surface, x, y,
 * metrics, transInfo) has x/y in f1/f2 and metrics/transInfo in r8/r9. The
 * game's CellFont is its own (larger) object; only its first 16 bytes are used
 * here: handle, type, scale x, scale y.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "ppu_context.h"
#include "stb_truetype.h"     /* implementation compiled into the runtime (cellFont.c) */

extern "C" uint8_t* vm_base;
extern "C" uint32_t vm_read32(uint64_t addr);
extern "C" void vm_write32(uint64_t addr, uint32_t v);
extern "C" int64_t sys_memory_allocate(ppu_context* ctx);
extern "C" int cellfs_translate_path(const char* ps3_path, char* host_buf, size_t buf_size);
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" int cellGcm_rsx_wait_consumed(unsigned timeout_ms);
extern "C" uint32_t cellGcm_flip_request_count(void);
extern "C" unsigned long long ps3_ms_now(void);
extern "C" int32_t _cellGcmSetFlipCommand(void* ctx, uint32_t bufferId);
extern "C" int32_t _cellGcmSetFlipCommandWithWaitLabel(void* ctx, uint32_t bufferId,
                                                    uint32_t labelIndex, uint32_t labelValue);

namespace {

constexpr int32_t CELL_OK                         = 0;
constexpr int32_t CELL_FONT_ERROR_INVALID_PARAMETER = (int32_t)0x80540002;
constexpr int32_t CELL_FONT_ERROR_UNINITIALIZED   = (int32_t)0x80540003;
constexpr int32_t CELL_FONT_ERROR_ALLOCATION_FAILED = (int32_t)0x80540005;
constexpr int32_t CELL_FONT_ERROR_OPEN_FAILED     = (int32_t)0x80540007;
constexpr int32_t CELL_FONT_ERROR_RENDERER_ALREADY_BIND = (int32_t)0x80540011;

constexpr uint32_t kMaxFonts     = 32;
constexpr uint32_t kMaxRenderers = 8;
constexpr uint32_t kUnbound      = 0xFFFFFFFFu;
/* Surfaces another thread may start between two flips (FontRenderSurfaceInit). */
constexpr uint32_t kNamesPerFlip = 3;

/* Guest memory this module hands the game: the library object, one glyph
 * image per open font (valid until that font renders its next glyph, as with
 * the firmware), and the objects GenerateCharGlyph returns. One 2 MB block
 * from sys_memory, so it never lands in the game's own heap. */
constexpr uint32_t kArenaSize     = 0x200000;
constexpr uint32_t kLibraryOff    = 0x0;          /* 0x100 bytes */
constexpr uint32_t kImageOff      = 0x1000;
constexpr uint32_t kImageCap      = 0x8000;       /* 32 KB per font: 181x181 */
constexpr uint32_t kGlyphOff      = kImageOff + kMaxFonts * kImageCap;
constexpr uint32_t kGlyphSize     = 0x80;
constexpr uint32_t kGlyphCount    = 1024;

/* The fontset chain, in lookup order: what the firmware opens for the game's
 * fontset (type 0x201). */
const char* const kFaceFiles[] = {
    "SCE-PS3-RD-R-LATIN2.TTF",
    "SCE-PS3-NR-R-JPN.TTF",
    "SCE-PS3-YG-R-KOR.TTF",
};

/* Bold: the firmware grows a glyph's outline by `strength` pixels in width
 * and height (half on each side) and its advance by the same, as FreeType's
 * outline emboldening does. strength = (weight - 1) * em * kBoldPerEm, fitted
 * to the lifted libfont: 39/64 px at weight 1.5, 16 px (TJ_FONT_ORACLE). */
constexpr float kBoldPerEm = 0.0762f;
/* The ink grows by less than the metrics say: dilating by 0.75 of half the
 * strength matches the firmware's images to within 2/9 coverage a pixel. */
constexpr float kBoldInk = 0.75f;

struct Face {
    std::vector<uint8_t> data;
    stbtt_fontinfo info;
    int ascent, descent, gap;   /* font units */
    float units_per_em;
};

/* The fontset's vertical extent, in em: the union of its faces' bounding
 * boxes (head yMax/yMin). The firmware's line layout comes from it. */
float g_set_ymax, g_set_ymin;

struct FontSlot {
    bool     in_use;
    uint32_t type;
    float    px_w, px_h;        /* em size in pixels */
    uint32_t dpi_h, dpi_v;
    float    weight, slant;
    uint32_t renderer;
};

std::mutex g_mutex;
bool g_initialized;
bool g_log;
std::vector<Face> g_faces;
bool g_faces_tried;
FontSlot g_fonts[kMaxFonts];
bool g_renderers[kMaxRenderers];
uint32_t g_arena;
bool g_glyph_used[kGlyphCount];
/* The guest thread that issues the flips (FlipCommand*). */
std::atomic<uint64_t> g_flip_tid{~0ull};

#define FLOG(...) do { if (g_log) { printf("[font] " __VA_ARGS__); fflush(stdout); } } while (0)

uint32_t u32_arg(ppu_context* ctx, int r) { return (uint32_t)ctx->gpr[r]; }
void ret(ppu_context* ctx, int32_t rc) { ctx->gpr[3] = (uint64_t)(int64_t)rc; }


void write_float(uint32_t ea, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, 4);
    vm_write32(ea, bits);
}

/* ---- faces ---------------------------------------------------------------- */

bool load_face(const char* name, Face& face)
{
    char guest[256], host[1024];
    snprintf(guest, sizeof guest, "/dev_flash/data/font/%s", name);
    if (cellfs_translate_path(guest, host, sizeof host) != 0) return false;
    FILE* f = fopen(host, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return false; }
    face.data.resize((size_t)size);
    bool ok = fread(face.data.data(), 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok) return false;
    int off = stbtt_GetFontOffsetForIndex(face.data.data(), 0);
    if (off < 0 || !stbtt_InitFont(&face.info, face.data.data(), off)) return false;
    stbtt_GetFontVMetrics(&face.info, &face.ascent, &face.descent, &face.gap);
    face.units_per_em = 1.0f / stbtt_ScaleForMappingEmToPixels(&face.info, 1.0f);
    int x0, y0, x1, y1;
    stbtt_GetFontBoundingBox(&face.info, &x0, &y0, &x1, &y1);
    g_set_ymax = std::max(g_set_ymax, (float)y1 / face.units_per_em);
    g_set_ymin = std::min(g_set_ymin, (float)y0 / face.units_per_em);
    return true;
}

bool load_faces()
{
    if (g_faces_tried) return !g_faces.empty();
    g_faces_tried = true;
    g_faces.reserve(sizeof kFaceFiles / sizeof kFaceFiles[0]);
    for (const char* name : kFaceFiles) {
        g_faces.emplace_back();
        if (load_face(name, g_faces.back())) {
            printf("[font] HLE cellFont: %s\n", name);
        } else {
            g_faces.pop_back();
            fprintf(stderr, "[font] HLE cellFont: /dev_flash/data/font/%s not found\n", name);
        }
    }
    if (g_faces.empty())
        fprintf(stderr, "[font] HLE cellFont: no system fonts, so no text. Install the PS3 "
                        "system software from the dashboard (Settings > System Update) or "
                        "set PS3_DEV_FLASH\n");
    return !g_faces.empty();
}

/* The face that draws `code`, and its glyph index (0 = none has it). */
const Face* face_for(uint32_t code, int* glyph)
{
    for (const Face& f : g_faces) {
        int g = stbtt_FindGlyphIndex(&f.info, (int)code);
        if (g) { *glyph = g; return &f; }
    }
    *glyph = 0;
    return g_faces.empty() ? nullptr : &g_faces[0];
}

/* SetScalePixel is the em size, as FreeType's FT_Set_Pixel_Sizes. */
float scale_x(const Face& f, const FontSlot& s) { return s.px_w / f.units_per_em; }
float scale_y(const Face& f, const FontSlot& s) { return s.px_h / f.units_per_em; }

float bold_strength(const FontSlot& s)
{
    return s.weight > 1.0f ? (s.weight - 1.0f) * s.px_h * kBoldPerEm : 0.0f;
}

/* The line's layout, in FreeType's 26.6 fixed point as the firmware has it:
 * baseline below the line top, line height, and the bold growth above. */
struct Layout { float baseline, line_height, effect; };

Layout line_layout(const FontSlot& s)
{
    const float half = bold_strength(s) * 0.5f * 64.0f;
    const float asc = roundf(g_set_ymax * s.px_h * 64.0f);
    const float desc = ceilf(-g_set_ymin * s.px_h * 64.0f);
    return {(asc + ceilf(half)) / 64.0f, (asc + desc + 2.0f * ceilf(half)) / 64.0f,
            floorf(half) / 64.0f};
}

/* A character's outline, slanted (x += slant * y, about the baseline) as the
 * firmware renders it. Two boxes, in font units: the slanted outline's control
 * box (what the image must hold) and the one the firmware reports, the
 * upright outline's box with its corners slanted (TJ_FONT_ORACLE). */
struct Glyph {
    const Face* face = nullptr;
    int index = 0;
    stbtt_vertex* v = nullptr;
    int n = 0;
    int adv = 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   /* reported */
    float ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;   /* ink */
    ~Glyph() { if (v) stbtt_FreeShape(&face->info, v); }
};

bool load_glyph(const FontSlot& s, uint32_t code, Glyph& g)
{
    g.face = face_for(code, &g.index);
    if (!g.face) return false;
    int lsb;
    stbtt_GetGlyphHMetrics(&g.face->info, g.index, &g.adv, &lsb);
    if (!g.index) return true;   /* in no face: an advance and nothing to draw */
    g.n = stbtt_GetGlyphShape(&g.face->info, g.index, &g.v);
    auto shear = [&](short& x, short y) { x = (short)lroundf((float)x + s.slant * (float)y); };
    struct Box {
        bool empty = true;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        void add(float x, float y) {
            if (empty) { x0 = x1 = x; y0 = y1 = y; empty = false; return; }
            x0 = std::min(x0, x); x1 = std::max(x1, x);
            y0 = std::min(y0, y); y1 = std::max(y1, y);
        }
        void add(const stbtt_vertex& p) {
            add(p.x, p.y);
            if (p.type == STBTT_vcurve || p.type == STBTT_vcubic) add(p.cx, p.cy);
            if (p.type == STBTT_vcubic) add(p.cx1, p.cy1);
        }
    } upright, ink;
    for (int i = 0; i < g.n; ++i) {
        stbtt_vertex& p = g.v[i];
        upright.add(p);
        if (s.slant != 0.0f) { shear(p.x, p.y); shear(p.cx, p.cy); shear(p.cx1, p.cy1); }
        ink.add(p);
    }
    if (upright.empty) { g.n = 0; return true; }   /* no outline: a space */
    g.x0 = upright.x0 + s.slant * (s.slant > 0 ? upright.y0 : upright.y1);
    g.x1 = upright.x1 + s.slant * (s.slant > 0 ? upright.y1 : upright.y0);
    g.y0 = upright.y0;
    g.y1 = upright.y1;
    g.ix0 = ink.x0; g.iy0 = ink.y0; g.ix1 = ink.x1; g.iy1 = ink.y1;
    return true;
}

/* ---- guest objects ------------------------------------------------------- */

bool ensure_arena(ppu_context* ctx)
{
    if (g_arena) return true;
    /* sys_memory_allocate writes the address to a guest word: borrow one
     * below the caller's stack pointer, which nothing live occupies. */
    const uint32_t out = (uint32_t)ctx->gpr[1] - 16;
    ppu_context call = *ctx;
    call.gpr[3] = kArenaSize;
    call.gpr[4] = 0x400;   /* SYS_MEMORY_PAGE_SIZE_1M */
    call.gpr[5] = out;
    if (sys_memory_allocate(&call) != CELL_OK) return false;
    g_arena = vm_read32(out);
    memset(vm_base + g_arena, 0, kArenaSize);
    return true;
}

FontSlot* slot_at(uint32_t font_ea)
{
    if (!font_ea) return nullptr;
    uint32_t h = vm_read32(font_ea);
    if (h >= kMaxFonts || !g_fonts[h].in_use) return nullptr;
    return &g_fonts[h];
}

void write_font(uint32_t font_ea, const FontSlot& s)
{
    vm_write32(font_ea + 0, (uint32_t)(&s - g_fonts));
    vm_write32(font_ea + 4, s.type);
    write_float(font_ea + 8, s.px_w);
    write_float(font_ea + 12, s.px_h);
}

FontSlot* alloc_slot()
{
    for (FontSlot& s : g_fonts) {
        if (!s.in_use) {
            s = FontSlot{};
            s.in_use = true;
            s.px_w = s.px_h = 1.0f;
            s.dpi_h = s.dpi_v = 72;
            s.weight = 1.0f;
            s.renderer = kUnbound;
            return &s;
        }
    }
    return nullptr;
}

struct Metrics { float w, h, hbx, hby, hadv, vbx, vby, vadv; };

/* The firmware's metrics are the slanted, emboldened outline's control box,
 * unrounded: width, height, horizontal bearing x/y (left and top from the
 * pen on the baseline) and advance, then the vertical set. */
Metrics glyph_metrics(const FontSlot& s, const Glyph& g)
{
    Metrics m{};
    if (!g.face) return m;
    const float sx = scale_x(*g.face, s), sy = scale_y(*g.face, s), st = bold_strength(s);
    m.hadv = (float)g.adv * sx + st;
    m.vadv = (float)(g.face->ascent - g.face->descent) * sy + st;
    if (!g.n) return m;
    m.w = (g.x1 - g.x0) * sx + st;
    m.h = (g.y1 - g.y0) * sy + st;
    m.hbx = g.x0 * sx - st * 0.5f;
    m.hby = g.y1 * sy + st * 0.5f;
    m.vbx = -m.w * 0.5f;
    m.vby = line_layout(s).baseline - m.hby;
    return m;
}

Metrics glyph_metrics(const FontSlot& s, uint32_t code)
{
    Glyph g;
    load_glyph(s, code, g);
    return glyph_metrics(s, g);
}

void write_metrics(uint32_t ea, const Metrics& m)
{
    const float v[8] = {m.w, m.h, m.hbx, m.hby, m.hadv, m.vbx, m.vby, m.vadv};
    for (int i = 0; i < 8; ++i) write_float(ea + 4 * i, v[i]);
}

/* ---- the imports --------------------------------------------------------- */

/* cellFontInitializeWithRevision(revisionFlags, config) */
void FontInitializeWithRevision(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    FLOG("InitializeWithRevision\n");
    g_log = getenv("TJ_FONT_LOG") != nullptr;
    for (FontSlot& s : g_fonts) s = FontSlot{};
    for (bool& r : g_renderers) r = false;
    g_initialized = true;
    load_faces();
    ret(ctx, CELL_OK);
}

/* cellFontEnd() */
void FontEnd(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    FLOG("End\n");
    if (!g_initialized) return ret(ctx, CELL_FONT_ERROR_UNINITIALIZED);
    for (FontSlot& s : g_fonts) s.in_use = false;
    g_initialized = false;
    ret(ctx, CELL_OK);
}

/* cellFontInitLibraryFreeTypeWithRevision(revisionFlags, config, lib**) */
void FontInitLibraryFreeType(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t lib_out = u32_arg(ctx, 5);
    if (!lib_out) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    if (!ensure_arena(ctx)) return ret(ctx, CELL_FONT_ERROR_ALLOCATION_FAILED);
    vm_write32(lib_out, g_arena + kLibraryOff);
    FLOG("InitLibraryFreeType -> 0x%08X\n", g_arena + kLibraryOff);
    ret(ctx, CELL_OK);
}

/* cellFontEndLibrary(lib) */
void FontEndLibrary(ppu_context* ctx)
{
    ret(ctx, u32_arg(ctx, 3) ? CELL_OK : CELL_FONT_ERROR_INVALID_PARAMETER);
}

/* cellFontOpenFontset(lib, fontType*, font*), and ...OnMemory, same ABI. */
void FontOpenFontset(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t type_ea = u32_arg(ctx, 4), font_ea = u32_arg(ctx, 5);
    if (!g_initialized) return ret(ctx, CELL_FONT_ERROR_UNINITIALIZED);
    if (!type_ea || !font_ea) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    if (!load_faces()) return ret(ctx, CELL_FONT_ERROR_OPEN_FAILED);
    FontSlot* s = alloc_slot();
    if (!s) return ret(ctx, CELL_FONT_ERROR_ALLOCATION_FAILED);
    s->type = vm_read32(type_ea);
    write_font(font_ea, *s);
    printf("[font] OpenFontset(type=0x%X) -> font %u\n", s->type, (unsigned)(s - g_fonts));
    ret(ctx, CELL_OK);
}

/* cellFontOpenFontInstance(source*, font*) */
void FontOpenFontInstance(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    FontSlot* src = slot_at(u32_arg(ctx, 3));
    const uint32_t font_ea = u32_arg(ctx, 4);
    if (!src || !font_ea) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    FontSlot* s = alloc_slot();
    if (!s) return ret(ctx, CELL_FONT_ERROR_ALLOCATION_FAILED);
    *s = *src;
    s->renderer = kUnbound;
    write_font(font_ea, *s);
    FLOG("OpenFontInstance(%u) -> %u\n", (unsigned)(src - g_fonts), (unsigned)(s - g_fonts));
    ret(ctx, CELL_OK);
}

/* cellFontCloseFont(font*) */
void FontCloseFont(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    FontSlot* s = slot_at(u32_arg(ctx, 3));
    if (!s) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    FLOG("CloseFont(%u)\n", (unsigned)(s - g_fonts));
    s->in_use = false;
    ret(ctx, CELL_OK);
}

/* cellFontCreateRenderer(lib, config, renderer*): the game owns the renderer
 * object; its first word holds our index. */
void FontCreateRenderer(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t r_ea = u32_arg(ctx, 5);
    if (!r_ea) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    for (uint32_t i = 0; i < kMaxRenderers; ++i) {
        if (!g_renderers[i]) {
            g_renderers[i] = true;
            vm_write32(r_ea, i);
            FLOG("CreateRenderer -> %u\n", i);
            return ret(ctx, CELL_OK);
        }
    }
    ret(ctx, CELL_FONT_ERROR_ALLOCATION_FAILED);
}

/* cellFontDestroyRenderer(renderer*) */
void FontDestroyRenderer(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t r_ea = u32_arg(ctx, 3);
    if (!r_ea) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    const uint32_t r = vm_read32(r_ea);
    if (r >= kMaxRenderers) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    g_renderers[r] = false;
    vm_write32(r_ea, kUnbound);
    ret(ctx, CELL_OK);
}

/* cellFontBindRenderer(font*, renderer*) */
void FontBindRenderer(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    FontSlot* s = slot_at(u32_arg(ctx, 3));
    const uint32_t r_ea = u32_arg(ctx, 4);
    if (!s || !r_ea) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    const uint32_t r = vm_read32(r_ea);
    if (r >= kMaxRenderers || !g_renderers[r]) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    if (s->renderer != kUnbound) return ret(ctx, CELL_FONT_ERROR_RENDERER_ALREADY_BIND);
    s->renderer = r;
    ret(ctx, CELL_OK);
}

/* cellFontUnbindRenderer(font*) */
void FontUnbindRenderer(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    FontSlot* s = slot_at(u32_arg(ctx, 3));
    if (!s) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    s->renderer = kUnbound;
    ret(ctx, CELL_OK);
}

/* cellFontSetScalePixel(font*, w, h), and SetupRenderScalePixel, same ABI. */
void FontSetScalePixel(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t font_ea = u32_arg(ctx, 3);
    FontSlot* s = slot_at(font_ea);
    if (!s) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    s->px_w = (float)ctx->fpr[1];
    s->px_h = (float)ctx->fpr[2];
    write_font(font_ea, *s);
    FLOG("SetScalePixel(%u, %.2f, %.2f)\n", (unsigned)(s - g_fonts), s->px_w, s->px_h);
    ret(ctx, CELL_OK);
}

/* cellFontSetScalePoint(font*, w, h): points at the font's dpi. */
void FontSetScalePoint(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t font_ea = u32_arg(ctx, 3);
    FontSlot* s = slot_at(font_ea);
    if (!s) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    s->px_w = (float)ctx->fpr[1] * (float)s->dpi_h / 72.0f;
    s->px_h = (float)ctx->fpr[2] * (float)s->dpi_v / 72.0f;
    write_font(font_ea, *s);
    FLOG("SetScalePoint(%u) -> %.2f x %.2f px\n", (unsigned)(s - g_fonts), s->px_w, s->px_h);
    ret(ctx, CELL_OK);
}

/* cellFontSetResolutionDpi(font*, h, v) */
void FontSetResolutionDpi(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    FontSlot* s = slot_at(u32_arg(ctx, 3));
    if (!s) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    s->dpi_h = u32_arg(ctx, 4) ? u32_arg(ctx, 4) : 72;
    s->dpi_v = u32_arg(ctx, 5) ? u32_arg(ctx, 5) : 72;
    ret(ctx, CELL_OK);
}

/* cellFontSetEffectWeight(font*, weight), and SetupRenderEffectWeight. */
void FontSetEffectWeight(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    FontSlot* s = slot_at(u32_arg(ctx, 3));
    if (!s) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    s->weight = (float)ctx->fpr[1];
    FLOG("SetEffectWeight(%u, %.3f)\n", (unsigned)(s - g_fonts), s->weight);
    ret(ctx, CELL_OK);
}

/* cellFontSetEffectSlant(font*, slant), and SetupRenderEffectSlant. */
void FontSetEffectSlant(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    FontSlot* s = slot_at(u32_arg(ctx, 3));
    if (!s) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    s->slant = (float)ctx->fpr[1];
    FLOG("SetEffectSlant(%u, %.3f)\n", (unsigned)(s - g_fonts), s->slant);
    ret(ctx, CELL_OK);
}

/* cellFontGetHorizontalLayout(font*, layout*): baseLineY, lineHeight,
 * effectHeight. GetVerticalLayout has the same three-float shape. */
void FontGetHorizontalLayout(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    FontSlot* s = slot_at(u32_arg(ctx, 3));
    const uint32_t l_ea = u32_arg(ctx, 4);
    if (!s || !l_ea || g_faces.empty()) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    const Layout l = line_layout(*s);
    write_float(l_ea + 0, l.baseline);
    write_float(l_ea + 4, l.line_height);
    write_float(l_ea + 8, l.effect);
    FLOG("GetHorizontalLayout(%u) -> %.3f %.3f %.3f\n", (unsigned)(s - g_fonts), l.baseline,
         l.line_height, l.effect);
    ret(ctx, CELL_OK);
}

void FontGetVerticalLayout(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    FontSlot* s = slot_at(u32_arg(ctx, 3));
    const uint32_t l_ea = u32_arg(ctx, 4);
    if (!s || !l_ea) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    write_float(l_ea + 0, s->px_w * 0.5f);   /* baseLineX: the column's centre */
    write_float(l_ea + 4, s->px_w);          /* lineWidth */
    write_float(l_ea + 8, s->px_w);          /* effectWidth */
    ret(ctx, CELL_OK);
}

/* cellFont{GetRender,Get}CharGlyphMetrics[Vertical](font*, code, metrics*) */
void FontGetCharGlyphMetrics(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    FontSlot* s = slot_at(u32_arg(ctx, 3));
    const uint32_t m_ea = u32_arg(ctx, 5);
    if (!s || !m_ea) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    write_metrics(m_ea, glyph_metrics(*s, u32_arg(ctx, 4)));
    ret(ctx, CELL_OK);
}

/* cellFontGetFontIdCode(font*, code, fontId*, fontCode*): fontCode may be null. */
void FontGetFontIdCode(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    FontSlot* s = slot_at(u32_arg(ctx, 3));
    const uint32_t code = u32_arg(ctx, 4), id_ea = u32_arg(ctx, 5), code_ea = u32_arg(ctx, 6);
    if (!s || !id_ea) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    int glyph;
    const Face* f = face_for(code, &glyph);
    vm_write32(id_ea, f ? (uint32_t)(f - g_faces.data()) : 0);
    if (code_ea) vm_write32(code_ea, code);
    ret(ctx, CELL_OK);
}

/* cellFontRenderSurfaceInit(surface*, buffer, widthByte, pixelSizeByte, w, h):
 * buffer, widthByte, pixelSizeByte, width, height, scissor x0 y0 x1 y1. */
void FontRenderSurfaceInit(ppu_context* ctx)
{
    const uint32_t s_ea = u32_arg(ctx, 3);
    if (!s_ea) return;
    /* The game draws every map-tag name into one scratch surface, copies it
     * into one of two 6-name staging buffers in RSX memory and queues an
     * NV3089 blit of each full buffer into its name sheet; it refills a
     * staging buffer as soon as its turn comes again, trusting the RSX to
     * have copied it by then. A real RSX consumes the command stream in
     * microseconds, and the firmware takes milliseconds a name; our FIFO
     * drain trails the PPU by up to a frame, so names 13-18 overwrote 1-6
     * before their blit ran and the tags showed other labels (and, with the
     * lifted libfont, nothing). So each new surface first waits until the
     * drain has consumed what the game has queued. TJ_FONT_RSX_SYNC=0: off.
     *
     * That alone is not enough: the blits sit in the game's command buffer
     * unkicked until its render thread flushes at the end of the frame, so
     * PUT == GET and the wait returned at once (all nine surface tags blank,
     * m12-map3). The flip count only moves when the drain reaches a flip, so
     * a flip seen means everything queued before it has run. Waiting for one
     * at every surface was too slow (user, 2026-10-22): the render thread
     * also starts surfaces and can't flip while it waits (a 100 ms freeze
     * each, m12-wait1), and the font thread took a frame a name. So the
     * thread that issues the flips never waits for one, and other threads
     * start at most kNamesPerFlip surfaces between two flips: a staging
     * buffer is refilled 7+ names after its last name, so at least two flips
     * (its blit queued, kicked and drained) fall in between.
     * TJ_FONT_RSX_SYNC=2: the drain only. */
    static const int sync = getenv("TJ_FONT_RSX_SYNC") ? atoi(getenv("TJ_FONT_RSX_SYNC")) : 1;
    if (sync) {
        if (sync == 1 && ctx->thread_id != g_flip_tid.load(std::memory_order_relaxed)) {
            static std::mutex m;
            static uint32_t win_flip, win_n;
            std::lock_guard<std::mutex> lock(m);
            uint32_t f = cellGcm_flip_request_count();
            if (f != win_flip) { win_flip = f; win_n = 0; }
            if (win_n >= kNamesPerFlip) {
                const unsigned long long t0 = ps3_ms_now();
                while ((f = cellGcm_flip_request_count()) == win_flip && ps3_ms_now() - t0 < 100)
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                FLOG("RenderSurfaceInit: thread %llu waited %llu ms for a flip\n",
                     (unsigned long long)ctx->thread_id, ps3_ms_now() - t0);
                win_flip = f; win_n = 0;
            }
            ++win_n;
        }
        const int ok = cellGcm_rsx_wait_consumed(100);
        FLOG("RenderSurfaceInit: RSX %s\n", ok ? "caught up" : "wait timed out");
    }
    const uint32_t w = (int32_t)ctx->gpr[7] > 0 ? u32_arg(ctx, 7) : 0;
    const uint32_t h = (int32_t)ctx->gpr[8] > 0 ? u32_arg(ctx, 8) : 0;
    vm_write32(s_ea + 0, u32_arg(ctx, 4));
    vm_write32(s_ea + 4, u32_arg(ctx, 5));
    vm_write32(s_ea + 8, u32_arg(ctx, 6));
    vm_write32(s_ea + 12, w);
    vm_write32(s_ea + 16, h);
    vm_write32(s_ea + 20, 0);
    vm_write32(s_ea + 24, 0);
    vm_write32(s_ea + 28, w);
    vm_write32(s_ea + 32, h);
    FLOG("RenderSurfaceInit(0x%08X: buf 0x%08X, %u B/row, %u B/px, %ux%u)\n", s_ea,
         u32_arg(ctx, 4), u32_arg(ctx, 5), u32_arg(ctx, 6), w, h);
}

/* cellFontRenderSurfaceSetScissor(surface*, x0, y0, w, h) */
void FontRenderSurfaceSetScissor(ppu_context* ctx)
{
    const uint32_t s_ea = u32_arg(ctx, 3);
    if (!s_ea) return;
    const int32_t x0 = (int32_t)ctx->gpr[4], y0 = (int32_t)ctx->gpr[5];
    const uint32_t sw = vm_read32(s_ea + 12), sh = vm_read32(s_ea + 16);
    uint64_t sx0 = x0 > 0 ? (uint32_t)x0 : 0, sy0 = y0 > 0 ? (uint32_t)y0 : 0;
    uint64_t sx1 = sx0 + u32_arg(ctx, 6), sy1 = sy0 + u32_arg(ctx, 7);
    if (sx0 > sw) sx0 = sw;
    if (sy0 > sh) sy0 = sh;
    if (sx1 > sw) sx1 = sw;
    if (sy1 > sh) sy1 = sh;
    vm_write32(s_ea + 20, (uint32_t)sx0);
    vm_write32(s_ea + 24, (uint32_t)sy0);
    vm_write32(s_ea + 28, (uint32_t)sx1);
    vm_write32(s_ea + 32, (uint32_t)sy1);
}

/* cellFontRenderCharGlyphImage(font*, code, surface*, x, y, metrics*,
 * transInfo*). (x, y) is the top-left of the glyph's line on the surface.
 * With a transInfo (the game's case: a 4-byte-per-pixel sheet) nothing is
 * written to the surface: the game blends the alpha image itself, from
 * transInfo = {image, imageWidthByte, imageWidth, imageHeight, surface
 * position, surfaceWidthByte}, clipped to the surface and its scissor. */
void FontRenderCharGlyphImage(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    FontSlot* s = slot_at(u32_arg(ctx, 3));
    const uint32_t code = u32_arg(ctx, 4), s_ea = u32_arg(ctx, 5);
    const uint32_t m_ea = u32_arg(ctx, 8), t_ea = u32_arg(ctx, 9);
    const float x = (float)ctx->fpr[1], y = (float)ctx->fpr[2];
    if (!s || !s_ea) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    if (!ensure_arena(ctx)) return ret(ctx, CELL_FONT_ERROR_ALLOCATION_FAILED);
    Glyph g;
    load_glyph(*s, code, g);
    if (m_ea) write_metrics(m_ea, glyph_metrics(*s, g));
    if (t_ea) for (uint32_t o = 0; o < 24; o += 4) vm_write32(t_ea + o, 0);
    FLOG("RenderCharGlyphImage(%u, U+%04X, %.2f, %.2f, glyph %d)\n",
         (unsigned)(s - g_fonts), code, x, y, g.index);
    if (!g.n) return ret(ctx, CELL_OK);   /* space or missing: advance only */

    /* The image: the outline's box on the surface, pen at (x, baseline),
     * with room for the bold growth, out to whole pixels. */
    const float sx = scale_x(*g.face, *s), sy = scale_y(*g.face, *s);
    const float r = bold_strength(*s) * 0.5f * kBoldInk;
    const float base = y + line_layout(*s).baseline;
    const int32_t gx = (int32_t)floorf(x + g.ix0 * sx - r);
    const int32_t gy = (int32_t)floorf(base - g.iy1 * sy - 2 * r);
    const int32_t bw = (int32_t)ceilf(x + g.ix1 * sx + 2 * r) - gx;
    const int32_t bh = (int32_t)ceilf(base - g.iy0 * sy + r) - gy;
    if (bw <= 0 || bh <= 0) return ret(ctx, CELL_OK);
    std::vector<uint8_t> bmp((size_t)bw * bh), pass((size_t)bw * bh);
    /* Bold is the outline dilated by r (the most coverage over the outline
     * moved r in eight directions) and moved r right and up, so its left and
     * bottom edges stay put, as FreeType's emboldening does. */
    static const float kDirs[9][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1},
                                      {0.7071f, 0.7071f}, {-0.7071f, 0.7071f},
                                      {0.7071f, -0.7071f}, {-0.7071f, -0.7071f}};
    for (int i = 0; i < (r > 0.0f ? 9 : 1); ++i) {
        std::fill(pass.begin(), pass.end(), 0);
        stbtt__bitmap out = {bw, bh, bw, pass.data()};
        stbtt_Rasterize(&out, 0.35f, g.v, g.n, sx, sy, x + (kDirs[i][0] + 1.0f) * r,
                        base + (kDirs[i][1] - 1.0f) * r, gx, gy, 1, nullptr);
        for (size_t j = 0; j < bmp.size(); ++j) bmp[j] = std::max(bmp[j], pass[j]);
    }

    const uint32_t buf = vm_read32(s_ea + 0);
    const int32_t wbyte = (int32_t)vm_read32(s_ea + 4), psize = (int32_t)vm_read32(s_ea + 8);
    const int32_t sw = (int32_t)vm_read32(s_ea + 12), sh = (int32_t)vm_read32(s_ea + 16);
    int32_t left = (int32_t)vm_read32(s_ea + 20), top = (int32_t)vm_read32(s_ea + 24);
    int32_t right = (int32_t)vm_read32(s_ea + 28), bottom = (int32_t)vm_read32(s_ea + 32);
    if (right > sw) right = sw;
    if (bottom > sh) bottom = sh;
    if (left < gx) left = gx;
    if (top < gy) top = gy;
    if (right > gx + bw) right = gx + bw;
    if (bottom > gy + bh) bottom = gy + bh;

    /* A surface may have no buffer (the game sizes glyphs on a 64x64 one with
     * buffer 0): the image still comes back. */
    if (psize > 0 && right > left && bottom > top) {
        if (t_ea) {
            /* The glyph image, whole, in this font's guest buffer. */
            const uint32_t img = g_arena + kImageOff + (uint32_t)(s - g_fonts) * kImageCap;
            if ((uint32_t)(bw * bh) > kImageCap) {
                fprintf(stderr, "[font] U+%04X is %dx%d px, over the %u-byte glyph buffer\n",
                        code, bw, bh, kImageCap);
            } else {
                memcpy(vm_base + img, bmp.data(), bmp.size());
                vm_write32(t_ea + 0, img + (uint32_t)((top - gy) * bw + (left - gx)));
                vm_write32(t_ea + 4, (uint32_t)bw);
                vm_write32(t_ea + 8, (uint32_t)(right - left));
                vm_write32(t_ea + 12, (uint32_t)(bottom - top));
                vm_write32(t_ea + 16, buf + (uint32_t)(top * wbyte + left * psize));
                vm_write32(t_ea + 20, (uint32_t)wbyte);
            }
        } else if (buf) {
            /* No transInfo: the alpha goes straight to the surface. */
            for (int32_t row = top; row < bottom; ++row)
                for (int32_t col = left; col < right; ++col) {
                    const uint8_t a = bmp[(size_t)(row - gy) * bw + (col - gx)];
                    if (a) vm_base[buf + (uint32_t)(row * wbyte + col * psize)] = a;
                }
        }
    }
    ret(ctx, CELL_OK);
}

/* cellFontGenerateCharGlyph(font*, code, glyph**) / cellFontDeleteGlyph(font*,
 * glyph*): an opaque object holding the character code. */
void FontGenerateCharGlyph(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t out = u32_arg(ctx, 5);
    if (!u32_arg(ctx, 3) || !out) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    if (!ensure_arena(ctx)) return ret(ctx, CELL_FONT_ERROR_ALLOCATION_FAILED);
    for (uint32_t i = 0; i < kGlyphCount; ++i) {
        if (!g_glyph_used[i]) {
            g_glyph_used[i] = true;
            const uint32_t g = g_arena + kGlyphOff + i * kGlyphSize;
            memset(vm_base + g, 0, kGlyphSize);
            vm_write32(g, u32_arg(ctx, 4));
            vm_write32(out, g);
            return ret(ctx, CELL_OK);
        }
    }
    ret(ctx, CELL_FONT_ERROR_ALLOCATION_FAILED);
}

void FontDeleteGlyph(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t g = u32_arg(ctx, 4);
    if (!u32_arg(ctx, 3) || !g || !g_arena) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    const uint32_t i = (g - g_arena - kGlyphOff) / kGlyphSize;
    if (g < g_arena + kGlyphOff || i >= kGlyphCount) return ret(ctx, CELL_FONT_ERROR_INVALID_PARAMETER);
    g_glyph_used[i] = false;
    ret(ctx, CELL_OK);
}

/* ---- TJ_FONT_ORACLE=1: what a glyph call returned ---------------------------
 *
 * Logs, for the first glyphs of a few characters, the metrics, where the image
 * lands relative to (x, y) and the image itself as text. Matched against
 * the lifted firmware libfont this way before it was removed. */
void oracle_log(const char* who, ppu_context* in, ppu_context* out)
{
    static int budget = 200;
    const uint32_t code = (uint32_t)in->gpr[4], s_ea = (uint32_t)in->gpr[5];
    const uint32_t m_ea = (uint32_t)in->gpr[8], t_ea = (uint32_t)in->gpr[9];
    if (!strchr("ATgjy", (int)code) || budget <= 0) return;
    --budget;
    float m[8] = {};
    if (m_ea) for (int i = 0; i < 8; ++i) { uint32_t b = vm_read32(m_ea + 4 * i); memcpy(&m[i], &b, 4); }
    const uint32_t buf = vm_read32(s_ea), wbyte = vm_read32(s_ea + 4), psize = vm_read32(s_ea + 8);
    printf("[font-oracle] %s '%c' at (%.1f, %.1f) rc=0x%X surface buf 0x%08X %u B/row %u B/px "
           "metrics w %.2f h %.2f hbx %.2f hby %.2f hadv %.2f vbx %.2f vby %.2f vadv %.2f\n",
           who, (char)code, in->fpr[1], in->fpr[2], (unsigned)out->gpr[3], buf, wbyte, psize,
           m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
    if (!t_ea) return;
    const uint32_t img = vm_read32(t_ea), iwb = vm_read32(t_ea + 4), iw = vm_read32(t_ea + 8),
                   ih = vm_read32(t_ea + 12), dst = vm_read32(t_ea + 16);
    const uint32_t off = dst - buf;
    printf("[font-oracle]   image 0x%08X %u B/row %ux%u lands at col %u row %u\n", img, iwb, iw,
           ih, wbyte && psize ? off % wbyte / psize : 0, wbyte ? off / wbyte : 0);
    if (!img || iw > 80 || ih > 80) return;
    for (uint32_t r = 0; r < ih; ++r) {
        char line[96];
        for (uint32_t c = 0; c < iw; ++c)
            line[c] = " .:-=+*#%@"[vm_base[img + r * iwb + c] * 9 / 255];
        line[iw] = 0;
        printf("[font-oracle]   |%s|\n", line);
    }
}

void OracleHle(ppu_context* ctx)
{
    ppu_context in = *ctx;
    FontRenderCharGlyphImage(ctx);
    oracle_log("hle", &in, ctx);
}

/* The game's flip imports, unchanged, noting which thread calls them: that
 * thread must never wait for a flip in FontRenderSurfaceInit. */
void FlipCommand(ppu_context* ctx)
{
    g_flip_tid.store(ctx->thread_id, std::memory_order_relaxed);
    ret(ctx, _cellGcmSetFlipCommand((void*)(uintptr_t)u32_arg(ctx, 3), u32_arg(ctx, 4)));
}

void FlipCommandWithWaitLabel(ppu_context* ctx)
{
    g_flip_tid.store(ctx->thread_id, std::memory_order_relaxed);
    ret(ctx, _cellGcmSetFlipCommandWithWaitLabel((void*)(uintptr_t)u32_arg(ctx, 3), u32_arg(ctx, 4),
                                                 u32_arg(ctx, 5), u32_arg(ctx, 6)));
}

} // namespace

/* Every cellFont/cellFontFT import the game has (generated/analysis/
 * EBOOT.imports.json), and the two flip imports (FlipCommand). */
extern "C" void tj_font_hle_register(void)
{
    struct { uint32_t nid; const char* name; void (*fn)(ppu_context*); } const t[] = {
        {0xF03DCC29u, "cellFontInitializeWithRevision", FontInitializeWithRevision},
        {0x7AB47F7Eu, "cellFontEnd", FontEnd},
        {0x7A0A83C4u, "cellFontInitLibraryFreeTypeWithRevision", FontInitLibraryFreeType},
        {0x40D40544u, "cellFontEndLibrary", FontEndLibrary},
        {0xA885CC9Bu, "cellFontOpenFontset", FontOpenFontset},
        {0x073FA321u, "cellFontOpenFontsetOnMemory", FontOpenFontset},
        {0x29329541u, "cellFontOpenFontInstance", FontOpenFontInstance},
        {0xB276F1F6u, "cellFontCloseFont", FontCloseFont},
        {0x042E74E3u, "cellFontCreateRenderer", FontCreateRenderer},
        {0x21EBB248u, "cellFontDestroyRenderer", FontDestroyRenderer},
        {0x66A23100u, "cellFontBindRenderer", FontBindRenderer},
        {0xF16379FAu, "cellFontUnbindRenderer", FontUnbindRenderer},
        {0x297F0E93u, "cellFontSetScalePixel", FontSetScalePixel},
        {0x227E1E3Cu, "cellFontSetupRenderScalePixel", FontSetScalePixel},
        {0x70F3E728u, "cellFontSetScalePoint", FontSetScalePoint},
        {0xFB3341BAu, "cellFontSetResolutionDpi", FontSetResolutionDpi},
        {0x25253FE4u, "cellFontSetEffectWeight", FontSetEffectWeight},
        {0xA6DC25D1u, "cellFontSetupRenderEffectWeight", FontSetEffectWeight},
        {0x8657C8F5u, "cellFontSetEffectSlant", FontSetEffectSlant},
        {0x78D05E08u, "cellFontSetupRenderEffectSlant", FontSetEffectSlant},
        {0x1387C45Cu, "cellFontGetHorizontalLayout", FontGetHorizontalLayout},
        {0x698897F8u, "cellFontGetVerticalLayout", FontGetVerticalLayout},
        {0x2DA9FD9Du, "cellFontGetRenderCharGlyphMetrics", FontGetCharGlyphMetrics},
        {0xD8EAEE9Fu, "cellFontGetCharGlyphMetrics", FontGetCharGlyphMetrics},
        {0x700E6223u, "cellFontGetRenderCharGlyphMetricsVertical", FontGetCharGlyphMetrics},
        {0xFE9A6DD7u, "cellFontGetCharGlyphMetricsVertical", FontGetCharGlyphMetrics},
        {0x98AC5524u, "cellFontGetFontIdCode", FontGetFontIdCode},
        {0x90B9465Eu, "cellFontRenderSurfaceInit", FontRenderSurfaceInit},
        {0xB422B005u, "cellFontRenderSurfaceSetScissor", FontRenderSurfaceSetScissor},
        {0x88BE4799u, "cellFontRenderCharGlyphImage", FontRenderCharGlyphImage},
        {0xC17259DEu, "cellFontGenerateCharGlyph", FontGenerateCharGlyph},
        {0xD62F5D76u, "cellFontDeleteGlyph", FontDeleteGlyph},
        {0x21397818u, "_cellGcmSetFlipCommand", FlipCommand},
        {0xD8F88E1Au, "_cellGcmSetFlipCommandWithWaitLabel", FlipCommandWithWaitLabel},
    };
    for (const auto& e : t) ps3_hle_register_ctx(e.nid, e.name, e.fn);
    if (getenv("TJ_FONT_ORACLE"))
        ps3_hle_register_ctx(0x88BE4799u, "cellFontRenderCharGlyphImage", OracleHle);
}
