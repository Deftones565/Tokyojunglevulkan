/* The helpers under test: spu_helpers.h from the -I directory (the
 * submodule's runtime/spu once patch 0057 is applied, or a patched copy). */
#include <stddef.h>
#include <spu_helpers.h>
#ifndef SPU_XF_SIMD
#error "spu_helpers.h has no SPU_XF_SIMD vector path (patch 0057 not applied, or no -msse4.1)"
#endif
#define PFX new
#include "xf_ops.inc"
