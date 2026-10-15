/* Build + run (from the repo root; HDR = dir with the spu_helpers.h under test):
 *   HDR=third_party/ps3recomp/runtime/spu; D=tools/spu-bisect/xf-harness; clang -O2 -std=gnu17 -msse4.1 -mfma -ffp-contract=off -Ithird_party/ps3recomp/include -I$HDR -Ithird_party/ps3recomp/runtime/spu $D/bx_harness.c $D/bx_ref.c $D/bx_new.c -o /tmp/bx_harness && /tmp/bx_harness [op ...]
 *   (all ops in parallel: /tmp/bx_harness -l | xargs -P8 -n1 /tmp/bx_harness; "bench" runs the benchmark)
 *
 * Bit-exactness check for patch 0058 (spu-byte-helpers-simd): the integer
 * byte/halfword/word/quadword-byte helpers of the current spu_helpers.h
 * against a verbatim copy of the old scalar code (old_bx.h; shufb against
 * its byte-loop reference), built with the lifted SPU sources' flags.
 * Coverage per op kind:
 *   byte x byte (ceqb cgtb clgtb absdb avgb sumb): all 65536 byte pairs in
 *     each of the 16 lanes, then random vectors with 0/1/7F/80/FF-biased bytes.
 *   byte x imm (ceqbi cgtbi clgtbi andbi orbi xorbi): all 256 bytes in every
 *     lane x all 256 imm low bytes x 8 high-bit patterns, then random.
 *   halfword x halfword (ah sfh ceqh cgth clgth): all 2^32 pairs, then random.
 *   halfword x imm (ahi sfhi ceqhi cgthi clgthi andhi orhi xorhi): all 65536
 *     values x all 65536 imm low halves (random high halves), then random.
 *   halfword shift by imm (shlhi rothi rotmhi rothmi rotmahi), quadword byte
 *     shifts (shlqby* rotqby* rotqmby*): every shift count -4096..4095 (imm,
 *     or word 0 of b) plus random 32-bit counts, each on many random vectors.
 *   unary bytes (cntb xsbh): all 256 bytes in every lane, then random.
 *   words (cg bg addx sfx), shufb: random vectors with edge-biased lanes.
 * Exit status 0 = no mismatch. */
#include <ps3emu/ps3types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void (*bx_fn)(const u128 *, const u128 *, const u128 *, const int32_t *, u128 *, size_t);
enum { K_B2, K_BI, K_H2, K_HI, K_HS, K_U1, K_W, K_QV, K_QI, K_SHUFB };
#define BX_DECL(n, k) void ref_##n(const u128 *, const u128 *, const u128 *, const int32_t *, u128 *, size_t); \
                      void new_##n(const u128 *, const u128 *, const u128 *, const int32_t *, u128 *, size_t);
#define BX_LIST(X) \
    X(ah, K_H2) X(sfh, K_H2) X(ahi, K_HI) X(sfhi, K_HI) X(ceqh, K_H2) X(cgth, K_H2) X(clgth, K_H2) \
    X(ceqhi, K_HI) X(cgthi, K_HI) X(clgthi, K_HI) X(andhi, K_HI) X(orhi, K_HI) X(xorhi, K_HI) \
    X(shlhi, K_HS) X(rothi, K_HS) X(rotmhi, K_HS) X(rothmi, K_HS) X(rotmahi, K_HS) \
    X(ceqb, K_B2) X(cgtb, K_B2) X(clgtb, K_B2) X(ceqbi, K_BI) X(cgtbi, K_BI) X(clgtbi, K_BI) \
    X(andbi, K_BI) X(orbi, K_BI) X(xorbi, K_BI) X(absdb, K_B2) X(avgb, K_B2) X(cntb, K_U1) X(xsbh, K_U1) \
    X(cg, K_W) X(bg, K_W) X(addx, K_W) X(sfx, K_W) \
    X(shlqbyi, K_QI) X(rotqbyi, K_QI) X(shlqby, K_QV) X(rotqby, K_QV) X(shlqbybi, K_QV) X(rotqbybi, K_QV) \
    X(rotqmby, K_QV) X(rotqmbybi, K_QV) X(rotqmbyi, K_QI) X(sumb, K_B2) X(shufb, K_SHUFB)
