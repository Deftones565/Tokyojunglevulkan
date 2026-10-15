/* Build + run (from the repo root; HDR = dir with the spu_helpers.h under test):
 *   HDR=third_party/ps3recomp/runtime/spu; D=tools/spu-bisect/xf-harness; clang -O2 -std=gnu17 -msse4.1 -mfma -ffp-contract=off -Ithird_party/ps3recomp/include -I$HDR -Ithird_party/ps3recomp/runtime/spu $D/xf_harness.c $D/xf_ref.c $D/xf_new.c -lm -o /tmp/xf_harness && /tmp/xf_harness [vectors_per_op=50000000]
 *
 * Bit-exactness check for patch 0057 (spu-float-helpers-simd): the u128 SPU
 * float helpers (fa fs fm fma fms fnms fceq fcgt fcmeq fcmgt) of the current
 * spu_helpers.h against a verbatim copy of the old per-lane code (old_xf.h),
 * built with the flags the lifted SPU sources use. Exit status 0 = no
 * mismatch anywhere; also prints ns per u128 op for old and new.
 *   1. Special classes: every pair (and every triple for the fma family) of
 *      ~50 special lane values, in all four lanes, and alone in one lane with
 *      random normal values in the others.
 *   2. Biased random vectors: random bits, exponents near 0/127/254/255,
 *      product overflow/underflow, add/sub near-cancellation, fma
 *      near-cancellation and exact-zero cancellation (signed zeros). Half the
 *      vectors keep every operand off exponent 255 (exercises the vector fast
 *      path), half mix lanes freely (exercises the fallback).
 */
#include <ps3emu/ps3types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void (*xf_fn)(const u128 *, const u128 *, const u128 *, u128 *, size_t);
#define XF_DECL(n) void ref_##n(const u128 *, const u128 *, const u128 *, u128 *, size_t); \
                   void new_##n(const u128 *, const u128 *, const u128 *, u128 *, size_t);
XF_DECL(fa) XF_DECL(fs) XF_DECL(fm) XF_DECL(fma) XF_DECL(fms) XF_DECL(fnms)
XF_DECL(fceq) XF_DECL(fcgt) XF_DECL(fcmeq) XF_DECL(fcmgt)

