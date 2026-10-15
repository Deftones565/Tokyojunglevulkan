#include <ps3emu/ps3types.h>
#include <stddef.h>
#include <string.h>
#define SPU_W(P) (((P) & ~3) | (3 - ((P) & 3)))
static inline u128 spu_zero(void) { u128 r; memset(&r,0,sizeof r); return r; }
#include "old_bx.h"
#define spu_shufb spu_shufb_ref   /* the byte loop is the reference for shufb */
#define PFX ref
#include "bx_ops.inc"
