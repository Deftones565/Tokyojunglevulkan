/* Run one lifted SPU function from two builds of the same image (pshufb vs
 * byte-loop shufb) on identical random state; report any difference. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "spu_context.h"
int g_spu_ls_watch_n = 0;
__thread void (*g_spu_trampoline_fn)(spu_context*);
static uint64_t s = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void){ s^=s<<13; s^=s>>7; s^=s<<17; return (uint32_t)s; }
typedef void (*fn_t)(spu_context*);
int main(int argc, char** argv) {
  const char* fname = argv[1]; int iters = argc > 2 ? atoi(argv[2]) : 2000;
  int cnt_reg = argc > 3 ? atoi(argv[3]) : -1;   /* loop counter gpr, set to 4*k */
  void* hn = dlopen(argv[4] ? argv[4] : "./new.so", RTLD_NOW|RTLD_LOCAL);
  void* hr = dlopen(argv[5] ? argv[5] : "./ref.so", RTLD_NOW|RTLD_LOCAL);
  if (!hn || !hr) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
  fn_t fn = (fn_t)dlsym(hn, fname), fr = (fn_t)dlsym(hr, fname);
  if (!fn || !fr) { fprintf(stderr, "dlsym %s failed\n", fname); return 2; }
  spu_context *a = aligned_alloc(64, (sizeof *a + 63) & ~63ul), *b = aligned_alloc(64, (sizeof *b + 63) & ~63ul);
  int bad = 0;
  for (int it = 0; it < iters && bad < 5; it++) {
    memset(a, 0, sizeof *a);
    for (int r = 0; r < 128; r++) for (int w = 0; w < 4; w++) a->gpr[r]._u32[w] = rnd();
    /* keep pointer-ish registers in LS range so loads/stores hit varied LS */
    for (unsigned i = 0; i < SPU_LS_SIZE/4; i++) ((uint32_t*)a->ls_store)[i] = rnd();
    if (cnt_reg >= 0) a->gpr[cnt_reg]._u32[0] = 4u * (1 + rnd() % 8);
    a->ls = a->ls_store; a->image_id = 9; a->host_depth = 1;
    memcpy(b, a, sizeof *a); b->ls = b->ls_store;
    fn(a); fr(b);
    a->ls = b->ls = NULL;
    for (int r = 0; r < 128; r++) if (memcmp(&a->gpr[r], &b->gpr[r], 16)) {
      printf("iter %d: gpr[%d] differs new %08x %08x %08x %08x ref %08x %08x %08x %08x\n", it, r,
        a->gpr[r]._u32[0],a->gpr[r]._u32[1],a->gpr[r]._u32[2],a->gpr[r]._u32[3],
        b->gpr[r]._u32[0],b->gpr[r]._u32[1],b->gpr[r]._u32[2],b->gpr[r]._u32[3]); bad++; }
    if (memcmp(a->ls_store, b->ls_store, SPU_LS_SIZE)) {
      for (unsigned i = 0; i < SPU_LS_SIZE; i += 16) if (memcmp(a->ls_store+i, b->ls_store+i, 16)) { printf("iter %d: LS 0x%05x differs\n", it, i); bad++; break; } }
    if (memcmp(a, b, sizeof *a)) { /* any other field */
      const uint8_t *pa=(const uint8_t*)a, *pb=(const uint8_t*)b; size_t i=0; while (pa[i]==pb[i]) i++;
      if (i < offsetof(spu_context, gpr) || i >= offsetof(spu_context, gpr) + sizeof a->gpr) if (!(i >= offsetof(spu_context, ls_store) && i < offsetof(spu_context, ls_store) + SPU_LS_SIZE)) { printf("iter %d: ctx byte %zu differs\n", it, i); bad++; } }
  }
  printf("%s: %d iterations, %s\n", fname, iters, bad ? "MISMATCH" : "identical");
  return bad != 0;
}