static const struct { const char *name; xf_fn ref, nw; int nargs; } ops[] = {
#define XF_ENT(n, k) { #n, ref_##n, new_##n, k },
    XF_ENT(fa, 2) XF_ENT(fs, 2) XF_ENT(fm, 2) XF_ENT(fma, 3) XF_ENT(fms, 3) XF_ENT(fnms, 3)
    XF_ENT(fceq, 2) XF_ENT(fcgt, 2) XF_ENT(fcmeq, 2) XF_ENT(fcmgt, 2)
};
#define NOPS ((int)(sizeof ops / sizeof ops[0]))

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static inline uint64_t rnd64(void) { /* splitmix64 */
    uint64_t z = (rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static inline uint32_t rnd32(void) { return (uint32_t)rnd64(); }
static inline uint32_t mk(uint32_t s, uint32_t e, uint32_t m) { return (s << 31) | ((e & 0xFF) << 23) | (m & 0x7FFFFF); }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* mantissa: random, or with few significant bits, or all ones/zero */
static uint32_t rmant(void) {
    switch (rnd32() & 7) {
    case 0: return 0;
    case 1: return 0x7FFFFF;
    case 2: return rnd32() & 0x7FF000;   /* <= 12 significant bits: exact products */
    case 3: return rnd32() & 0x7F;       /* tiny mantissa (denormal edge) */
    default: return rnd32();
    }
}
static uint32_t lane_any(int allow255) {
    for (;;) {
        uint32_t s = rnd32() & 1, e, v;
        switch (rnd32() % 8) {
        case 0: v = rnd32(); break;
        case 1: e = rnd32() % 4; v = mk(s, e, rmant()); break;              /* near 0 */
        case 2: e = 120 + rnd32() % 15; v = mk(s, e, rmant()); break;       /* near 127 */
        case 3: e = 250 + rnd32() % 6; v = mk(s, e, rmant()); break;        /* near 254/255 */
        case 4: e = 1 + rnd32() % 254; v = mk(s, e, rmant()); break;        /* any normal */
        case 5: e = 60 + rnd32() % 135; v = mk(s, e, rmant()); break;       /* products overflow/underflow */
        default: e = rnd32() % 256; v = mk(s, e, rmant()); break;
        }
        if (allow255 || ((v >> 23) & 0xFF) != 0xFF) return v;
    }
}
static inline uint32_t ulps(uint32_t v) { /* nudge by a few ulps in the bit pattern */
    int d = (int)(rnd32() % 7) - 3;
    return v + (uint32_t)d;
}
/* One lane of (a, b, c) for op k, possibly correlated for cancellation. */
static void gen_lane(int k, int allow255, uint32_t *a, uint32_t *b, uint32_t *c) {
    *a = lane_any(allow255); *b = lane_any(allow255); *c = lane_any(allow255);
    const uint32_t r = rnd32() % 8;
    if (k <= 1 && r < 3) {            /* fa/fs: b ~ +-a */
        *b = ulps(*a) ^ ((rnd32() & 1) << 31);
    } else if (k >= 3 && k <= 5 && r < 4) { /* fma family: c ~ +-a*b */
        if (r == 0) { *a = mk(rnd32() & 1, 100 + rnd32() % 55, rnd32() & 0x7FF000);
                      *b = mk(rnd32() & 1, 100 + rnd32() % 55, rnd32() & 0x7FF000); }
        float p = u2f(*a) * u2f(*b);
        uint32_t pu = f2u(p);
        if (((pu >> 23) & 0xFF) == 0xFF && !allow255) return;
        *c = (r <= 1 ? pu : ulps(pu)) ^ ((rnd32() & 1) << 31);
        if (!allow255 && ((*c >> 23) & 0xFF) == 0xFF) *c = pu;
    } else if (k >= 6 && r < 3) {     /* compares: equal / adjacent / sign-flipped */
        *b = (r == 0) ? *a : (r == 1) ? ulps(*a) : (*a ^ 0x80000000u);
    }
}

static const uint32_t specials[] = {
    0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF, 0x00400000, 0x00012345,
    0x00800000, 0x80800000, 0x00800001, 0x00FFFFFF, 0x01000000, 0x0D800000,
    0x1F800000, 0x1F800001, 0x20000000, 0x3F000000, 0x3F800000, 0xBF800000, 0x3F800001, 0x3FFFFFFF,
    0x40000000, 0xC0400000, 0x3EAAAAAB, 0x5F000000, 0x5F800000, 0x5F7FFFFF, 0x60000000, 0xDF800000,
    0x7E800000, 0x7F000000, 0x7F000001, 0x7F7FFFFF, 0xFF7FFFFF, 0x7EFFFFFF,
    0x7F800000, 0xFF800000, 0x7F800001, 0x7FC00000, 0x7FFFFFFF, 0xFFFFFFFF, 0x7FBFFFFF, 0xFFC00000,
    0x7F812345, 0x00000002, 0x2E800000, 0x51000000, 0x41200000, 0xC1200000,
};
#define NSPEC ((int)(sizeof specials / sizeof specials[0]))

#define BATCH 4096
static u128 A[BATCH], B[BATCH], C[BATCH], R0[BATCH], R1[BATCH];
static unsigned long long mism[NOPS], tested[NOPS];

static void check(int k, size_t n) {
    ops[k].ref(A, B, C, R0, n);
    ops[k].nw(A, B, C, R1, n);
    for (size_t i = 0; i < n; i++)
        for (int l = 0; l < 4; l++) {
            tested[k]++;
            if (R0[i]._u32[l] != R1[i]._u32[l]) {
                if (mism[k]++ < 8)
                    printf("MISMATCH %s lane %d: a=%08X b=%08X c=%08X old=%08X new=%08X\n", ops[k].name, l,
                           A[i]._u32[l], B[i]._u32[l], C[i]._u32[l], R0[i]._u32[l], R1[i]._u32[l]);
            }
        }
}
static size_t nb;
static void push(int k, const uint32_t a[4], const uint32_t b[4], const uint32_t c[4]) {
    memcpy(A[nb]._u32, a, 16); memcpy(B[nb]._u32, b, 16); memcpy(C[nb]._u32, c, 16);
    if (++nb == BATCH) { check(k, nb); nb = 0; }
}
static void flush(int k) { if (nb) check(k, nb); nb = 0; }
static uint32_t rnormal(void) { return mk(rnd32() & 1, 64 + rnd32() % 128, rnd32()); }

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char **argv) {
    const unsigned long long nvec = argc > 1 ? strtoull(argv[1], 0, 0) : 50000000ull;
    unsigned long long spec_tested[NOPS] = {0};
    /* 1. specials */
    for (int k = 0; k < NOPS; k++) {
        const int n3 = ops[k].nargs == 3 ? NSPEC : 1;
        for (int i = 0; i < NSPEC; i++) for (int j = 0; j < NSPEC; j++) for (int m = 0; m < n3; m++) {
            const uint32_t x = specials[i], y = specials[j], z = specials[m];
            uint32_t a[4] = {x, x, x, x}, b[4] = {y, y, y, y}, c[4] = {z, z, z, z};
            push(k, a, b, c);
            for (int l = 0; l < 4; l++) {     /* special alone in lane l */
                for (int q = 0; q < 4; q++) { a[q] = rnormal(); b[q] = rnormal(); c[q] = rnormal(); }
                a[l] = x; b[l] = y; c[l] = z;
                push(k, a, b, c);
            }
        }
        flush(k);
        spec_tested[k] = tested[k];
    }
    /* 2. biased random */
    for (int k = 0; k < NOPS; k++) {
        for (unsigned long long v = 0; v < nvec; v++) {
            uint32_t a[4], b[4], c[4];
            const int allow255 = (v & 1) ? 1 : 0;
            for (int l = 0; l < 4; l++) gen_lane(k, allow255 && (rnd32() & 3) == 0, &a[l], &b[l], &c[l]);
            push(k, a, b, c);
        }
        flush(k);
    }
    int bad = 0;
    unsigned long long total = 0;
    for (int k = 0; k < NOPS; k++) {
        printf("%-6s special-class lane tests %10llu  random lane tests %11llu  mismatches %llu\n",
               ops[k].name, spec_tested[k], tested[k] - spec_tested[k], mism[k]);
        total += tested[k]; bad |= mism[k] != 0;
    }
    printf("total lane tests %llu, %s\n", total, bad ? "MISMATCHES FOUND" : "0 mismatches");

    /* 3. benchmark: random normal operands (the common case), ns per u128 op */
    for (int i = 0; i < BATCH; i++) for (int l = 0; l < 4; l++) {
        A[i]._u32[l] = mk(rnd32() & 1, 100 + rnd32() % 50, rnd32());
        B[i]._u32[l] = mk(rnd32() & 1, 100 + rnd32() % 50, rnd32());
        C[i]._u32[l] = mk(rnd32() & 1, 100 + rnd32() % 50, rnd32());
    }
    const int reps = 4000;
    printf("benchmark (BATCH=%d x %d reps, normal operands): ns per u128 op\n", BATCH, reps);
    for (int k = 0; k < NOPS; k++) {
        double t[2];
        for (int w = 0; w < 2; w++) {
            xf_fn f = w ? ops[k].nw : ops[k].ref;
            f(A, B, C, R0, BATCH);
            double t0 = now();
            for (int r = 0; r < reps; r++) f(A, B, C, R0, BATCH);
            t[w] = (now() - t0) * 1e9 / ((double)reps * BATCH);
        }
        printf("%-6s old %7.3f  new %7.3f  speedup %5.1fx\n", ops[k].name, t[0], t[1], t[0] / t[1]);
    }
    return bad;
}
