/*
 * Tokyo Jungle -- cellSail (the movie player) and the two cellPamf calls the
 * game makes, answered natively.
 *
 * The game plays its PAMF movies (the looping load_*.pam behind loading
 * screens, the openings, the credits) through cellSail: a player with one
 * graphics and one sound adapter it polls once a frame (GetFrame), plus a
 * player callback that reports progress as events. This replaces the
 * firmware's libsail and libpamf, which ran lifted (scripts/lift_firmware.py).
 *
 * What the game sees is taken from a trace of the lifted libsail
 * (tools/sail/sail_trace.cpp, 2026-10-22): the events and their order, which
 * of them land while the call that caused them is still waiting, the
 * GetFrame results and the formats. The work behind it is ours: a small
 * MPEG-PS demuxer for the PAMF payload, H.264 and AC3 decoded by the
 * runtime's FFmpeg back ends (vdec_ffmpeg / adec_ffmpeg, FFmpeg loaded at
 * run time), the picture converted to ARGB straight into the frame buffers
 * the game's allocFrame callback hands over.
 *
 * Threads: like libsail, a worker thread per player runs the decoder and
 * delivers every event and adapter callback (ppu_guest_call). The four calls
 * that report a transition state (OpenStream, Start, Stop, CloseStream) wait
 * until the worker has delivered it, as they did. GetFrame and the clock run
 * on the game's threads under one mutex.
 *
 * Clock: the movie's time is the wall time since the session started (not
 * counting pauses). Video frames are shown when their PTS comes due; audio is
 * handed out as the game's audio thread asks for it.
 *
 * Diagnostics: TJ_SAIL_LOG=1 logs calls and events.
 */

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ppu_context.h"

extern "C" uint8_t* vm_base;
extern "C" uint32_t vm_read32(uint64_t addr);
extern "C" void vm_write32(uint64_t addr, uint32_t v);
extern "C" int cellfs_translate_path(const char* ps3_path, char* host_buf, size_t buf_size);
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" uint64_t ppu_guest_call(uint32_t opd_addr, uint64_t a0, uint64_t a1, uint64_t a2,
                                   uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);

/* third_party/ps3recomp/libs/codec: vdec_ffmpeg.h, adec_ffmpeg.h */
extern "C" {
typedef struct VdecFf VdecFf;
typedef struct {
    uint8_t* yuv; uint32_t width, height, token; int pict_type, key;
} VdecFfPicture;
typedef void (*vdec_ff_emit_fn)(void* ctx, const VdecFfPicture* pic);
VdecFf* vdec_ff_open(uint32_t codec_type);
void vdec_ff_close(VdecFf* d);
int  vdec_ff_decode(VdecFf* d, const uint8_t* au, uint32_t size, uint32_t token,
                    vdec_ff_emit_fn emit, void* ctx);
void vdec_ff_drain(VdecFf* d, vdec_ff_emit_fn emit, void* ctx);
void vdec_ff_reset(VdecFf* d);
void vdec_store_picture(uint32_t dst, const uint8_t* yuv, uint32_t w, uint32_t h,
                        uint32_t format_type, uint32_t color_matrix, uint8_t alpha);

typedef struct AdecFf AdecFf;
typedef struct {
    const float* plane[8]; int channels, samples, rate; uint64_t mask;
} AdecFfFrame;
typedef void (*adec_ff_emit_fn)(void* ctx, const AdecFfFrame* f);
AdecFf* adec_ff_open(uint32_t codec_type);
void adec_ff_close(AdecFf* d);
void adec_ff_reset(AdecFf* d);
int  adec_ff_decode(AdecFf* d, const uint8_t* au, uint32_t size, adec_ff_emit_fn emit, void* ctx);
}

namespace {

constexpr uint32_t kVdecMpeg2 = 0;                /* CELL_VDEC_CODEC_TYPE_MPEG2 */
constexpr uint32_t kVdecAvc = 1;                  /* CELL_VDEC_CODEC_TYPE_AVC */
constexpr uint32_t kAdecAc3 = 2;                  /* CELL_ADEC_TYPE_AC3 */
constexpr int32_t  kErrInvalidArg   = (int32_t)0x80610701u;  /* CELL_SAIL_ERROR_INVALID_ARG */
constexpr int32_t  kErrInvalidState = (int32_t)0x80610702u;  /* CELL_SAIL_ERROR_INVALID_STATE */
constexpr int32_t  kPamfErrInvalidArg = (int32_t)0x80610501u;
constexpr uint64_t kNoPts = ~0ull;

/* CellSailEvent majors, player states and calls (cell/sail/common.h). */
enum : uint32_t {
    EV_CALL_COMPLETED = 2, EV_STATE_CHANGED = 3, EV_STREAM_OPENED = 4, EV_STREAM_CLOSED = 5,
    EV_SESSION_STARTED = 6, EV_SOURCE_EOS = 8, EV_ES_OPENED = 9, EV_ES_CLOSED = 10,
};
enum : uint32_t {
    ST_CLOSED = 2, ST_OPEN_TRANSITION = 3, ST_OPENED = 4, ST_START_TRANSITION = 5,
    ST_RUNNING = 6, ST_STOP_TRANSITION = 7, ST_CLOSE_TRANSITION = 8,
};
enum : uint32_t { CALL_BOOT = 1, CALL_OPEN_STREAM = 2, CALL_CLOSE_STREAM = 3,
                  CALL_START = 10, CALL_STOP = 11 };

/* GetFrame results as libsail returned them: > 0xFF is a frame (the game
 * tests exactly that), 0x204 a new one, 0x104 the previous one again; 0xC is
 * an open stream with nothing to show yet, 0x1C no stream at all. */
constexpr uint32_t kFrameNew = 0x204, kFrameSame = 0x104, kFrameNone = 0xC, kFrameNoStream = 0x1C;

constexpr uint32_t kAudioSamples = 256;           /* per SoundAdapterGetFrame */
constexpr uint32_t kMaxChannels = 6;
constexpr size_t   kVideoAhead = 6;               /* decoded pictures kept ahead */
constexpr size_t   kAudioAhead = 48000;           /* samples kept ahead */
constexpr uint32_t kSessionStep = 0x80;           /* session ids: 0x80, 0x100, ... */

bool g_log = false;
const auto g_t0 = std::chrono::steady_clock::now();
long long ms_now() { return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_t0).count(); }
#define SLOG(...) do { if (g_log) { std::printf("[sail %6lld] ", ms_now()); std::printf(__VA_ARGS__); std::fflush(stdout); } } while (0)

uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
void w8(uint32_t ea, uint8_t v) { vm_base[ea] = v; }
void w16(uint32_t ea, uint16_t v) { vm_base[ea] = (uint8_t)(v >> 8); vm_base[ea + 1] = (uint8_t)v; }
void w64(uint32_t ea, uint64_t v) { vm_write32(ea, (uint32_t)(v >> 32)); vm_write32(ea + 4, (uint32_t)v); }
int32_t ret(ppu_context* c, int32_t v) { c->gpr[3] = (uint64_t)(int64_t)v; return v; }

/* ---------------------------------------------------------------------------
 * PAMF: header fields, and the MPEG-PS payload cut into access units
 * -----------------------------------------------------------------------*/

struct PamfFile {
    std::vector<uint8_t> data;
    uint32_t payload = 0;                         /* byte offset of the PS payload */
    uint64_t start_pts = 0, end_pts = 0;          /* presentation start / end, 90 kHz */
    bool avc = false;                             /* video is H.264 (0x1B), else MPEG-2 (0x02) */

    bool load(const std::string& host)
    {
        FILE* f = std::fopen(host.c_str(), "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        data.resize(n > 0 ? (size_t)n : 0);
        const bool ok = n > 0x80 && std::fread(data.data(), 1, data.size(), f) == data.size();
        std::fclose(f);
        if (!ok || std::memcmp(data.data(), "PAMF", 4) != 0) return false;
        const uint8_t* h = data.data();
        payload = be32(h + 0x08) << 11;
        /* Stream table: 0x30-byte entries from 0x88, type first. */
        for (uint32_t i = 0, n = h[0x6D]; i < n && 0x88 + i * 0x30 < data.size(); ++i)
            if (h[0x88 + i * 0x30] == 0x1B) avc = true;
        start_pts = (uint64_t)be16(h + 0x56) << 32 | be32(h + 0x58);
        end_pts = (uint64_t)be16(h + 0x5C) << 32 | be32(h + 0x5E);
        return payload < data.size();
    }
};

uint64_t read_ts(const uint8_t* p)
{
    return (uint64_t)(p[0] & 0x0E) << 29 | (uint64_t)p[1] << 22 | (uint64_t)(p[2] >> 1) << 15 |
           (uint64_t)p[3] << 7 | (uint64_t)(p[4] >> 1);
}

/* AC3 frame size in 16-bit words by [fscod][frmsizecod] (ATSC A/52). */
const uint16_t k_ac3_words[3][38] = {
    { 64, 64, 80, 80, 96, 96, 112, 112, 128, 128, 160, 160, 192, 192, 224, 224, 256, 256, 320, 320,
      384, 384, 448, 448, 512, 512, 640, 640, 768, 768, 896, 896, 1024, 1024, 1152, 1152, 1280, 1280 },
    { 69, 70, 87, 88, 104, 105, 121, 122, 139, 140, 174, 175, 208, 209, 243, 244, 278, 279, 348, 349,
      417, 418, 487, 488, 557, 558, 696, 697, 835, 836, 975, 976, 1114, 1115, 1253, 1254, 1393, 1394 },
    { 96, 96, 120, 120, 144, 144, 168, 168, 192, 192, 240, 240, 288, 288, 336, 336, 384, 384, 480, 480,
      576, 576, 672, 672, 768, 768, 960, 960, 1152, 1152, 1344, 1344, 1536, 1536, 1728, 1728, 1920, 1920 },
};

/* One elementary stream being reassembled: bytes, and the PTS each PES
 * brought with the offset its payload starts at. */
struct EsBuf {
    std::vector<uint8_t> acc;
    std::deque<std::pair<size_t, uint64_t>> pts;
    void push(const uint8_t* p, size_t n, uint64_t ts)
    {
        if (ts != kNoPts) pts.emplace_back(acc.size(), ts);
        acc.insert(acc.end(), p, p + n);
    }
    void take(size_t n)
    {
        for (auto& e : pts) e.first = e.first > n ? e.first - n : 0;
        acc.erase(acc.begin(), acc.begin() + (ptrdiff_t)n);
    }
    /* An AU starts at acc[0]: it takes the newest PTS of a packet whose
     * payload began at or before it (the MPEG rule), using those up. */
    uint64_t begin()
    {
        uint64_t ts = kNoPts;
        while (!pts.empty() && pts.front().first == 0) { ts = pts.front().second; pts.pop_front(); }
        return ts;
    }
    void clear() { acc.clear(); pts.clear(); }
};

struct Demux {
    const PamfFile* f = nullptr;
    size_t pos = 0;
    bool eof = false;
    bool avc = false;                             /* H.264, else MPEG-2 video */
    EsBuf video, audio;
    /* the video AU being cut: started at acc[0], has a picture, searched up to */
    bool vstarted = false, vpic = false;
    size_t vscan = 0;
    uint64_t vts = kNoPts;

    void reset(const PamfFile* file)
    {
        f = file; pos = file->payload; eof = false; avc = file->avc;
        video.clear(); audio.clear();
        vstarted = vpic = false; vscan = 0; vts = kNoPts;
    }

    /* Parse one pack / PES. False at the end of the payload. */
    bool step()
    {
        const std::vector<uint8_t>& d = f->data;
        while (pos + 6 <= d.size() && !(d[pos] == 0 && d[pos + 1] == 0 && d[pos + 2] == 1)) ++pos;
        if (pos + 6 > d.size()) { eof = true; return false; }
        const uint8_t* p = &d[pos];
        const uint8_t code = p[3];
        if (code == 0xBA) { pos += 14 + (pos + 14 <= d.size() ? (p[13] & 7) : 0); return true; }
        if (code == 0xB9) { eof = true; return false; }
        const size_t len = 6 + be16(p + 4);
        if (pos + len > d.size()) { eof = true; return false; }
        if ((code == 0xE0 || code == 0xBD) && len >= 9) {
            const size_t hdr = 9 + p[8];
            const uint64_t ts = (p[7] & 0x80) && hdr >= 14 ? read_ts(p + 9) : kNoPts;
            if (hdr <= len) {
                const uint8_t* pay = p + hdr;
                size_t n = len - hdr;
                if (code == 0xE0) video.push(pay, n, ts);
                else if (n > 4 && pay[0] == 0x30) audio.push(pay + 4, n - 4, ts);   /* AC3 #0 */
            }
        }
        pos += len;
        return true;
    }

