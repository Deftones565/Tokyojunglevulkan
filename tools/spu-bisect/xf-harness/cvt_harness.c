/* Build + run (from the repo root; HDR = dir with the spu_helpers.h under test):
 *   HDR=third_party/ps3recomp/runtime/spu; gcc -O2 -std=gnu17 -msse4.1 -mfma -ffp-contract=off \
 *     -Ithird_party/ps3recomp/include -I$HDR -Ithird_party/ps3recomp/runtime/spu \
 *     tools/spu-bisect/xf-harness/cvt_harness.c -lm -o /tmp/cvt_harness && /tmp/cvt_harness
 *
 * Bit-exactness check for patch 0062 (spu-pow2-no-libm): spu__xf_in, spu_fi
 * and spu_cflts/cfltu/csflt/cuflt of the header under test against verbatim
 * copies of the old ldexp/ldexpf/exp2 versions (below).
 *   spu__xf_in: all 2^32 inputs (as double bit patterns).
 *   spu_fi:     every b exponent x step field x sign, with sampled base
 *               mantissas and a values (and every a & 0x7FFFF for a few b).
 *   conversions: every i8 0..255 with special and random inputs.
 * Exit status 0 = no mismatch. */
#include <ps3emu/ps3types.h>
#include <spu_helpers.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ---- old code, verbatim (before 0062) ---- */
static inline double old_xf_in(uint32_t b) {
    const uint32_t e = (b >> 23) & 0xFFu;
    if (e == 0u) return (b & 0x80000000u) ? -0.0 : 0.0;
    const double v = ldexp(1.0 + (double)(b & 0x7FFFFFu) / 8388608.0, (int)e - 127);
    return (b & 0x80000000u) ? -v : v;
}
static inline u128 old_fi(u128 a, u128 b) {
    u128 r;
    for (int i = 0; i < 4; i++) {
        uint32_t rb = b._u32[i];
        uint32_t ra = a._u32[i];
        uint32_t exp = (rb >> 23) & 0xFFu;
        uint32_t base_bits = rb & 0xFFFFFC00u;
        float base; memcpy(&base, &base_bits, sizeof base);
        uint32_t step_frac = rb & 0x3FFu;
        float step = ldexpf((float)step_frac, (int)exp - 127 - 13);
        if (base < 0.0f) step = -step;
        float y = ldexpf((float)(ra & 0x7FFFFu), -19);
        r._f32[i] = base - step * y;
    }
    return r;
}
static inline u128 old_cflts(u128 a, int i8){ u128 r; double f=exp2((double)(173-i8));
    for(int i=0;i<4;i++){ double v=old_xf_in(a._u32[i])*f; if(v>2147483647.0)v=2147483647.0; if(v<-2147483648.0)v=-2147483648.0; r._s32[i]=(int32_t)v; } return r; }
static inline u128 old_cfltu(u128 a, int i8){ u128 r; double f=exp2((double)(173-i8));
    for(int i=0;i<4;i++){ double v=old_xf_in(a._u32[i])*f; if(v<0)v=0; if(v>4294967295.0)v=4294967295.0; r._u32[i]=(uint32_t)v; } return r; }
static inline u128 old_csflt(u128 a, int i8){ u128 r; double f=exp2((double)(i8-155));
    for(int i=0;i<4;i++) r._f32[i]=(float)((double)a._s32[i]*f); return r; }
static inline u128 old_cuflt(u128 a, int i8){ u128 r; double f=exp2((double)(i8-155));
    for(int i=0;i<4;i++) r._f32[i]=(float)((double)a._u32[i]*f); return r; }

