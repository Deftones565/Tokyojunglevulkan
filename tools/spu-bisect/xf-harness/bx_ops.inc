/* Batched wrappers around the integer helpers, one set per TU (PFX = ref for
 * the old scalar code in old_bx.h, new for the current spu_helpers.h).
 * Every wrapper: r[i] = op(a[i], b[i], c[i] or imm[i]) for the arguments it takes. */
#define BX_CAT2(a, b) a##_##b
#define BX_CAT(a, b) BX_CAT2(a, b)
#define BX_SIG(n) __attribute__((noinline)) void BX_CAT(PFX, n)(const u128 *a, const u128 *b, const u128 *c, const int32_t *im, u128 *r, size_t cnt)
#define BX_V1(n) BX_SIG(n) { (void)b; (void)c; (void)im; for (size_t i = 0; i < cnt; i++) r[i] = spu_##n(a[i]); }
#define BX_V2(n) BX_SIG(n) { (void)c; (void)im; for (size_t i = 0; i < cnt; i++) r[i] = spu_##n(a[i], b[i]); }
#define BX_V3(n) BX_SIG(n) { (void)im; for (size_t i = 0; i < cnt; i++) r[i] = spu_##n(a[i], b[i], c[i]); }
#define BX_VI(n) BX_SIG(n) { (void)b; (void)c; for (size_t i = 0; i < cnt; i++) r[i] = spu_##n(a[i], im[i]); }
BX_V2(ah) BX_V2(sfh) BX_VI(ahi) BX_VI(sfhi) BX_V2(ceqh) BX_V2(cgth) BX_V2(clgth)
BX_VI(ceqhi) BX_VI(cgthi) BX_VI(clgthi) BX_VI(andhi) BX_VI(orhi) BX_VI(xorhi)
BX_VI(shlhi) BX_VI(rothi) BX_VI(rotmhi) BX_VI(rothmi) BX_VI(rotmahi)
BX_V2(ceqb) BX_V2(cgtb) BX_V2(clgtb) BX_VI(ceqbi) BX_VI(cgtbi) BX_VI(clgtbi)
BX_VI(andbi) BX_VI(orbi) BX_VI(xorbi) BX_V2(absdb) BX_V2(avgb) BX_V1(cntb) BX_V1(xsbh)
BX_V2(cg) BX_V2(bg) BX_V3(addx) BX_V3(sfx)
BX_VI(shlqbyi) BX_VI(rotqbyi) BX_V2(shlqby) BX_V2(rotqby) BX_V2(shlqbybi) BX_V2(rotqbybi)
BX_V2(rotqmby) BX_V2(rotqmbybi) BX_VI(rotqmbyi) BX_V2(sumb) BX_V3(shufb)
