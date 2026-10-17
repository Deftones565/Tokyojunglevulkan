/* Whole AOT region vs original trampoline loop. No guest decoding. */
#include <dlfcn.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spu_context.h"
int g_spu_ls_watch_n;
__thread void (*g_spu_trampoline_fn)(spu_context*);
typedef void (*fn_t)(spu_context*);
static unsigned ticks, hooks, abort_at;
static uint64_t trace;
static fn_t entries[2];
static jmp_buf unwind;
static uint64_t seed=1234567;
static uint32_t rnd(void){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}
void yz_lockstep_tick(spu_context* c){ ++ticks; trace=trace*131+c->pc; }
void spu_task_launch_check(spu_context* c, void* fn){
 if(g_spu_trampoline_fn || (fn!=(void*)entries[0] && fn!=(void*)entries[1])) abort();
 ++hooks; ++c->steps;
 trace=trace*131+c->pc;
 for(unsigned r=0;r<128;r++)for(unsigned j=0;j<4;j++)trace=trace*131+c->gpr[r]._u32[j];
 if(abort_at && hooks==abort_at)longjmp(unwind,1);
}
int main(int argc,char** argv){
 if(argc!=3)return 2;
 void *h[2]={dlopen(argv[1],RTLD_NOW|RTLD_LOCAL),dlopen(argv[2],RTLD_NOW|RTLD_LOCAL)};
 if(!h[0]||!h[1]){puts(dlerror());return 2;}
 fn_t f[2][2];
 const char* names[]={"j8D485835BE6E8738_spu_func_00007F48","j8D485835BE6E8738_spu_func_00008000"};
 for(int k=0;k<2;k++)for(int j=0;j<2;j++)f[k][j]=(fn_t)dlsym(h[k],names[j]);
 int (*region)(spu_context*,fn_t)=dlsym(h[1],"spu_try_region");
 if(!region||!f[0][0]||!f[1][1])return 2;
 spu_context* c[2];for(int k=0;k<2;k++)c[k]=aligned_alloc(64,(sizeof(spu_context)+63)&~(size_t)63);
 memset(c[0],0,sizeof *c[0]);
 if(getenv("SPU_REGIONS") && getenv("SPU_REGIONS")[0]=='0'){
  if(region(c[0],f[1][0]))abort();puts("PASS: disabled region rejects entry");return 0;
 }
 if(region(c[0],spu_indirect_branch))abort();
 for(unsigned it=0;it<2000;it++){
  memset(c[0],0,sizeof *c[0]);
  for(unsigned r=0;r<128;r++)for(unsigned j=0;j<4;j++)c[0]->gpr[r]._u32[j]=rnd();
  for(unsigned i=0;i<SPU_LS_SIZE/4;i++)((uint32_t*)c[0]->ls_store)[i]=rnd();
  c[0]->image_id=9;c[0]->host_depth=it%2;c[0]->steps=it%3?17:UINT32_MAX;
  c[0]->gpr[33]._u32[0]=4*(1+rnd()%16);
  c[0]->drain_ret_pc=it%2?0x7f48:0x8000;
  c[0]->pc=it%2?0x7f48:0x8000;
  memcpy(c[1],c[0],sizeof *c[0]);
  unsigned counts[2],ends[2];uint64_t traces[2];
  for(int k=0;k<2;k++){
   c[k]->ls=c[k]->ls_store;entries[0]=f[k][0];entries[1]=f[k][1];
   hooks=ticks=0;trace=0;abort_at=it%7==0?1+it%12:0;g_spu_trampoline_fn=NULL;
   if(!setjmp(unwind)){
    if(k){if(!region(c[k],f[k][it%2?0:1]))abort();}
    else{
     fn_t next=f[k][it%2?0:1];unsigned count=0;
     while(next && next!=spu_indirect_branch){
      if(++count>40)abort();next(c[k]);next=g_spu_trampoline_fn;g_spu_trampoline_fn=NULL;
      if(next && next!=spu_indirect_branch){yz_lockstep_tick(c[k]);spu_task_launch_check(c[k],(void*)next);}
     }
     g_spu_trampoline_fn=next;
    }
   }
   counts[k]=hooks+ticks*100;traces[k]=trace;ends[k]=g_spu_trampoline_fn==spu_indirect_branch;
   c[k]->ls=NULL;
  }
  if(memcmp(c[0],c[1],sizeof *c[0])||counts[0]!=counts[1]||traces[0]!=traces[1]||ends[0]!=ends[1]){
   printf("MISMATCH case %u hooks %u/%u trace %llu/%llu\n",it,counts[0],counts[1],(unsigned long long)traces[0],(unsigned long long)traces[1]);return 1;
  }
 }
 memset(c[0],0,sizeof *c[0]);
 for(unsigned i=0;i<4;i++){
  c[0]->int_enable=i==0;c[0]->policy_mode=i==1;c[0]->irq_saved=i==2;c[0]->irq_frame=i==3?(void*)1:NULL;
  memcpy(c[1],c[0],sizeof *c[0]);
  if(region(c[0],f[1][0])||memcmp(c[0],c[1],sizeof *c[0]))abort();
 }
 puts("PASS: 2000 whole-region comparisons, hooks/unwinds/return-PC collisions and 4 rejected contexts");
 return 0;
}