/* ---- harness ---- */
static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint64_t rnd64(void) {
    uint64_t z = (rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static uint32_t rnd32(void) { return (uint32_t)rnd64(); }
static unsigned long long n_in, n_fi, n_cv, bad;
static void miss(const char* what, uint32_t a, uint32_t b, uint64_t o, uint64_t n) {
    if (bad++ < 12) printf("MISMATCH %s a=%08X b=%08X old=%016llX new=%016llX\n", what, a, b,
                           (unsigned long long)o, (unsigned long long)n);
}
static void chk128(const char* w, u128 x, uint32_t aux, u128 o, u128 n, unsigned long long* cnt) {
    for (int l = 0; l < 4; l++) {
        (*cnt)++;
        if (o._u32[l] != n._u32[l]) miss(w, x._u32[l], aux, o._u32[l], n._u32[l]);
    }
}
static u128 splat(uint32_t v) { u128 r; for (int l = 0; l < 4; l++) r._u32[l] = v; return r; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(void) {
    /* spu__xf_in: exhaustive */
    for (uint64_t b = 0; b <= 0xFFFFFFFFull; b++) {
        double o = old_xf_in((uint32_t)b), n = spu__xf_in((uint32_t)b);
        uint64_t ou, nu; memcpy(&ou, &o, 8); memcpy(&nu, &n, 8);
        n_in++;
        if (ou != nu) miss("xf_in", (uint32_t)b, 0, ou, nu);
    }
    /* spu_fi: every exponent x step field x sign; base mantissa and a sampled */
    for (uint32_t s = 0; s < 2; s++)
        for (uint32_t e = 0; e < 256; e++)
            for (uint32_t st = 0; st < 1024; st++) {
                u128 a, b;
                for (int l = 0; l < 4; l++) {
                    uint32_t mant = (l == 0) ? 0 : (l == 1) ? 0x7FFC00u : (rnd32() & 0x7FFC00u);
                    b._u32[l] = (s << 31) | (e << 23) | mant | st;
                    a._u32[l] = (l == 0) ? 0x7FFFFu : rnd32();
                }
                chk128("fi", a, b._u32[0], old_fi(a, b), spu_fi(a, b), &n_fi);
            }
    for (int k = 0; k < 64; k++) {              /* every a field for a few b */
        const uint32_t bb = rnd32();
        for (uint32_t av = 0; av < 0x80000u; av += 4) {
            u128 a, b = splat(bb);
            for (int l = 0; l < 4; l++) a._u32[l] = (rnd32() & 0xFFF80000u) | (av + l);
            chk128("fi", a, bb, old_fi(a, b), spu_fi(a, b), &n_fi);
        }
    }
    /* conversions: every i8, special and random inputs */
    static const uint32_t sp[] = { 0, 0x80000000u, 1, 0x7FFFFFFFu, 0xFFFFFFFFu, 0x80000001u,
        0x3F800000u, 0xBF800000u, 0x4F000000u, 0xCF000000u, 0x4F800000u, 0x7F7FFFFFu, 0x7F800000u,
        0xFF800000u, 0x7FC00000u, 0x00800000u, 0x007FFFFFu, 0x5F000000u, 0x2F800000u, 0x4EFFFFFFu };
    for (int i8 = 0; i8 < 256; i8++) {
        for (unsigned k = 0; k < 200000; k++) {
            u128 a;
            for (int l = 0; l < 4; l++)
                a._u32[l] = (k < sizeof sp / sizeof sp[0]) ? sp[(k + l) % (sizeof sp / sizeof sp[0])]
                          : (k & 1) ? rnd32()
                          : ((rnd32() & 0x80000000u) | ((uint32_t)(100 + rnd32() % 80) << 23) | (rnd32() & 0x7FFFFFu));
            chk128("cflts", a, (uint32_t)i8, old_cflts(a, i8), spu_cflts(a, i8), &n_cv);
            chk128("cfltu", a, (uint32_t)i8, old_cfltu(a, i8), spu_cfltu(a, i8), &n_cv);
            chk128("csflt", a, (uint32_t)i8, old_csflt(a, i8), spu_csflt(a, i8), &n_cv);
            chk128("cuflt", a, (uint32_t)i8, old_cuflt(a, i8), spu_cuflt(a, i8), &n_cv);
        }
    }
    printf("xf_in %llu (all 2^32), fi lanes %llu, conversion lanes %llu, mismatches %llu\n",
           n_in, n_fi, n_cv, bad);

    /* benchmark: ns per u128 op */
    enum { N = 4096 };
    static u128 A[N], B[N], R[N];
    for (int i = 0; i < N; i++) for (int l = 0; l < 4; l++) {
        A[i]._u32[l] = rnd32();
        B[i]._u32[l] = (rnd32() & 0x80000000u) | ((uint32_t)(110 + rnd32() % 30) << 23) | (rnd32() & 0x7FFFFFu);
    }
    struct { const char* n; int w; } ops[] = { {"fi", 0}, {"cfltu", 1}, {"cflts", 2} };
    for (int o = 0; o < 3; o++) {
        double t[2];
        for (int w = 0; w < 2; w++) {
            double t0 = now();
            for (int r = 0; r < 2000; r++)
                for (int i = 0; i < N; i++)
                    R[i] = ops[o].w == 0 ? (w ? spu_fi(A[i], B[i]) : old_fi(A[i], B[i]))
                         : ops[o].w == 1 ? (w ? spu_cfltu(B[i], 142) : old_cfltu(B[i], 142))
                                         : (w ? spu_cflts(B[i], 155) : old_cflts(B[i], 155));
            t[w] = (now() - t0) * 1e9 / (2000.0 * N);
        }
        printf("%-6s old %7.3f  new %7.3f ns  speedup %.1fx (%08X)\n", ops[o].n, t[0], t[1], t[0] / t[1], R[7]._u32[1]);
    }
    return bad != 0;
}
