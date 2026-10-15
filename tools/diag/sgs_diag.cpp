/* sgs_diag.cpp -- DIAGNOSTIC ONLY, never for landing.
 *
 * Traces Tokyo Jungle's sgsfile library (its own file layer, lifted at
 * 0x230000-0x239600) to find out who frees or marks a file slot before the
 * loader asks sgsFileGetResult for it ("fileslot.c(241): 'sgsFileGetResult'
 * unknown file handle ID (1)", then BROKEN_EXIT_HDDGAME = white screen after
 * the tutorial). tools/diag/sgs_instrument.py wraps every lifted function in
 * that range with sgsdiag_enter/leave.
 *
 * Output dir: $SGSDIAG_OUT (default ./sgsdiag):
 *   events.txt  every wrapped call: seq, ms, host tid, guest tid, depth,
 *               function, lr, r3..r7 in, r3 out, strings the args point at;
 *               slot-table changes (CHG lines, from calls and from a 200 us
 *               poller that catches the request thread's own writes).
 *   On the first unknown-handle error: the whole slot table (slots.txt),
 *   guest main memory 0-256 MB (mem_00000000.bin) and the stack area
 *   (mem_D0000000.bin), then the run carries on as before.
 */
#include "ppu_recomp.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>

extern uint8_t* vm_base;

namespace {

const uint32_t SLOT_DESC_TOC = 0x0035B674;   /* [TOC 0x359208 + 0x246C] -> {+4 base, +8 count}; +0x2470 is the flags word */
const uint32_t SLOT_SIZE = 0x2A8;
const uint32_t ERR_FMT_UNKNOWN_HANDLE = 0x002A06E0;
const uint32_t FN_ERROR_PRINT = 0x002390F8;

std::mutex g_mu;
FILE* g_ev = nullptr;
std::string g_dir;
std::vector<uint8_t> g_prev;        /* last seen slot table */
uint32_t g_prev_base = 0, g_prev_count = 0;
std::atomic<uint64_t> g_seq{0};
std::atomic<int> g_dumped{0};
std::chrono::steady_clock::time_point g_t0;
thread_local int t_depth = 0;
uint32_t g_calls[0x10000];          /* per function (addr >> 2 & 0xFFFF) */
const uint32_t CALL_LOG_CAP = 200000;

bool guest_ok(uint32_t a, uint32_t n) { return a >= 0x10000 && (uint64_t)a + n <= 0x100000000ull; }

/* Copy guest bytes without faulting on unmapped pages (write() to a pipe
 * returns EFAULT instead of raising SIGSEGV). */
bool safe_read(uint32_t a, void* out, uint32_t n)
{
    static int pfd[2] = {-1, -1};
    if (pfd[0] < 0) { if (pipe(pfd)) return false; fcntl(pfd[0], F_SETFL, O_NONBLOCK); fcntl(pfd[1], F_SETFL, O_NONBLOCK); }
    if (!vm_base || !guest_ok(a, n) || n > 4096) return false;
    ssize_t w = write(pfd[1], vm_base + a, n);
    if (w != (ssize_t)n) { if (w > 0) { char junk[4096]; (void)!read(pfd[0], junk, (size_t)w); } return false; }
    return read(pfd[0], out, n) == (ssize_t)n;
}
uint32_t rd32(uint32_t a) { uint8_t b[4]; if (!safe_read(a, b, 4)) return 0xDEADDEAD; return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }

double ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_t0).count(); }

void open_out()
{
    if (g_ev) return;
    const char* d = getenv("SGSDIAG_OUT");
    g_dir = (d && *d) ? d : "sgsdiag";
    mkdir(g_dir.c_str(), 0755);
    g_ev = fopen((g_dir + "/events.txt").c_str(), "w");
    if (g_ev) setvbuf(g_ev, nullptr, _IOFBF, 1 << 20);
    g_t0 = std::chrono::steady_clock::now();
    fprintf(stderr, "[sgsdiag] tracing sgsfile lib -> %s/events.txt\n", g_dir.c_str());
}

