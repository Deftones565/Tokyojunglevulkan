/* Differential tests against scalar guest-word arithmetic, including all
 * count encodings, signed halfword edges, carries across every word boundary. */
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
static uint32_t seed=0x12345678;
static uint32_t rng(void) { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
static void check(const char *op,u128 got,u128 want,unsigned n) {
    if(memcmp(&got,&want,16)) { fprintf(stderr,"FAIL %s case %u\n",op,n);exit(1); }
}
static u128 left(u128 a,unsigned s,int rotate) {
    u128 r=a; if(!s)return r;
    for(int i=0;i<4;i++)r._u32[i]=(a._u32[i]<<s)|((i<3?a._u32[i+1]:(rotate?a._u32[0]:0))>>(32-s));
    return r;
}
static u128 right(u128 a,unsigned s) {
    u128 r=a;if(!s)return r;
    for(int i=0;i<4;i++)r._u32[i]=(a._u32[i]>>s)|((i?a._u32[i-1]:0)<<(32-s));
    return r;
}
int main(void) {
    const uint32_t edges[]={0,1,0xffff,0x7fff,0x8000,0xffff0000,0x80000000,0x7fffffff,0xffffffff,0x80008000,0x7fff7fff,0x00010001};
    for(unsigned n=0;n<2000000;n++) {
        u128 a,b,r;
        for(int i=0;i<4;i++) {
            a._u32[i]=n<144?edges[n/12]:rng();b._u32[i]=n<144?edges[n%12]:rng();
        }
        for(int i=0;i<4;i++)r._u32[i]=(uint32_t)((int32_t)a._s16[2*i+1]*(int32_t)b._s16[2*i])<<16;
        check("mpyh",spu_mpyh(a,b),r,n);
        for(int i=0;i<4;i++)r._s32[i]=(int32_t)a._s16[2*i+1]*(int32_t)b._s16[2*i+1];
        check("mpyhh",spu_mpyhh(a,b),r,n);
        for(int i=0;i<4;i++)r._u32[i]=(uint32_t)a._u16[2*i+1]*b._u16[2*i+1];
        check("mpyhhu",spu_mpyhhu(a,b),r,n);
        int imm=(int)(n&255)-128;
        check("shlqbii",spu_shlqbii(a,imm),left(a,(unsigned)imm&7,0),n);
        check("rotqbii",spu_rotqbii(a,imm),left(a,(unsigned)imm&7,1),n);
        check("rotqmbii",spu_rotqmbii(a,imm),right(a,(0u-(unsigned)imm)&7),n);
        check("shlqbi",spu_shlqbi(a,b),left(a,b._u32[0]&7,0),n);
        check("rotqbi",spu_rotqbi(a,b),left(a,b._u32[0]&7,1),n);
        check("rotqmbi",spu_rotqmbi(a,b),right(a,(0u-b._u32[0])&7),n);
    }
    puts("PASS: 2,000,000 vectors per helper, 9 helpers; edges and all immediate counts");
}
