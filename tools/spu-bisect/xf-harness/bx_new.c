/* The helpers under test: spu_helpers.h from the -I directory (the
 * submodule's runtime/spu once patch 0058 is applied, or a patched copy). */
#include <stddef.h>
#include <spu_helpers.h>
#ifndef SPU_SIMD_X86
#error "spu_helpers.h has no SPU_SIMD_X86 integer vector path (patch 0058 not applied, or no -msse4.1)"
#endif
#define PFX new
#include "bx_ops.inc"