BX_LIST(BX_DECL)
static const struct { const char *name; bx_fn ref, nw; int kind; } ops[] = {
#define BX_ENT(n, k) { #n, ref_##n, new_##n, k },
    BX_LIST(BX_ENT)
};
#define NOPS ((int)(sizeof ops / sizeof ops[0]))

static uint64_t rs = 0x2545F4914F6CDD1Dull;
static inline uint64_t rnd64(void) { /* splitmix64 */
    uint64_t z = (rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static inline uint32_t rnd32(void) { return (uint32_t)rnd64(); }
static uint32_t rbyte(void) { static const uint8_t e[] = {0, 1, 0x7F, 0x80, 0x81, 0xFE, 0xFF};
    uint32_t r = rnd32(); return (r & 3) ? (r >> 8) & 0xFF : e[(r >> 8) % 7]; }
static uint32_t rhalf(void) { static const uint16_t e[] = {0, 1, 2, 0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF, 0x00FF, 0xFF00};
    uint32_t r = rnd32(); return (r & 3) ? r >> 16 : e[(r >> 8) % 10]; }
static uint32_t rword(void) {
    static const uint32_t e[] = {0, 1, 2, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF, 0xFFFF, 0x10000};
    switch (rnd32() % 6) {
    case 0: return e[rnd32() % 10];
    case 1: return rbyte() | rbyte() << 8 | rbyte() << 16 | rbyte() << 24;
    case 2: return rhalf() | rhalf() << 16;
    default: return rnd32();
    }
}

#define BATCH 4096
static u128 A[BATCH], B[BATCH], C[BATCH], R0[BATCH], R1[BATCH];
static int32_t IM[BATCH];
static size_t nb;
static unsigned long long tested, mism;
static int cur;

static void check(void) {
    ops[cur].ref(A, B, C, IM, R0, nb);
    ops[cur].nw(A, B, C, IM, R1, nb);
    for (size_t i = 0; i < nb; i++) {
        tested++;
        if (memcmp(&R0[i], &R1[i], 16)) {
            if (mism++ < 8)
                printf("MISMATCH %s: a=%08X%08X%08X%08X b=%08X%08X%08X%08X c=%08X%08X%08X%08X imm=%d old=%08X%08X%08X%08X new=%08X%08X%08X%08X\n",
                       ops[cur].name, A[i]._u32[0], A[i]._u32[1], A[i]._u32[2], A[i]._u32[3],
                       B[i]._u32[0], B[i]._u32[1], B[i]._u32[2], B[i]._u32[3],
                       C[i]._u32[0], C[i]._u32[1], C[i]._u32[2], C[i]._u32[3], IM[i],
                       R0[i]._u32[0], R0[i]._u32[1], R0[i]._u32[2], R0[i]._u32[3],
                       R1[i]._u32[0], R1[i]._u32[1], R1[i]._u32[2], R1[i]._u32[3]);
        }
    }
    nb = 0;
}
/* slot for the next test vector; the caller fills A/B/C/IM[nb] then calls next() */
static inline void next(void) { if (++nb == BATCH) check(); }
static void rvec(u128 *v, int kind) {
    for (int l = 0; l < 4; l++)
        v->_u32[l] = kind == K_B2 || kind == K_BI || kind == K_U1
            ? rbyte() | rbyte() << 8 | rbyte() << 16 | rbyte() << 24
            : kind == K_H2 || kind == K_HI ? rhalf() | rhalf() << 16 : rword();
}
static void fill_random(int kind) {
    rvec(&A[nb], kind); rvec(&B[nb], kind); rvec(&C[nb], kind);
    IM[nb] = (int32_t)rword();
    if (kind == K_SHUFB) /* control bytes: random, or biased to the special 0x80.. selectors */
        for (int j = 0; j < 16; j++) if (rnd32() & 1) C[nb]._u8[j] = (uint8_t)(0x80 | rnd32());
}

static void run(int k, unsigned long long nrand) {
    cur = k; tested = mism = 0;
    const int kind = ops[k].kind;
    switch (kind) {
    case K_B2: /* every byte pair in every lane */
        for (uint32_t v = 0; v < 65536; v++) {
            rvec(&C[nb], kind); IM[nb] = 0;
            for (int j = 0; j < 16; j++) { uint32_t p = (v + (uint32_t)j * 4099u) & 0xFFFF;
                A[nb]._u8[j] = (uint8_t)(p >> 8); B[nb]._u8[j] = (uint8_t)p; }
            next();
        }
        break;
    case K_BI: case K_U1:
        for (uint32_t hi = 0; hi < (kind == K_BI ? 8u : 1u); hi++)
            for (uint32_t imm = 0; imm < (kind == K_BI ? 256u : 1u); imm++)
                for (uint32_t v = 0; v < 256; v++) {
                    for (int j = 0; j < 16; j++) A[nb]._u8[j] = (uint8_t)(v + (uint32_t)j * 17u);
                    rvec(&B[nb], kind); rvec(&C[nb], kind);
                    IM[nb] = (int32_t)((hi == 0 ? 0u : hi == 1 ? 0xFFFFFF00u : rnd32() & ~0xFFu) | imm);
                    next();
                }
        break;
    case K_H2: /* all 2^32 halfword pairs: lane j covers pairs [j*2^29, (j+1)*2^29) */
        for (uint32_t v = 0; v < (1u << 29); v++) {
            for (int j = 0; j < 8; j++) { uint32_t p = v + ((uint32_t)j << 29);
                A[nb]._u16[j] = (uint16_t)(p >> 16); B[nb]._u16[j] = (uint16_t)p; }
            next();
        }
        break;
    case K_HI: /* all 65536 values x all 65536 imm low halves */
        for (uint32_t imm = 0; imm < 65536; imm++)
            for (uint32_t v = 0; v < 8192; v++) {
                for (int j = 0; j < 8; j++) A[nb]._u16[j] = (uint16_t)(v * 8 + (uint32_t)j);
                IM[nb] = (int32_t)((rnd32() & 0xFFFF0000u) | imm);
                next();
            }
        break;
    case K_HS: case K_QI: case K_QV:
        for (int pass = 0; pass < 2; pass++)
            for (int64_t s = -4096; s < 4096; s++)
                for (int t = 0; t < 64; t++) {
                    rvec(&A[nb], K_W); rvec(&B[nb], K_W); rvec(&C[nb], K_W);
                    const uint32_t cnt = pass ? rnd32() : (uint32_t)(int32_t)s;
                    IM[nb] = (int32_t)cnt; B[nb]._u32[0] = cnt;
                    next();
                }
        break;
    default: break;
    }
    for (unsigned long long i = 0; i < nrand; i++) { fill_random(kind); next(); }
    if (nb) check();
    printf("%-9s vector tests %12llu  mismatches %llu\n", ops[k].name, tested, mism);
    fflush(stdout);
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void bench(void) {
    for (int i = 0; i < BATCH; i++) { rvec(&A[i], K_W); rvec(&B[i], K_W); rvec(&C[i], K_W);
        IM[i] = (int32_t)(rnd32() % 64) - 32; B[i]._u32[0] = rnd32() % 64; }
    const int reps = 2000;
    printf("benchmark (BATCH=%d x %d reps): ns per u128 op\n", BATCH, reps);
    for (int k = 0; k < NOPS; k++) {
        double t[2];
        for (int w = 0; w < 2; w++) {
            bx_fn f = w ? ops[k].nw : ops[k].ref;
            f(A, B, C, IM, R0, BATCH);
            double t0 = now();
            for (int r = 0; r < reps; r++) f(A, B, C, IM, R0, BATCH);
            t[w] = (now() - t0) * 1e9 / ((double)reps * BATCH);
        }
        printf("%-9s old %7.3f  new %7.3f  speedup %5.1fx\n", ops[k].name, t[0], t[1], t[0] / t[1]);
    }
}

int main(int argc, char **argv) {
    const unsigned long long nrand = getenv("BX_RANDOM") ? strtoull(getenv("BX_RANDOM"), 0, 0) : 20000000ull;
    if (argc > 1 && !strcmp(argv[1], "-l")) { for (int k = 0; k < NOPS; k++) puts(ops[k].name); puts("bench"); return 0; }
    int bad = 0;
    for (int k = 0; k < NOPS; k++) {
        int want = argc == 1;
        for (int i = 1; i < argc; i++) want |= !strcmp(argv[i], ops[k].name);
        if (!want) continue;
        rs = 0x2545F4914F6CDD1Dull + (uint64_t)k * 0x1000193ull;
        run(k, nrand);
        bad |= mism != 0;
    }
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "bench")) bench();
    if (argc == 1) bench();
    return bad;
}
