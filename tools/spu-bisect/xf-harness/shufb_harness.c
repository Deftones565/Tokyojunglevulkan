/* Build + run (from the repo root; HDR = dir with the spu_helpers.h under test):
 *   HDR=third_party/ps3recomp/runtime/spu; gcc -O2 -std=gnu17 -msse4.1 -mfma -ffp-contract=off \
 *     -Ithird_party/ps3recomp/include -I$HDR -Ithird_party/ps3recomp/runtime/spu \
 *     tools/spu-bisect/xf-harness/shufb_harness.c -lm -o /tmp/shufb_harness && /tmp/shufb_harness
 *
 * Bit-exactness check for patch 0061 (spu-shufb-host-order): spu_shufb (the
 * SSE path) against spu_shufb_ref (the byte loop, the reference) from the same
 * header. shufb is byte-wise independent, so it is checked
 *   1. exhaustively per byte: every control value 0..255 in every one of the 16
 *      result positions, against several random (a, b) pairs, and
 *   2. on random vectors whose control bytes mix every selector class,
 * then benchmarked. Exit status 0 = no mismatch. */
#include <ps3emu/ps3types.h>
#include <spu_helpers.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef SPU_SIMD_X86
#error "needs the SSE4.1 spu_shufb (build with -msse4.1)"
#endif

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint64_t rnd64(void) {
    uint64_t z = (rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static u128 rvec(void) { u128 v; v._u64[0] = rnd64(); v._u64[1] = rnd64(); return v; }
static unsigned long long tested, bad;
static void check(u128 a, u128 b, u128 c) {
    u128 r0 = spu_shufb_ref(a, b, c), r1 = spu_shufb(a, b, c);
    tested++;
    if (memcmp(&r0, &r1, 16)) {
        if (bad++ < 8) {
            printf("MISMATCH c=");
            for (int i = 0; i < 16; i++) printf("%02X", c._u8[i]);
            printf("\n");
        }
    }
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(void) {
    /* 1. every control byte value in every position */
    for (int rep = 0; rep < 64; rep++) {
        const u128 a = rvec(), b = rvec();
        for (int pos = 0; pos < 16; pos++)
            for (int v = 0; v < 256; v++) {
                u128 c = rvec();
                c._u8[pos] = (uint8_t)v;
                check(a, b, c);
            }
    }
    const unsigned long long t1 = tested;
    /* 2. random vectors, control bytes from every class */
    for (long i = 0; i < 20000000; i++) {
        u128 a = rvec(), b = rvec(), c = rvec();
        for (int k = 0; k < 16; k++)
            switch (rnd64() & 3) {
            case 0: c._u8[k] &= 0x1F; break;              /* plain select */
            case 1: c._u8[k] = 0x80 | (c._u8[k] & 0x3F); break;  /* 10x -> 0x00 */
            case 2: c._u8[k] = 0xC0 | (c._u8[k] & 0x3F); break;  /* 110 / 111 */
            default: break;                               /* any bits */
            }
        check(a, b, c);
    }
    printf("per-byte exhaustive %llu, random %llu, mismatches %llu\n", t1, tested - t1, bad);

    /* 3. benchmark: a dependent chain, as in lifted code */
    enum { N = 4096 };
    static u128 A[N], B[N], C[N];
    for (int i = 0; i < N; i++) { A[i] = rvec(); B[i] = rvec(); C[i] = rvec(); }
    for (int w = 0; w < 2; w++) {
        u128 acc = A[0];
        double t0 = now();
        for (int r = 0; r < 4000; r++)
            for (int i = 0; i < N; i++) acc = w ? spu_shufb(acc, B[i], C[i]) : spu_shufb_ref(acc, B[i], C[i]);
        double dt = (now() - t0) * 1e9 / (4000.0 * N);
        printf("%s %.3f ns per shufb (chk %08X)\n", w ? "sse" : "ref", dt, acc._u32[0]);
    }
    return bad != 0;
}