/* A printable string at guest address a, or "". */
std::string gstr(uint32_t a)
{
    char buf[97]; if (!guest_ok(a, 96) || !safe_read(a, buf, 96)) return "";
    buf[96] = 0; int n = 0;
    while (n < 96 && buf[n] >= 0x20 && buf[n] < 0x7F) n++;
    if (n < 3 || buf[n] != 0) return "";
    return std::string(buf, n);
}

/* Diff the slot table against the last snapshot; caller holds g_mu. */
void diff_slots(const char* who)
{
    uint32_t desc = rd32(SLOT_DESC_TOC);
    if (desc == 0xDEADDEAD || !desc) return;
    uint32_t flags = rd32(desc), base = rd32(desc + 4), count = rd32(desc + 8);
    if (base == 0xDEADDEAD || count == 0xDEADDEAD || !base || count == 0 || count > 256) return;
    std::vector<uint8_t> cur(count * SLOT_SIZE);
    for (uint32_t off = 0; off < cur.size(); off += 4096) {
        uint32_t n = (uint32_t)std::min<size_t>(4096, cur.size() - off);
        if (!safe_read(base + off, cur.data() + off, n)) return;
    }
    if (base != g_prev_base || count != g_prev_count || g_prev.size() != cur.size()) {
        fprintf(g_ev, "%10.3f TABLE desc=0x%08X flags=0x%08X base=0x%08X count=%u (%s)\n", ms(), desc, flags, base, count, who);
        g_prev = cur; g_prev_base = base; g_prev_count = count;
        for (uint32_t s = 0; s < count; s++) {
            const uint8_t* p = &cur[s * SLOT_SIZE];
            fprintf(g_ev, "           slot %u: hdr %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n", s,
                    p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
        }
        return;
    }
    for (uint32_t s = 0; s < count; s++) {
        const uint8_t* a = &g_prev[s * SLOT_SIZE];
        const uint8_t* b = &cur[s * SLOT_SIZE];
        if (!memcmp(a, b, SLOT_SIZE)) continue;
        std::string line;
        char t[64];
        int shown = 0;
        for (uint32_t o = 0; o < SLOT_SIZE; o += 4) {
            if (memcmp(a + o, b + o, 4)) {
                if (shown++ < 24) {
                    snprintf(t, sizeof t, " +%03X:%02X%02X%02X%02X>%02X%02X%02X%02X", o, a[o],a[o+1],a[o+2],a[o+3], b[o],b[o+1],b[o+2],b[o+3]);
                    line += t;
                }
            }
        }
        fprintf(g_ev, "%10.3f CHG slot %u [%s] tid=%ld%s%s\n", ms(), s, who, (long)syscall(SYS_gettid), line.c_str(), shown > 24 ? " ..." : "");
        std::string path = gstr(base + s * SLOT_SIZE + 0x10);
        if (!path.empty()) fprintf(g_ev, "           slot %u path?+0x10 '%s'\n", s, path.c_str());
    }
    g_prev.swap(cur);
}

void dump_region(uint32_t start, uint32_t len, const char* name)
{
    std::string p = g_dir + "/" + name;
    int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    std::vector<uint8_t> zero(1 << 20, 0);
    for (uint32_t off = 0; off < len; off += (1u << 20)) {
        ssize_t w = write(fd, vm_base + start + off, 1u << 20);
        if (w != (1 << 20)) { lseek(fd, (off_t)off, SEEK_SET); (void)!write(fd, zero.data(), 1u << 20); }
    }
    close(fd);
    fprintf(g_ev, "%10.3f DUMP %s 0x%08X+0x%X\n", ms(), name, start, len);
}

void dump_all(const char* why)
{
    fprintf(g_ev, "%10.3f ===== %s: dumping =====\n", ms(), why);
    uint32_t desc = rd32(SLOT_DESC_TOC);
    uint32_t base = rd32(desc + 4), count = rd32(desc + 8);
    FILE* f = fopen((g_dir + "/slots.txt").c_str(), "w");
    if (f && count && count <= 256) {
        fprintf(f, "desc=0x%08X flags=0x%08X base=0x%08X count=%u\n", desc, rd32(desc), base, count);
        for (uint32_t s = 0; s < count; s++) {
            fprintf(f, "slot %u @0x%08X:\n", s, base + s * SLOT_SIZE);
            for (uint32_t o = 0; o < SLOT_SIZE; o += 16) {
                fprintf(f, "  +%03X:", o);
                for (uint32_t k = 0; k < 16 && o + k < SLOT_SIZE; k += 4) fprintf(f, " %08X", rd32(base + s * SLOT_SIZE + o + k));
                fprintf(f, "\n");
            }
        }
    }
    if (f) fclose(f);
    dump_region(0x00000000, 0x10000000, "mem_00000000.bin");
    dump_region(0xD0000000, 0x01000000, "mem_D0000000.bin");
    fflush(g_ev);
    fprintf(stderr, "[sgsdiag] %s: slot table + guest memory dumped to %s\n", why, g_dir.c_str());
}

void poller()
{
    for (;;) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        std::lock_guard<std::mutex> l(g_mu);
        if (g_ev) diff_slots("poll");
        static int n = 0;
        if (++n % 5000 == 0 && g_ev) fflush(g_ev);
    }
}

} // namespace

