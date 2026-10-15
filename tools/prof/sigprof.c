/* Sampling profiler for the port, loaded with LD_PRELOAD (no ptrace needed;
 * this machine has kernel.yama.ptrace_scope=1, so perf-less attach tools
 * like eu-stack/gdb can't attach to a running game).
 *
 * Every PROF_HZ of process CPU time (ITIMER_PROF) the thread that was running
 * takes SIGPROF and its stack (glibc backtrace(), frame-pointer free) is written
 * as one record to PROF_OUT. Only samples between PROF_START_S and PROF_END_S
 * seconds after start are kept. Records are appended with write(), which is
 * async-signal-safe, so a SIGKILLed run still leaves a usable file.
 *
 *   cc -O2 -shared -fPIC -o build-runs/sigprof.so tools/prof/sigprof.c
 *   PROF_OUT=prof.bin PROF_START_S=160 PROF_END_S=220 \
 *     LD_PRELOAD=$PWD/build-runs/sigprof.so ./build/tokyojungle ...
 *   python3 tools/prof/report.py prof.bin build/tokyojungle
 *
 * File format: a text header "MAPS\n<copy of /proc/self/maps>\nEND\n", then
 * records: u32 tid, u32 depth, depth x u64 return addresses.
 *
 * Diagnostic only: it reads stacks, it never runs or changes guest code.
 */
#define _GNU_SOURCE
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>

#define MAX_DEPTH 48

static int s_fd = -1;
static double s_t0, s_start, s_end;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void on_prof(int sig, siginfo_t* si, void* uc)
{
    (void)sig; (void)si; (void)uc;
    if (s_fd < 0) return;
    const double t = now_s() - s_t0;
    if (t < s_start || t > s_end) return;
    struct { uint32_t tid, depth; uint64_t pc[MAX_DEPTH]; } rec;
    void* buf[MAX_DEPTH];
    const int n = backtrace(buf, MAX_DEPTH);
    rec.tid = (uint32_t)syscall(SYS_gettid);
    rec.depth = n > 0 ? (uint32_t)n : 0;
    for (int i = 0; i < n; i++) rec.pc[i] = (uint64_t)(uintptr_t)buf[i];
    (void)!write(s_fd, &rec, 8 + 8 * (size_t)rec.depth);
}

/* PROF_WALL_HZ=<hz>: wall-clock mode. Instead of sampling whoever burns CPU,
 * a helper thread signals EVERY thread of the process <hz> times a second, so
 * a thread blocked in a wait shows up with the stack of that wait (what is the
 * game thread waiting for?). Samples per thread / hz = seconds of wall time.
 * Blocking calls are restarted (SA_RESTART); a sleep may return early, which
 * perturbs timing slightly. */
static pid_t s_wall_tid;
static void* wall_thread(void* arg)
{
    const long hz = (long)(intptr_t)arg;
    s_wall_tid = (pid_t)syscall(SYS_gettid);
    const pid_t pid = getpid();
    struct timespec ts = { 0, 1000000000L / hz };
    for (;;) {
        nanosleep(&ts, NULL);
        /* Something in the process may reset SIGPROF (the default action
         * kills it); put the handler back before signalling anyone. */
        struct sigaction cur;
        if (sigaction(SIGPROF, NULL, &cur) == 0 && cur.sa_sigaction != on_prof) {
            static int warned;
            if (!warned++) {
                static const char m[] = "[sigprof] SIGPROF handler was replaced; reinstalling\n";
                (void)!write(2, m, sizeof m - 1);
            }
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_sigaction = on_prof;
            sa.sa_flags = SA_SIGINFO | SA_RESTART;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGPROF, &sa, NULL);
        }
        DIR* d = opendir("/proc/self/task");
        if (!d) continue;
        struct dirent* e;
        while ((e = readdir(d))) {
            const pid_t tid = (pid_t)atoi(e->d_name);
            if (tid > 0 && tid != s_wall_tid) syscall(SYS_tgkill, pid, tid, SIGPROF);
        }
        closedir(d);
    }
    return NULL;
}

__attribute__((constructor)) static void prof_init(void)
{
    const char* out = getenv("PROF_OUT");
    if (!out) return;
    s_t0 = now_s();
    s_start = getenv("PROF_START_S") ? atof(getenv("PROF_START_S")) : 0.0;
    s_end = getenv("PROF_END_S") ? atof(getenv("PROF_END_S")) : 1e30;
    const int hz = getenv("PROF_HZ") ? atoi(getenv("PROF_HZ")) : 997;
    s_fd = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (s_fd < 0) return;
    /* The mappings, so the report can turn PIE addresses into file offsets. */
    static const char hdr[] = "MAPS\n";
    (void)!write(s_fd, hdr, sizeof hdr - 1);
    const int m = open("/proc/self/maps", O_RDONLY);
    if (m >= 0) {
        char b[65536]; ssize_t r;
        while ((r = read(m, b, sizeof b)) > 0) (void)!write(s_fd, b, (size_t)r);
        close(m);
    }
    static const char end[] = "END\n";
    (void)!write(s_fd, end, sizeof end - 1);
    /* The first backtrace() loads libgcc_s; do it here, not in the handler. */
    void* warm[4];
    backtrace(warm, 4);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    const char* wall = getenv("PROF_WALL_HZ");
    if (wall && atoi(wall) > 0) {
        pthread_t th;
        pthread_create(&th, NULL, wall_thread, (void*)(intptr_t)atoi(wall));
        return;
    }
    struct itimerval it = { { 0, 1000000 / hz }, { 0, 1000000 / hz } };
    setitimer(ITIMER_PROF, &it, NULL);
}
