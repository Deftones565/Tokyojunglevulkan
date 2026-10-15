/* Verbatim copy of the scalar SPU integer helpers from
 * third_party/ps3recomp/runtime/spu/spu_helpers.h BEFORE patch 0058
 * (spu-byte-helpers-simd): the reference for bx_harness.c. Do not edit. */
static inline u128 spu_ah(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]+b._u16[i]; return r; }
static inline u128 spu_sfh(u128 a, u128 b){ u128 r; for(int i=0;i<8;i++) r._u16[i]=b._u16[i]-a._u16[i]; return r; }
static inline u128 spu_ahi(u128 a, int32_t imm){ u128 r; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]+(uint16_t)imm; return r; }
static inline u128 spu_sfhi(u128 a, int32_t imm){ u128 r; for(int i=0;i<8;i++) r._u16[i]=(uint16_t)imm-a._u16[i]; return r; }
static inline u128 spu_ceqh(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]==b._u16[i])?0xFFFFu:0; return r; }
static inline u128 spu_cgth(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]>b._s16[i])?0xFFFFu:0; return r; }
static inline u128 spu_clgth(u128 a, u128 b){ u128 r; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]>b._u16[i])?0xFFFFu:0; return r; }
static inline u128 spu_ceqhi(u128 a, int32_t imm)  { u128 r; int16_t v=(int16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]==v)?0xFFFFu:0; return r; }
static inline u128 spu_cgthi(u128 a, int32_t imm)  { u128 r; int16_t v=(int16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]>v)?0xFFFFu:0; return r; }
static inline u128 spu_clgthi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]>v)?0xFFFFu:0; return r; }
static inline u128 spu_andhi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]&v; return r; }
static inline u128 spu_orhi(u128 a, int32_t imm)  { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]|v; return r; }
static inline u128 spu_xorhi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]^v; return r; }
static inline u128 spu_shlhi(u128 a, int sh) { u128 r; sh&=0x1F; for(int i=0;i<8;i++) r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]<<sh); return r; }
static inline u128 spu_rothi(u128 a, int sh) { u128 r; sh&=15; for(int i=0;i<8;i++) r._u16[i]=(uint16_t)((a._u16[i]<<sh)|(a._u16[i]>>(16-sh))); return r; }
static inline u128 spu_rotmhi(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]>>sh); return r; }
static inline u128 spu_rothmi(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]>>sh); return r; }
static inline u128 spu_rotmahi(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._s16[i]=(sh>15)?(a._s16[i]>>15):(a._s16[i]>>sh); return r; }
static inline u128 spu_ceqb(u128 a, u128 b) { u128 r; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]==b._u8[i])?0xFFu:0; return r; }
static inline u128 spu_cgtb(u128 a, u128 b) { u128 r; for(int i=0;i<16;i++) r._u8[i]=(a._s8[i]>b._s8[i])?0xFFu:0; return r; }
static inline u128 spu_clgtb(u128 a, u128 b){ u128 r; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]>b._u8[i])?0xFFu:0; return r; }
static inline u128 spu_ceqbi(u128 a, int32_t imm)  { u128 r; uint8_t v=(uint8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]==v)?0xFFu:0; return r; }
static inline u128 spu_cgtbi(u128 a, int32_t imm)  { u128 r; int8_t v=(int8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._s8[i]>v)?0xFFu:0; return r; }
static inline u128 spu_clgtbi(u128 a, int32_t imm) { u128 r; uint8_t v=(uint8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]>v)?0xFFu:0; return r; }
static inline u128 spu_andbi(u128 a, int32_t imm) { u128 r; uint8_t  v=(uint8_t)imm;  for(int i=0;i<16;i++) r._u8[i]=a._u8[i]&v;   return r; }
static inline u128 spu_orbi(u128 a, int32_t imm)  { u128 r; uint8_t  v=(uint8_t)imm;  for(int i=0;i<16;i++) r._u8[i]=a._u8[i]|v;   return r; }
static inline u128 spu_xorbi(u128 a, int32_t imm){ u128 r; for(int i=0;i<16;i++) r._u8[i]=(uint8_t)(a._u8[i]^(uint8_t)imm); return r; }
static inline u128 spu_absdb(u128 a, u128 b)  { u128 r; for(int i=0;i<16;i++){ uint8_t x=a._u8[i],y=b._u8[i]; r._u8[i]=(uint8_t)(x>y?x-y:y-x); } return r; }
static inline u128 spu_avgb(u128 a, u128 b)   { u128 r; for(int i=0;i<16;i++) r._u8[i]=(uint8_t)(((uint32_t)a._u8[i]+(uint32_t)b._u8[i]+1u)>>1); return r; }
static inline u128 spu_cntb(u128 a) { u128 r; for(int i=0;i<16;i++){ uint8_t v=a._u8[i],c=0; while(v){c+=v&1;v>>=1;} r._u8[i]=c; } return r; }
static inline u128 spu_xsbh(u128 a) { u128 r; for(int i=0;i<8;i++) r._s16[i] = (int8_t)a._u8[2*i]; return r; }
static inline u128 spu_cg(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)(((uint64_t)a._u32[i]+(uint64_t)b._u32[i])>>32); return r; }
static inline u128 spu_bg(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)(((uint64_t)b._u32[i]+(uint64_t)(~a._u32[i])+1u)>>32); return r; }
static inline u128 spu_addx(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]+b._u32[i]+(t._u32[i]&1); return r; }
static inline u128 spu_sfx(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=b._u32[i]+~a._u32[i]+(t._u32[i]&1u); return r; }
static inline u128 spu_shlqbyi(u128 a, int sh) { u128 r=spu_zero(); sh&=0x1F; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
static inline u128 spu_rotqbyi(u128 a, int sh) { u128 r; sh&=0x0F; for(int i=0;i<16;i++) r._u8[SPU_W(i)]=a._u8[SPU_W((i+sh)&0x0F)]; return r; }
static inline u128 spu_shlqby(u128 a, u128 b){ int sh=b._u32[0]&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
static inline u128 spu_rotqby(u128 a, u128 b){ int sh=b._u32[0]&0x0F; u128 r; for(int i=0;i<16;i++) r._u8[SPU_W(i)]=a._u8[SPU_W((i+sh)&0x0F)]; return r; }
static inline u128 spu_shlqbybi(u128 a, u128 b){ int sh=(b._u32[0]>>3)&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
static inline u128 spu_rotqbybi(u128 a, u128 b){ int sh=(b._u32[0]>>3)&0x0F; u128 r; for(int i=0;i<16;i++) r._u8[SPU_W(i)]=a._u8[SPU_W((i+sh)&0x0F)]; return r; }
static inline u128 spu_rotqmby(u128 a, u128 b)   { int sh=(0-(int)b._u32[0])&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i-sh; if(s>=0) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
static inline u128 spu_rotqmbybi(u128 a, u128 b) { int sh=(0-((int)b._u32[0]>>3))&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i-sh; if(s>=0) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
static inline u128 spu_rotqmbyi(u128 a, int i7)  { int sh=(0-i7)&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i-sh; if(s>=0) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
static inline u128 spu_sumb(u128 a, u128 b) {
    u128 r;
    for(int i=0;i<4;i++) {
        uint32_t wa=a._u32[i], wb=b._u32[i];
        uint32_t sa=((wa>>24)&0xFF)+((wa>>16)&0xFF)+((wa>>8)&0xFF)+(wa&0xFF);
        uint32_t sb=((wb>>24)&0xFF)+((wb>>16)&0xFF)+((wb>>8)&0xFF)+(wb&0xFF);
        r._u32[i]=(sb<<16)|sa;
    }
    return r;
}
static inline u128 spu_shufb_ref(u128 a, u128 b, u128 c) {
    /* shufb is defined on SPU byte positions. Our u128 is host-native LE, so SPU
     * byte P lives at _u8[SPU_W(P)]; map every access (the concat source, the
     * control, and the result) through SPU_W so a control supplied as an immediate
     * (ila/il) or an LS-loaded constant -- already in true SPU byte order -- is
     * interpreted correctly. The cbd/chd/cwd/cdd generators below produce true
     * SPU-byte-order selectors to match. */
    uint8_t cat[32];
    for (int j=0;j<16;j++) cat[j]    = a._u8[SPU_W(j)];   /* concat SPU byte j  = a SPU byte j */
    for (int j=0;j<16;j++) cat[16+j] = b._u8[SPU_W(j)];   /* concat SPU byte 16+j = b SPU byte j */
    u128 r;
    for (int t=0;t<16;t++) {                              /* result SPU byte t */
        uint8_t s = c._u8[SPU_W(t)];                      /* control SPU byte t */
        uint8_t v;
        if      ((s & 0xE0)==0xE0) v=0x80;
        else if ((s & 0xC0)==0xC0) v=0xFF;
        else if ((s & 0xC0)==0x80) v=0x00;
        else                       v=cat[s & 0x1F];       /* concat SPU byte (s & 0x1F) */
        r._u8[SPU_W(t)] = v;
    }
    return r;
}