    /* Next video access unit. H.264 AUs run from one access unit delimiter to
     * the next. An MPEG-2 AU is one picture with the sequence / GOP headers in
     * front of it; a sequence end code stays in the AU it ends. */
    bool next_video(std::vector<uint8_t>& au, uint64_t& ts)
    {
        for (;;) {
            std::vector<uint8_t>& a = video.acc;
            size_t cut = SIZE_MAX, i = vscan;
            for (; i + 4 <= a.size(); ++i) {
                if (a[i] || a[i + 1] || a[i + 2] != 1) continue;
                const uint8_t c = a[i + 3];
                const bool pic = avc ? (c & 0x1F) == 9 : c == 0x00;
                const bool hdr = !avc && (c == 0xB3 || c == 0xB8);
                const bool end = !avc && c == 0xB7;
                if (!vstarted) {
                    if (!pic && !hdr) continue;
                    video.take(i);
                    vts = video.begin();
                    vstarted = true;
                    vpic = pic;
                    i = 3;
                    continue;
                }
                if (end && vpic) { cut = i + 4; break; }
                if ((pic || hdr) && vpic) { cut = i; break; }
                vpic = vpic || pic;
            }
            if (cut == SIZE_MAX && eof && vstarted && !a.empty()) cut = a.size();
            if (cut != SIZE_MAX) {
                au.assign(a.begin(), a.begin() + (ptrdiff_t)cut);
                ts = vts;
                video.take(cut);
                vstarted = vpic = false;
                vscan = 0;
                return true;
            }
            vscan = i;
            if (!vstarted && a.size() > 3) { video.take(a.size() - 3); vscan = 0; }
            if (eof || !step()) return false;
        }
    }

    /* Next AC3 sync frame. */
    bool next_audio(std::vector<uint8_t>& au, uint64_t& ts)
    {
        for (;;) {
            std::vector<uint8_t>& a = audio.acc;
            size_t i = 0;
            while (i + 1 < a.size() && !(a[i] == 0x0B && a[i + 1] == 0x77)) ++i;
            if (i) { audio.take(i); continue; }
            if (a.size() >= 5) {
                const uint8_t fscod = a[4] >> 6, frm = a[4] & 0x3F;
                if (fscod > 2 || frm > 37) { audio.take(1); continue; }
                const size_t need = k_ac3_words[fscod][frm] * 2u;
                if (a.size() >= need) {
                    au.assign(a.begin(), a.begin() + (ptrdiff_t)need);
                    ts = audio.begin();
                    audio.take(need);
                    return true;
                }
            }
            if (eof || !step()) return false;
        }
    }
};

/* ---------------------------------------------------------------------------
 * Player state
 * -----------------------------------------------------------------------*/

struct Picture { std::vector<uint8_t> yuv; uint32_t w = 0, h = 0; uint64_t pts = 0; };

struct Player {
    uint32_t ea = 0;
    uint32_t cb = 0, cb_arg = 0;                  /* player callback (OPD) and its arg */
    uint32_t alloc_opd = 0, free_opd = 0, alloc_arg = 0;
    /* sound adapter: makeup, cleanup, formatChanged; graphics adapter: makeup,
     * cleanup, formatChanged, allocFrame, freeFrame */
    uint32_t sa = 0, sa_cb[3] = {}, sa_arg = 0;
    uint32_t ga = 0, ga_cb[5] = {}, ga_arg = 0;
    uint32_t scratch = 0;                         /* guest memory for formats and lists */
    uint32_t audio_buf[2] = {};
    uint32_t desc = 0;
    std::string uri;
    uint32_t repeat = 0;

    std::thread worker;
    std::mutex m;
    std::condition_variable cv;
    std::deque<uint32_t> cmds;                    /* CALL_* to run on the worker */
    uint32_t states_seen = 0;                     /* bit per ST_* reported since the last post */
    bool quit = false;

    /* stream (worker-owned unless noted) */
    PamfFile file;
    Demux demux;
    VdecFf* vdec = nullptr;
    bool vdec_avc = false;
    AdecFf* adec = nullptr;
    uint64_t next_vpts = kNoPts, vdur = 3003;
    std::vector<uint64_t> au_pts;                 /* by decode token */
    uint32_t token = 0;

