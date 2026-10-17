/* AOT lifted-block microbenchmark: no runtime guest decoding or execution.
 * Usage: bench_pair REF.so NEW.so FIRST_SYMBOL SECOND_SYMBOL [iterations]
 * Independent context per lane, alternating order, identical initialized input.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "spu_context.h"
int g_spu_ls_watch_n;
__thread void (*g_spu_trampoline_fn)(spu_context*);
typedef void (*fn_t)(spu_context*);
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static void init(spu_context* c) {
 memset(c,0,sizeof *c); c->ls=c->ls_store;c->image_id=9;c->host_depth=1;
 for(unsigned r=0;r<128;r++) for(unsigned w=0;w<4;w++) c->gpr[r]._u32[w]=r*16+w*4;
 for(unsigned i=0;i<SPU_LS_SIZE;i++) c->ls[i]=(uint8_t)(i*31+7);
}
int main(int argc,char** argv) {
 if(argc<5)return 2;
 unsigned n=argc>5?strtoul(argv[5],0,0):500000;
 void* h[2];fn_t f[2][2];spu_context* c[2];
 for(int i=0;i<2;i++) {
  h[i]=dlopen(argv[i+1],RTLD_NOW|RTLD_LOCAL);if(!h[i]){puts(dlerror());return 2;}
  for(int j=0;j<2;j++){f[i][j]=(fn_t)dlsym(h[i],argv[j+3]);if(!f[i][j])return 2;}
  c[i]=aligned_alloc(64,(sizeof(spu_context)+63)&~(size_t)63);if(!c[i])return 2;
 }
 for(int round=0;round<6;round++) {
  double sec[2];
  for(int pass=0;pass<2;pass++) {
   int k=(round+pass)&1;init(c[k]);c[k]->gpr[33]._u32[0]=n*4;
   double begin=now();
   for(unsigned i=0;i<n;i++){f[k][0](c[k]);f[k][1](c[k]);}
   sec[k]=now()-begin;
  }
  c[0]->ls=c[1]->ls=NULL;
  if(memcmp(c[0],c[1],sizeof(spu_context))){puts("MISMATCH");return 1;}
  printf("round %d ref %.6f new %.6f speedup %.3fx identical\n",round,sec[0],sec[1],sec[0]/sec[1]);fflush(stdout);
 }
 return 0;
}