extern "C" void sgsdiag_enter(uint32_t fn, ppu_context* ctx, void** tok)
{
    static std::once_flag once;
    std::call_once(once, [] { std::lock_guard<std::mutex> l(g_mu); open_out(); std::thread(poller).detach(); });
    *tok = (void*)(uintptr_t)(++g_seq);
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_ev) return;
    uint32_t& c = g_calls[(fn >> 2) & 0xFFFF];
    c++;
    if (c <= CALL_LOG_CAP) {
        fprintf(g_ev, "%10.3f > #%llu tid=%ld g%llu d%d func_%08X lr=0x%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X",
                ms(), (unsigned long long)(uintptr_t)*tok, (long)syscall(SYS_gettid), (unsigned long long)ctx->thread_id,
                t_depth, fn, (uint32_t)ctx->lr, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5],
                (uint32_t)ctx->gpr[6], (uint32_t)ctx->gpr[7]);
        for (int r = 3; r <= 8; r++) {
            std::string s = gstr((uint32_t)ctx->gpr[r]);
            if (!s.empty()) fprintf(g_ev, " r%d='%s'", r, s.c_str());
        }
        fprintf(g_ev, "\n");
        if (c == CALL_LOG_CAP) fprintf(g_ev, "           (func_%08X: call log cap reached, slot diffs continue)\n", fn);
    }
    diff_slots("enter");
    if (fn == FN_ERROR_PRINT && (uint32_t)ctx->gpr[6] == ERR_FMT_UNKNOWN_HANDLE) {
        fprintf(g_ev, "%10.3f !!!!! UNKNOWN HANDLE error: r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X (id is the %%d arg) tid=%ld g%llu\n",
                ms(), (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6],
                (uint32_t)ctx->gpr[7], (uint32_t)ctx->gpr[8], (long)syscall(SYS_gettid), (unsigned long long)ctx->thread_id);
        if (!g_dumped.exchange(1)) dump_all("first unknown-handle error");
    }
    t_depth++;
}

extern "C" void sgsdiag_leave(uint32_t fn, ppu_context* ctx, void* tok)
{
    t_depth--;
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_ev) return;
    if (g_calls[(fn >> 2) & 0xFFFF] <= CALL_LOG_CAP)
        fprintf(g_ev, "%10.3f < #%llu func_%08X -> r3=%08X\n", ms(), (unsigned long long)(uintptr_t)tok, fn, (uint32_t)ctx->gpr[3]);
    diff_slots("leave");
}