    /* shared under m */
    bool open = false, running = false, paused = false, eos_sent = false;
    uint32_t session = 0;
    std::deque<Picture> pics;
    bool vdone = false;                           /* decoder drained this session */
    std::vector<float> pcm;                       /* interleaved, host floats */
    size_t pcm_head = 0;
    bool adone = false;
    uint32_t channels = 0, rate = 48000;
    uint64_t audio_pts = kNoPts;                  /* PTS of pcm[pcm_head] */
    uint32_t frames[4] = {}, nframes = 0, next_frame = 0;
    uint32_t width = 0, height = 0, fps_code = 3;
    std::chrono::steady_clock::time_point t0;
    std::chrono::steady_clock::duration paused_for{};
    std::chrono::steady_clock::time_point paused_at;
    uint32_t shown_buf = 0;
    uint64_t shown_pts = kNoPts;
    uint64_t shown_end = 0;                       /* PTS the shown picture lasts until */
    uint32_t gtag = 0, stag = 0;                  /* GetFrame call counters */
};

Player g_p;

void event(Player& p, uint32_t major, uint32_t minor, uint64_t p0)
{
    SLOG("event %u:%u p0=0x%llX\n", major, minor, (unsigned long long)p0);
    ppu_guest_call(p.cb, p.cb_arg, (uint64_t)major << 32 | minor, p0, 0, 0, 0, 0, 0);
}

void state(Player& p, uint32_t st)
{
    event(p, EV_STATE_CHANGED, 0, st);
    std::lock_guard<std::mutex> lock(p.m);
    p.states_seen |= 1u << st;
    p.cv.notify_all();
}

/* Queue a call for the worker; with `wait_state`, return once the worker has
 * reported that transition state (libsail's calls block the same way). */
void post(Player& p, uint32_t call, uint32_t wait_state)
{
    std::unique_lock<std::mutex> lock(p.m);
    p.cmds.push_back(call);
    p.states_seen = 0;
    p.cv.notify_all();
    if (wait_state)
        if (!p.cv.wait_for(lock, std::chrono::seconds(3), [&] { return (p.states_seen >> wait_state) & 1; }))
            SLOG("call %u: timed out waiting for state %u\n", call, wait_state);
}

uint32_t galloc(Player& p, uint32_t align, uint32_t size)
{
    return (uint32_t)ppu_guest_call(p.alloc_opd, p.alloc_arg, align, size, 0, 0, 0, 0, 0);
}

void gfree(Player& p, uint32_t ea)
{
    if (ea) ppu_guest_call(p.free_opd, p.alloc_arg, ea, 0, 0, 0, 0, 0, 0);
}

/* ---- decoding (worker) ---- */

void on_picture(void* ctx, const VdecFfPicture* pic)
{
    Player& p = *(Player*)ctx;
    Picture out;
    out.w = pic->width; out.h = pic->height;
    const size_t n = (size_t)pic->width * pic->height + 2 * (size_t)((pic->width + 1) / 2) * ((pic->height + 1) / 2);
    out.yuv.assign(pic->yuv, pic->yuv + n);
    std::free(pic->yuv);
    uint64_t ts = pic->token < p.au_pts.size() ? p.au_pts[pic->token] : kNoPts;
    if (ts == kNoPts) ts = p.next_vpts;
    if (ts == kNoPts) ts = p.file.start_pts;
    p.next_vpts = ts + p.vdur;
    out.pts = ts;
    std::lock_guard<std::mutex> lock(p.m);
    p.pics.push_back(std::move(out));
}

void on_audio(void* ctx, const AdecFfFrame* f)
{
    Player& p = *(Player*)ctx;
    std::lock_guard<std::mutex> lock(p.m);
    if (!p.channels) p.channels = (uint32_t)std::min<int>(f->channels, kMaxChannels);
    if (f->rate > 0) p.rate = (uint32_t)f->rate;
    const uint32_t ch = p.channels;
    /* FFmpeg's native order (FL FR FC LFE BL BR) is the game's layout 8
     * order (L R C LFE Ls Rs); stereo and mono pass through. */
    for (int i = 0; i < f->samples; ++i)
        for (uint32_t c = 0; c < ch; ++c)
            p.pcm.push_back(c < (uint32_t)f->channels ? f->plane[c][i] : 0.0f);
}

/* Decode until the queues are full or the payload is done. Returns true if
 * it did something. */
bool decode_some(Player& p)
{
    size_t npics, nsamples;
    {
        std::lock_guard<std::mutex> lock(p.m);
        npics = p.pics.size();
        nsamples = p.channels ? (p.pcm.size() - p.pcm_head) / p.channels : 0;
    }
    bool did = false;
    std::vector<uint8_t> au;
    uint64_t ts;
    if (npics < kVideoAhead && !p.vdone) {
        if (p.demux.next_video(au, ts)) {
            if (p.au_pts.size() <= p.token) p.au_pts.resize(p.token + 1, kNoPts);
            p.au_pts[p.token] = ts;
            if (p.vdec) vdec_ff_decode(p.vdec, au.data(), (uint32_t)au.size(), p.token, on_picture, &p);
            ++p.token;
        } else {
            if (p.vdec) vdec_ff_drain(p.vdec, on_picture, &p);
            std::lock_guard<std::mutex> lock(p.m);
            p.vdone = true;
        }
        did = true;
    }
    if (nsamples < kAudioAhead && !p.adone) {
        if (p.demux.next_audio(au, ts)) {
            {
                std::lock_guard<std::mutex> lock(p.m);
                if (p.audio_pts == kNoPts && ts != kNoPts) p.audio_pts = ts;
            }
            if (p.adec) adec_ff_decode(p.adec, au.data(), (uint32_t)au.size(), on_audio, &p);
        } else {
            std::lock_guard<std::mutex> lock(p.m);
            p.adone = true;
        }
        did = true;
    }
    return did;
}

/* Start the stream over (open, and every repeat). */
void rewind_stream(Player& p)
{
    p.demux.reset(&p.file);
    if (p.vdec) vdec_ff_reset(p.vdec);
    if (p.adec) adec_ff_reset(p.adec);
    p.au_pts.clear();
    p.token = 0;
    p.next_vpts = kNoPts;
    std::lock_guard<std::mutex> lock(p.m);
    p.pics.clear();
    p.pcm.clear();
    p.pcm_head = 0;
    p.audio_pts = kNoPts;
    p.vdone = p.adone = false;
    p.eos_sent = false;
}

uint32_t video_format(Player& p, uint32_t ea)
{
    for (uint32_t i = 0; i < 0x20; i += 4) vm_write32(ea + i, 0);
    w8(ea + 0, 0);                                /* ARGB */
    w8(ea + 1, 0);                                /* progressive */
    w8(ea + 2, 8);                                /* bits per colour */
    w8(ea + 3, (uint8_t)p.fps_code);
    w16(ea + 4, (uint16_t)p.width);
    w16(ea + 6, (uint16_t)p.height);
    vm_write32(ea + 8, p.width * 4);              /* pitch */
    vm_write32(ea + 0xC, 0xFF);                   /* alpha */
    w8(ea + 0x10, 0xFF);
    w8(ea + 0x11, 1);
    return ea;
}

uint32_t channel_layout(uint32_t ch) { return ch == 6 ? 8 : ch == 1 ? 1 : 2; }

uint32_t audio_format(Player& p, uint32_t ea, uint16_t samples)
{
    for (uint32_t i = 0; i < 0x18; i += 4) vm_write32(ea + i, 0);
    w8(ea + 0, 1);                                /* float */
    w8(ea + 1, (uint8_t)p.channels);
    w16(ea + 2, samples);
    vm_write32(ea + 4, p.rate);
    vm_write32(ea + 8, channel_layout(p.channels));
    return ea;
}

uint32_t fps_code(uint64_t dur)
{
    struct { uint64_t d; uint32_t code; } k[] = {
        { 3754, 0 }, { 3750, 1 }, { 3600, 2 }, { 3003, 3 }, { 3000, 4 }, { 1800, 5 }, { 1502, 6 }, { 1500, 7 } };
    for (auto& e : k) if (dur + 2 >= e.d && dur <= e.d + 2) return e.code;
    return 3;
}

void do_open(Player& p)
{
    state(p, ST_OPEN_TRANSITION);
    char host[1024] = {};
    std::string path = p.uri;
    const std::string scheme = "x-cell-fs://";
    if (path.compare(0, scheme.size(), scheme) == 0) path = path.substr(scheme.size());
    cellfs_translate_path(path.c_str(), host, sizeof host);
    const bool ok = p.file.load(host);
    SLOG("open %s -> %s (%s) start=0x%llX end=0x%llX\n", p.uri.c_str(), host, ok ? "ok" : "FAILED",
         (unsigned long long)p.file.start_pts, (unsigned long long)p.file.end_pts);
    if (p.vdec && p.vdec_avc != p.file.avc) { vdec_ff_close(p.vdec); p.vdec = nullptr; }
    if (!p.vdec) { p.vdec = vdec_ff_open(p.file.avc ? kVdecAvc : kVdecMpeg2); p.vdec_avc = p.file.avc; }
    if (!p.adec) p.adec = adec_ff_open(kAdecAc3);
    if (ok) {
        rewind_stream(p);
        /* Frame duration: MPEG-2 states it in the sequence header (only one
         * picture per GOP carries a PTS here); H.264 streams are taken to be
         * 29.97 Hz. Then decode up to the first picture and the first audio
         * so the formats are real. */
        p.vdur = 3003;
        if (!p.file.avc) {
            Demux d = p.demux;
            while (d.video.acc.size() < 12 && d.step()) {}
            const std::vector<uint8_t>& a = d.video.acc;
            for (size_t i = 0; i + 8 <= a.size(); ++i)
                if (a[i] == 0 && a[i + 1] == 0 && a[i + 2] == 1 && a[i + 3] == 0xB3) {
                    static const uint16_t k_dur[9] = { 3003, 3754, 3750, 3600, 3003, 3000, 1800, 1502, 1500 };
                    const uint8_t code = a[i + 7] & 0x0F;
                    if (code >= 1 && code <= 8) p.vdur = k_dur[code];
                    break;
                }
        }
        for (int i = 0; i < 4000; ++i) {
            {
                std::lock_guard<std::mutex> lock(p.m);
                if ((!p.pics.empty() || p.vdone) && (p.channels || p.adone)) break;
            }
            if (!decode_some(p)) break;
        }
    }
    {
        std::lock_guard<std::mutex> lock(p.m);
        if (!p.pics.empty()) { p.width = p.pics.front().w; p.height = p.pics.front().h; }
        if (!p.width) { p.width = 1280; p.height = 720; }
        if (!p.channels) p.channels = 2;
        p.fps_code = fps_code(p.vdur);
    }
    if (p.sa_cb[2]) ppu_guest_call(p.sa_cb[2], p.sa_arg, audio_format(p, p.scratch, 0x600), 1, 0, 0, 0, 0, 0);
    if (p.ga_cb[3]) {
        const uint32_t list = p.scratch + 0x40;
        for (uint32_t i = 0; i < 4; ++i) vm_write32(list + i * 4, 0);
        const int32_t n = (int32_t)ppu_guest_call(p.ga_cb[3], p.ga_arg, p.width * p.height * 4, 4, list, 0, 0, 0, 0);
        std::lock_guard<std::mutex> lock(p.m);
        p.nframes = n > 0 ? (uint32_t)std::min(n, 4) : 0;
        for (uint32_t i = 0; i < p.nframes; ++i) p.frames[i] = vm_read32(list + i * 4);
        p.next_frame = 0;
    }
    if (p.ga_cb[2]) ppu_guest_call(p.ga_cb[2], p.ga_arg, video_format(p, p.scratch), 1, 0, 0, 0, 0, 0);
    event(p, EV_ES_OPENED, 0, 0);
    event(p, EV_ES_OPENED, 0, 1);
    event(p, EV_STREAM_OPENED, 0, p.desc);
    {
        std::lock_guard<std::mutex> lock(p.m);
        p.open = true;
    }
    state(p, ST_OPENED);
    event(p, EV_CALL_COMPLETED, CALL_OPEN_STREAM, 0);
}

void begin_session(Player& p)
{
    std::lock_guard<std::mutex> lock(p.m);
    p.session += kSessionStep;
    p.t0 = std::chrono::steady_clock::now();
    p.paused_for = {};
    p.paused_at = p.t0;
}

void do_start(Player& p)
{
    state(p, ST_START_TRANSITION);
    begin_session(p);
    event(p, EV_SESSION_STARTED, 0, p.session);
    {
        std::lock_guard<std::mutex> lock(p.m);
        p.running = true;
    }
    state(p, ST_RUNNING);
    event(p, EV_CALL_COMPLETED, CALL_START, 0);
}

void do_stop(Player& p)
{
    state(p, ST_STOP_TRANSITION);
    {
        std::lock_guard<std::mutex> lock(p.m);
        p.running = false;
    }
    state(p, ST_OPENED);
    event(p, EV_CALL_COMPLETED, CALL_STOP, 0);
}

void do_close(Player& p)
{
    state(p, ST_CLOSE_TRANSITION);
    uint32_t n;
    {
        std::lock_guard<std::mutex> lock(p.m);
        p.open = p.running = false;
        n = p.nframes;
        for (uint32_t i = 0; i < n; ++i) vm_write32(p.scratch + 0x40 + i * 4, p.frames[i]);
        p.nframes = 0;
        p.shown_buf = 0;
        p.shown_pts = kNoPts;
        p.pics.clear();
        p.pcm.clear();
        p.pcm_head = 0;
    }
    if (p.ga_cb[4]) ppu_guest_call(p.ga_cb[4], p.ga_arg, n, p.scratch + 0x40, 0, 0, 0, 0, 0);
    p.file.data.clear();
    p.file.data.shrink_to_fit();
    event(p, EV_ES_CLOSED, 0, 0);
    event(p, EV_ES_CLOSED, 0, 1);
    event(p, EV_STREAM_CLOSED, 0, p.desc);
    state(p, ST_CLOSED);
    event(p, EV_CALL_COMPLETED, CALL_CLOSE_STREAM, 0);
}

/* End of the stream: both queues ran dry after the payload ended. */
bool at_eos(Player& p)
{
    std::lock_guard<std::mutex> lock(p.m);
    if (!p.running || p.eos_sent || !p.vdone || !p.adone || !p.pics.empty()) return false;
    if (p.channels && p.pcm.size() - p.pcm_head >= (size_t)kAudioSamples * p.channels) return false;
    const auto now = std::chrono::steady_clock::now();
    const auto el = now - p.t0 - p.paused_for - (p.paused ? now - p.paused_at : std::chrono::steady_clock::duration{});
    const uint64_t due = p.file.start_pts + (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(el).count() * 9 / 100;
    if (p.shown_pts != kNoPts && due < p.shown_end) return false;
    p.eos_sent = true;
    return true;
}

void worker_main(Player* pp)
{
    Player& p = *pp;
    if (p.sa_cb[0]) ppu_guest_call(p.sa_cb[0], p.sa_arg, 0, 0, 0, 0, 0, 0, 0);
    if (p.ga_cb[0]) ppu_guest_call(p.ga_cb[0], p.ga_arg, 0, 0, 0, 0, 0, 0, 0);
    state(p, ST_CLOSED);
    event(p, EV_CALL_COMPLETED, CALL_BOOT, 0);
    for (;;) {
        uint32_t call = 0;
        bool running;
        {
            std::unique_lock<std::mutex> lock(p.m);
            p.cv.wait_for(lock, std::chrono::milliseconds(4), [&] { return p.quit || !p.cmds.empty(); });
            if (p.quit) break;
            if (!p.cmds.empty()) { call = p.cmds.front(); p.cmds.pop_front(); }
            running = p.running;
        }
        switch (call) {
        case CALL_OPEN_STREAM:  do_open(p); continue;
        case CALL_START:        do_start(p); continue;
        case CALL_STOP:         do_stop(p); continue;
        case CALL_CLOSE_STREAM: do_close(p); continue;
        default: break;
        }
        if (!running) continue;
        for (int i = 0; i < 8 && decode_some(p); ++i) {}
        if (at_eos(p)) {
            event(p, EV_SOURCE_EOS, 0, 0);
            if (p.repeat) {
                rewind_stream(p);
                begin_session(p);
                event(p, EV_SESSION_STARTED, 0, p.session);
            }
        }
    }
    if (p.sa_cb[1]) ppu_guest_call(p.sa_cb[1], p.sa_arg, 0, 0, 0, 0, 0, 0, 0);
    if (p.ga_cb[1]) ppu_guest_call(p.ga_cb[1], p.ga_arg, 0, 0, 0, 0, 0, 0, 0);
}

/* ---------------------------------------------------------------------------
 * cellSail calls
 * -----------------------------------------------------------------------*/

/* (allocator, funcs{alloc, free}, arg) */
void MemAllocatorInitialize(ppu_context* c)
{
    const uint32_t self = (uint32_t)c->gpr[3], funcs = (uint32_t)c->gpr[4];
    if (!self || !funcs) { ret(c, kErrInvalidArg); return; }
    vm_write32(self + 0, vm_read32(funcs + 0));
    vm_write32(self + 4, vm_read32(funcs + 4));
    vm_write32(self + 8, (uint32_t)c->gpr[5]);
    ret(c, 0);
}

/* (player, allocator, callback, callbackArg, attribute, resource) */
void PlayerInitialize2(ppu_context* c)
{
    Player& p = g_p;
    const uint32_t alloc = (uint32_t)c->gpr[4];
    if (!c->gpr[3] || !alloc || !c->gpr[5]) { ret(c, kErrInvalidArg); return; }
    p.ea = (uint32_t)c->gpr[3];
    p.cb = (uint32_t)c->gpr[5];
    p.cb_arg = (uint32_t)c->gpr[6];
    p.alloc_opd = vm_read32(alloc + 0);
    p.free_opd = vm_read32(alloc + 4);
    p.alloc_arg = vm_read32(alloc + 8);
    p.repeat = 0;
    p.session = 0;
    SLOG("PlayerInitialize2 player=0x%08X cb=0x%08X arg=0x%08X\n", p.ea, p.cb, p.cb_arg);
    ret(c, 0);
}

/* (adapter, callbacks, arg) */
void SoundAdapterInitialize(ppu_context* c)
{
    const uint32_t cbs = (uint32_t)c->gpr[4];
    if (!c->gpr[3] || !cbs) { ret(c, kErrInvalidArg); return; }
    g_p.sa = (uint32_t)c->gpr[3];
    for (int i = 0; i < 3; ++i) g_p.sa_cb[i] = vm_read32(cbs + i * 4);
    g_p.sa_arg = (uint32_t)c->gpr[5];
    ret(c, 0);
}

void GraphicsAdapterInitialize(ppu_context* c)
{
    const uint32_t cbs = (uint32_t)c->gpr[4];
    if (!c->gpr[3] || !cbs) { ret(c, kErrInvalidArg); return; }
    g_p.ga = (uint32_t)c->gpr[3];
    for (int i = 0; i < 5; ++i) g_p.ga_cb[i] = vm_read32(cbs + i * 4);
    g_p.ga_arg = (uint32_t)c->gpr[5];
    ret(c, 0);
}

/* The game asks for ARGB, any size; that is what it gets. */
void GraphicsAdapterSetPreferredFormat(ppu_context* c) { ret(c, 0); }
void PlayerSetSoundAdapter(ppu_context* c) { ret(c, 0); }
void PlayerSetGraphicsAdapter(ppu_context* c) { ret(c, 0); }
void AdapterFinalize(ppu_context* c) { ret(c, 0); }

void PlayerBoot(ppu_context* c)
{
    Player& p = g_p;
    if (!p.cb || p.worker.joinable()) { ret(c, kErrInvalidState); return; }
    p.scratch = galloc(p, 0x80, 0x100);
    for (uint32_t& b : p.audio_buf) b = galloc(p, 0x80, kAudioSamples * kMaxChannels * 4);
    p.quit = false;
    p.worker = std::thread(worker_main, &p);
    ret(c, 0);
}

void PlayerFinalize(ppu_context* c)
{
    Player& p = g_p;
    if (p.worker.joinable()) {
        {
            std::lock_guard<std::mutex> lock(p.m);
            p.quit = true;
            p.cv.notify_all();
        }
        p.worker.join();
    }
    if (p.vdec) { vdec_ff_close(p.vdec); p.vdec = nullptr; }
    if (p.adec) { adec_ff_close(p.adec); p.adec = nullptr; }
    for (uint32_t& b : p.audio_buf) { gfree(p, b); b = 0; }
    gfree(p, p.scratch);
    p.scratch = 0;
    ret(c, 0);
}

/* (player, streamType, mediaInfo, uri, &descriptor) */
void PlayerCreateDescriptor(ppu_context* c)
{
    Player& p = g_p;
    const uint32_t uri = (uint32_t)c->gpr[6], out = (uint32_t)c->gpr[7];
    if (!uri || !out) { ret(c, kErrInvalidArg); return; }
    p.uri.assign((const char*)vm_base + uri, strnlen((const char*)vm_base + uri, 1024));
    p.desc = galloc(p, 8, 0x280);
    if (p.desc) for (uint32_t i = 0; i < 0x280; i += 4) vm_write32(p.desc + i, 0);
    vm_write32(out, p.desc);
    SLOG("CreateDescriptor %s -> 0x%08X\n", p.uri.c_str(), p.desc);
    ret(c, p.desc ? 0 : kErrInvalidState);
}

void PlayerDestroyDescriptor(ppu_context* c)
{
    Player& p = g_p;
    if ((uint32_t)c->gpr[4] == p.desc) { gfree(p, p.desc); p.desc = 0; }
    ret(c, 0);
}

/* Returns the descriptor count, as libsail did. */
void PlayerAddDescriptor(ppu_context* c) { ret(c, 1); }

void PlayerOpenStream(ppu_context* c)  { post(g_p, CALL_OPEN_STREAM, ST_OPEN_TRANSITION); ret(c, 0); }
void PlayerStart(ppu_context* c)       { post(g_p, CALL_START, ST_START_TRANSITION); ret(c, 0); }
void PlayerStop(ppu_context* c)        { post(g_p, CALL_STOP, ST_STOP_TRANSITION); ret(c, 0); }
void PlayerCloseStream(ppu_context* c) { post(g_p, CALL_CLOSE_STREAM, ST_CLOSE_TRANSITION); ret(c, 0); }

/* (player, mode, command): returns the mode, as libsail did. */
void PlayerSetRepeatMode(ppu_context* c)
{
    g_p.repeat = (uint32_t)c->gpr[4];
    ret(c, (int32_t)g_p.repeat);
}

void PlayerGetRepeatMode(ppu_context* c) { ret(c, (int32_t)g_p.repeat); }

void PlayerSetPaused(ppu_context* c)
{
    Player& p = g_p;
    const bool pause = (c->gpr[4] & 0xFF) != 0;
    std::lock_guard<std::mutex> lock(p.m);
    const auto now = std::chrono::steady_clock::now();
    if (pause && !p.paused) p.paused_at = now;
    if (!pause && p.paused) p.paused_for += now - p.paused_at;
    p.paused = pause;
    ret(c, 0);
}

/* PAMF is the only stream type there is here (CELL_SAIL_STREAM_PAMF = 0). */
void DescriptorGetStreamType(ppu_context* c) { ret(c, 0); }

/* (descriptor, buffer, size, arg): the "database" is what the game hands to
 * cellPamfReaderGetPresentation{Start,End}Time; ours carries those times. */
constexpr uint32_t kDbMagic = 0x544A5044;         /* 'TJPD' */
void DescriptorCreateDatabase(ppu_context* c)
{
    Player& p = g_p;
    const uint32_t db = (uint32_t)c->gpr[4], size = (uint32_t)c->gpr[5];
    if (!db || size < 0x18) { ret(c, kErrInvalidArg); return; }
    uint64_t start = p.file.start_pts, end = p.file.end_pts;
    if (p.file.data.empty()) {                    /* not opened yet: read the header */
        PamfFile f;
        char host[1024] = {};
        std::string path = p.uri;
        if (path.compare(0, 12, "x-cell-fs://") == 0) path = path.substr(12);
        cellfs_translate_path(path.c_str(), host, sizeof host);
        if (f.load(host)) { start = f.start_pts; end = f.end_pts; }
    }
    for (uint32_t i = 0; i < size; i += 4) vm_write32(db + i, 0);
    vm_write32(db + 0, kDbMagic);
    w64(db + 8, start);
    w64(db + 0x10, end);
    ret(c, 0);
}

void DescriptorDestroyDatabase(ppu_context* c) { ret(c, 0); }

void PamfGetPresentationStartTime(ppu_context* c)
{
    const uint32_t db = (uint32_t)c->gpr[3], out = (uint32_t)c->gpr[4];
    if (!db || !out || vm_read32(db) != kDbMagic) { ret(c, kPamfErrInvalidArg); return; }
    vm_write32(out, vm_read32(db + 8));
    vm_write32(out + 4, vm_read32(db + 12));
    ret(c, 0);
}

void PamfGetPresentationEndTime(ppu_context* c)
{
    const uint32_t db = (uint32_t)c->gpr[3], out = (uint32_t)c->gpr[4];
    if (!db || !out || vm_read32(db) != kDbMagic) { ret(c, kPamfErrInvalidArg); return; }
    vm_write32(out, vm_read32(db + 0x10));
    vm_write32(out + 4, vm_read32(db + 0x14));
    ret(c, 0);
}

/* Frame info, 0x18 bytes: buffer, session, call tag, status, PTS. */
void frame_info(uint32_t ea, uint32_t buf, uint32_t session, uint32_t tag, uint64_t pts)
{
    vm_write32(ea + 0, buf);
    vm_write32(ea + 4, session);
    vm_write32(ea + 8, tag);
    vm_write32(ea + 0xC, 0);
    w64(ea + 0x10, pts);
}

/* (adapter, &info) */
void GraphicsAdapterGetFrame(ppu_context* c)
{
    Player& p = g_p;
    const uint32_t info = (uint32_t)c->gpr[4];
    std::unique_lock<std::mutex> lock(p.m);
    const uint32_t tag = ++p.gtag;
    if (!p.open || !p.nframes) {
        if (info) frame_info(info, 0, p.session, tag, kNoPts);
        ret(c, kFrameNoStream);
        return;
    }
    Picture pic;
    bool fresh = false;
    if (p.running) {
        const auto now = std::chrono::steady_clock::now();
        const auto el = now - p.t0 - p.paused_for - (p.paused ? now - p.paused_at : std::chrono::steady_clock::duration{});
        const uint64_t due = p.file.start_pts + (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(el).count() * 9 / 100;
        /* The newest picture that is due; older ones are dropped. */
        while (!p.pics.empty() && p.pics.front().pts <= due) {
            pic = std::move(p.pics.front());
            p.pics.pop_front();
            fresh = true;
        }
    }
    if (fresh) {
        { static unsigned n; if (n++ % 30 == 0) SLOG("video frame pts=0x%llX (%zu queued) tag=%u\n", (unsigned long long)pic.pts, p.pics.size(), tag); }
        const uint32_t buf = p.frames[p.next_frame];
        p.next_frame = (p.next_frame + 1) % p.nframes;
        p.shown_buf = buf;
        p.shown_pts = pic.pts;
        p.shown_end = pic.pts + p.vdur;
        const uint32_t session = p.session;
        lock.unlock();
        if (pic.w == p.width && pic.h == p.height)
            vdec_store_picture(buf, pic.yuv.data(), pic.w, pic.h, 0, 1, 0xFF);
        if (info) frame_info(info, buf, session, tag, pic.pts);
        ret(c, kFrameNew);
        return;
    }
    if (p.shown_buf) {
        if (info) frame_info(info, p.shown_buf, p.session, tag, p.shown_pts);
        ret(c, kFrameSame);
        return;
    }
    if (info) frame_info(info, 0, p.session, tag, kNoPts);
    ret(c, kFrameNone);
}

/* (adapter, samples, &info): `samples` interleaved float frames in the
 * stream's channel count, big-endian. */
void SoundAdapterGetFrame(ppu_context* c)
{
    Player& p = g_p;
    const uint32_t want = std::min<uint32_t>((uint32_t)c->gpr[4], kAudioSamples);
    const uint32_t info = (uint32_t)c->gpr[5];
    std::lock_guard<std::mutex> lock(p.m);
    const uint32_t tag = ++p.stag;
    const size_t ch = p.channels;
    if (!p.open || !p.running || p.paused || !ch || !want || !p.audio_buf[0] ||
        p.pcm.size() - p.pcm_head < want * ch) {
        if (info) frame_info(info, 0, p.open ? p.session : 0, tag, kNoPts);
        ret(c, kFrameNoStream);
        return;
    }
    { static unsigned n; if (n++ % 400 == 0) SLOG("audio frame pts=0x%llX ch=%zu\n", (unsigned long long)(p.audio_pts == kNoPts ? 0 : p.audio_pts), ch); }
    const uint32_t buf = p.audio_buf[tag & 1];
    uint8_t* o = vm_base + buf;
    for (size_t i = 0; i < want * ch; ++i) {
        float v = p.pcm[p.pcm_head + i];
        v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        o[0] = (uint8_t)(bits >> 24); o[1] = (uint8_t)(bits >> 16); o[2] = (uint8_t)(bits >> 8); o[3] = (uint8_t)bits;
        o += 4;
    }
    const uint64_t pts = p.audio_pts == kNoPts ? p.file.start_pts : p.audio_pts;
    p.pcm_head += want * ch;
    p.audio_pts = pts + (uint64_t)want * 90000 / p.rate;
    if (p.pcm_head > (1u << 20)) { p.pcm.erase(p.pcm.begin(), p.pcm.begin() + (ptrdiff_t)p.pcm_head); p.pcm_head = 0; }
    if (info) frame_info(info, buf, p.session, tag, pts);
    ret(c, kFrameNew);
}

void AdapterUpdateAvSync(ppu_context* c) { ret(c, 0); }

void GraphicsAdapterGetFormat(ppu_context* c)
{
    if (!c->gpr[4]) { ret(c, kErrInvalidArg); return; }
    std::lock_guard<std::mutex> lock(g_p.m);
    video_format(g_p, (uint32_t)c->gpr[4]);
    ret(c, 0);
}

void SoundAdapterGetFormat(ppu_context* c)
{
    if (!c->gpr[4]) { ret(c, kErrInvalidArg); return; }
    std::lock_guard<std::mutex> lock(g_p.m);
    audio_format(g_p, (uint32_t)c->gpr[4], kAudioSamples);
    ret(c, 0);
}

/* (adapter, pts, &usec): microseconds since the presentation start. */
void AdapterPtsToTimePosition(ppu_context* c)
{
    const uint64_t pts = c->gpr[4];
    const uint32_t out = (uint32_t)c->gpr[5];
    if (!out) { ret(c, kErrInvalidArg); return; }
    const uint64_t start = g_p.file.start_pts;
    w64(out, pts > start ? (pts - start) * 1000000 / 90000 : 0);
    ret(c, 0);
}

} // namespace

extern "C" void tj_sail_hle_register(void)
{
    const char* e = std::getenv("TJ_SAIL_LOG");
    g_log = e && *e && *e != '0';
    struct { uint32_t nid; const char* name; void (*fn)(ppu_context*); } k[] = {
        { 0x346EBBA3u, "cellSailMemAllocatorInitialize", MemAllocatorInitialize },
        { 0x23654375u, "cellSailPlayerInitialize2", PlayerInitialize2 },
        { 0x18B4629Du, "cellSailPlayerFinalize", PlayerFinalize },
        { 0x3D0D3B72u, "cellSailSoundAdapterInitialize", SoundAdapterInitialize },
        { 0xD1462438u, "cellSailSoundAdapterFinalize", AdapterFinalize },
        { 0x7EB8D6B5u, "cellSailSoundAdapterGetFrame", SoundAdapterGetFrame },
        { 0xF25F197Du, "cellSailSoundAdapterGetFormat", SoundAdapterGetFormat },
        { 0xEEC22809u, "cellSailSoundAdapterUpdateAvSync", AdapterUpdateAvSync },
        { 0x4AE979DFu, "cellSailSoundAdapterPtsToTimePosition", AdapterPtsToTimePosition },
        { 0x1C983864u, "cellSailGraphicsAdapterInitialize", GraphicsAdapterInitialize },
        { 0x76488BB1u, "cellSailGraphicsAdapterFinalize", AdapterFinalize },
        { 0x2E3CCB5Eu, "cellSailGraphicsAdapterSetPreferredFormat", GraphicsAdapterSetPreferredFormat },
        { 0x0247C69Eu, "cellSailGraphicsAdapterGetFrame", GraphicsAdapterGetFrame },
        { 0xFFD58AA4u, "cellSailGraphicsAdapterGetFormat", GraphicsAdapterGetFormat },
        { 0x44A20E79u, "cellSailGraphicsAdapterUpdateAvSync", AdapterUpdateAvSync },
        { 0x1872331Bu, "cellSailGraphicsAdapterPtsToTimePosition", AdapterPtsToTimePosition },
        { 0x1139A206u, "cellSailPlayerSetSoundAdapter", PlayerSetSoundAdapter },
        { 0x18BCD21Bu, "cellSailPlayerSetGraphicsAdapter", PlayerSetGraphicsAdapter },
        { 0xBDF21B0Fu, "cellSailPlayerBoot", PlayerBoot },
        { 0x34ECC1B9u, "cellSailPlayerOpenStream", PlayerOpenStream },
        { 0x85BEFFCCu, "cellSailPlayerCloseStream", PlayerCloseStream },
        { 0xE535B0D3u, "cellSailPlayerStart", PlayerStart },
        { 0xEBA8D4ECu, "cellSailPlayerStop", PlayerStop },
        { 0xFC5BAF8Au, "cellSailPlayerSetRepeatMode", PlayerSetRepeatMode },
        { 0x38144ECFu, "cellSailPlayerGetRepeatMode", PlayerGetRepeatMode },
        { 0xD1D55A90u, "cellSailPlayerSetPaused", PlayerSetPaused },
        { 0xD7938B8Du, "cellSailPlayerCreateDescriptor", PlayerCreateDescriptor },
        { 0xFC839BD4u, "cellSailPlayerDestroyDescriptor", PlayerDestroyDescriptor },
        { 0x7C8DFF3Bu, "cellSailPlayerAddDescriptor", PlayerAddDescriptor },
        { 0x0ABB318Bu, "cellSailDescriptorCreateDatabase", DescriptorCreateDatabase },
        { 0x28336E89u, "cellSailDescriptorDestroyDatabase", DescriptorDestroyDatabase },
        { 0xD5F9A15Bu, "cellSailDescriptorGetStreamType", DescriptorGetStreamType },
        { 0x4DE501B1u, "cellPamfReaderGetPresentationStartTime", PamfGetPresentationStartTime },
        { 0xF61609D6u, "cellPamfReaderGetPresentationEndTime", PamfGetPresentationEndTime },
    };
    for (auto& r : k) ps3_hle_register_ctx(r.nid, r.name, r.fn);
}
