/* Verbatim copy of the SPU single-precision float helpers from
 * third_party/ps3recomp/runtime/spu/spu_helpers.h BEFORE patch 0057
 * (spu-float-helpers-simd): the reference for xf_harness.c. Do not edit. */
/* ---- single-precision float: SPU semantics ----
 * The SPU has no NaN or Inf. An exponent field of 255 is an ordinary number
 * up to (2 - 2^-23) * 2^128 ("extended range"), results that overflow
 * saturate to sign | 0x7FFFFFFF, denormal operands and results are zero, and
 * rounding is toward zero (CBEA SPU ISA, single-precision FP). Plain host
 * float turned exponent-255 values into Inf/NaN, after which 0 * x was NaN
 * and every compare false: Tokyo Jungle's geometry job (E7517A8D) then culled
 * every object, so the world had no depth-writing geometry and no shadows.
 *
 * Denormal operands are flushed to zero up front, which is SPU-exact, and the
 * op then runs as a host float op (round-to-nearest; at most a last-bit
 * difference from the SPU's truncation). A denormal host result is flushed
 * to zero. Only lanes with an exponent-255 operand, or a host result that
 * overflowed, are redone exactly in double: the operands widen exactly, and
 * the result narrows with truncation and saturation. That keeps the exact
 * path off the hot paths: audio DSP jobs are full of denormals, and sending
 * every one of them through double made the SGX mixer slow enough to stall
 * the movie's audio clock. */
static inline uint32_t spu__xf_ftz(uint32_t b) {
    return ((b >> 23) & 0xFFu) == 0u ? (b & 0x80000000u) : b;
}
static inline int spu__xf_ext(uint32_t b) { return ((b >> 23) & 0xFFu) == 0xFFu; }
static inline double spu__xf_in(uint32_t b) {
    const uint32_t e = (b >> 23) & 0xFFu;
    if (e == 0u) return (b & 0x80000000u) ? -0.0 : 0.0;
    const double v = ldexp(1.0 + (double)(b & 0x7FFFFFu) / 8388608.0, (int)e - 127);
    return (b & 0x80000000u) ? -v : v;
}
static inline uint32_t spu__xf_out(double v) {
    const uint32_t s = signbit(v) ? 0x80000000u : 0u;
    const double a = fabs(v);
    if (a != a || a >= 0x1p129) return s | 0x7FFFFFFFu;   /* saturate */
    if (a < 0x1p-126) return s;                            /* flush to zero */
    int e; const double m = frexp(a, &e);                  /* a = m * 2^e, m in [0.5, 1) */
    return s | ((uint32_t)(e + 126) << 23) | ((uint32_t)((m * 2.0 - 1.0) * 8388608.0) & 0x7FFFFFu);
}
enum { SPU_XF_ADD, SPU_XF_SUB, SPU_XF_MUL, SPU_XF_MA, SPU_XF_MS, SPU_XF_NMS };
static inline uint32_t spu__xf_op(int op, uint32_t ua, uint32_t ub, uint32_t uc) {
    ua = spu__xf_ftz(ua); ub = spu__xf_ftz(ub); uc = spu__xf_ftz(uc);
    if (!spu__xf_ext(ua) && !spu__xf_ext(ub) && (op < SPU_XF_MA || !spu__xf_ext(uc))) {
        float a, b, c; memcpy(&a, &ua, 4); memcpy(&b, &ub, 4); memcpy(&c, &uc, 4);
        float f;
        switch (op) {
        case SPU_XF_ADD: f = a + b; break;
        case SPU_XF_SUB: f = a - b; break;
        case SPU_XF_MUL: f = a * b; break;
        case SPU_XF_MA:  f = fmaf(a, b, c); break;
        case SPU_XF_MS:  f = fmaf(a, b, -c); break;
        default:         f = fmaf(a, -b, c); break;
        }
        uint32_t u; memcpy(&u, &f, 4);
        if (!spu__xf_ext(u)) return spu__xf_ftz(u);       /* normal, zero, or flushed */
    }
    const double x = spu__xf_in(ua), y = spu__xf_in(ub), z = spu__xf_in(uc);
    double v;
    switch (op) {
    case SPU_XF_ADD: v = x + y; break;
    case SPU_XF_SUB: v = x - y; break;
    case SPU_XF_MUL: v = x * y; break;              /* exact in double */
    case SPU_XF_MA:  v = fma(x, y, z); break;
    case SPU_XF_MS:  v = fma(x, y, -z); break;
    default:         v = fma(-x, y, z); break;
    }
    return spu__xf_out(v);
}
static inline int spu__xf_cmp(int gt, int mag, uint32_t ua, uint32_t ub) {
    ua = spu__xf_ftz(ua); ub = spu__xf_ftz(ub);
    if (!spu__xf_ext(ua) && !spu__xf_ext(ub)) {
        float a, b; memcpy(&a, &ua, 4); memcpy(&b, &ub, 4);
        if (mag) { a = fabsf(a); b = fabsf(b); }
        return gt ? a > b : a == b;
    }
    double x = spu__xf_in(ua), y = spu__xf_in(ub);
    if (mag) { x = fabs(x); y = fabs(y); }
    return gt ? x > y : x == y;
}
static inline u128 spu_fa(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=spu__xf_op(SPU_XF_ADD,a._u32[i],b._u32[i],0); return r; }
static inline u128 spu_fs(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=spu__xf_op(SPU_XF_SUB,a._u32[i],b._u32[i],0); return r; }
static inline u128 spu_fm(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=spu__xf_op(SPU_XF_MUL,a._u32[i],b._u32[i],0); return r; }
static inline u128 spu_fma(u128 a, u128 b, u128 c)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=spu__xf_op(SPU_XF_MA,a._u32[i],b._u32[i],c._u32[i]); return r; }
static inline u128 spu_fms(u128 a, u128 b, u128 c)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=spu__xf_op(SPU_XF_MS,a._u32[i],b._u32[i],c._u32[i]); return r; }
static inline u128 spu_fnms(u128 a, u128 b, u128 c) { u128 r; for(int i=0;i<4;i++) r._u32[i]=spu__xf_op(SPU_XF_NMS,a._u32[i],b._u32[i],c._u32[i]); return r; }
static inline u128 spu_fceq(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=spu__xf_cmp(0,0,a._u32[i],b._u32[i])?0xFFFFFFFFu:0; return r; }
static inline u128 spu_fcgt(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=spu__xf_cmp(1,0,a._u32[i],b._u32[i])?0xFFFFFFFFu:0; return r; }
static inline u128 spu_fcmeq(u128 a, u128 b){ u128 r; for(int i=0;i<4;i++) r._u32[i]=spu__xf_cmp(0,1,a._u32[i],b._u32[i])?0xFFFFFFFFu:0; return r; }
static inline u128 spu_fcmgt(u128 a, u128 b){ u128 r; for(int i=0;i<4;i++) r._u32[i]=spu__xf_cmp(1,1,a._u32[i],b._u32[i])?0xFFFFFFFFu:0; return r; }
