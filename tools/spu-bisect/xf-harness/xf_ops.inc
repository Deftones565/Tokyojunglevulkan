/* Batched wrappers around the u128 float helpers, one set per TU (PFX = ref
 * for the old code in old_xf.h, new for the current spu_helpers.h). */
#define XF_CAT2(a, b) a##_##b
#define XF_CAT(a, b) XF_CAT2(a, b)
#define XF_OP2(n) __attribute__((noinline)) void XF_CAT(PFX, n)(const u128 *a, const u128 *b, const u128 *c, u128 *r, size_t cnt) \
    { (void)c; for (size_t i = 0; i < cnt; i++) r[i] = spu_##n(a[i], b[i]); }
#define XF_OP3(n) __attribute__((noinline)) void XF_CAT(PFX, n)(const u128 *a, const u128 *b, const u128 *c, u128 *r, size_t cnt) \
    { for (size_t i = 0; i < cnt; i++) r[i] = spu_##n(a[i], b[i], c[i]); }
XF_OP2(fa) XF_OP2(fs) XF_OP2(fm) XF_OP3(fma) XF_OP3(fms) XF_OP3(fnms)
XF_OP2(fceq) XF_OP2(fcgt) XF_OP2(fcmeq) XF_OP2(fcmgt)
