/* Differential test: rsx_vertex_fetch_range / rsx_vertex_remap_build (patch 0100)
 * against the per-vertex rsx_vertex_fetch_one loop and the old hashed remap,
 * on 200k random plans (all types/sizes, dividers, out-of-span elements,
 * invalid ATTR0). Output bytes and return codes must match exactly.
 *
 * build (from the ps3recomp tree; old_compact.c = rsx_vertex_compact.c before 0100):
 *   D="-Drsx_vertex_decode_element=o1 -Drsx_vertex_layout_plan_init=o2 -Drsx_vertex_fetch_plan_init=o3
 *      -Drsx_vertex_inline_layout=o4 -Drsx_vertex_fetch_plan_set_inline=o5 -Drsx_vertex_fetch_plan_prepare=o6
 *      -Drsx_vertex_fetch_one=o7 -Drsx_vertex_remap_build=old_remap_build -Drsx_vertex_remap_index=o9
 *      -Drsx_vertex_remap_destroy=o10 -Drsx_vertex_topology_plan=o11"
 *   cc -O2 -c -I libs/video -I include $D -o old.o old_compact.c
 *   cc -O2 -c -I libs/video -I include -o new.o libs/video/rsx_vertex_compact.c
 *   cc -O2 -I libs/video -I include -o difftest vertex_fetch_difftest.c new.o old.o && ./difftest
 */
#include "rsx_vertex_compact.h"
#include "rsx_dispatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int old_remap_build(rsx_vertex_remap*, rsx_vertex_ref*, u32, u32*);
void rsx_dsp_get_vertex_attr(const rsx_dispatch* r, u32 i, rsx_dsp_vertex_attr* o) { (void)r; (void)i; memset(o, 0, sizeof *o); }
u32 rsx_dsp_vertex_data_base_offset(const rsx_dispatch* r) { (void)r; return 0; }
void rsx_dsp_vertex_default(const rsx_dispatch* r, u32 i, float o[4]) { (void)r; (void)i; memset(o, 0, 16); }
#define MEM (1u << 20)
static u8 mem[MEM];
static const u8* gp(void* u, u32 loc, u32 off, u32 n) { (void)u; (void)loc; return (u64)off + n <= MEM ? mem + off : NULL; }
static u32 rnd(void) { static u64 s = 88172645463325252ull; s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (u32)s; }
int main(void) {
    for (u32 i = 0; i < MEM; i++) mem[i] = (u8)rnd();
    static u8 a[4096 * 256], b[4096 * 256];
    static rsx_vertex_ref refs[4096], r1[4096], r2[4096];
    long fails = 0, zero_ret = 0, trials = 200000, remaps = 0;
    for (long t = 0; t < trials; t++) {
        rsx_vertex_fetch_plan p; memset(&p, 0, sizeof p);
        rsx_vertex_layout_plan_init(&p.layout, rnd() | 1u);
        p.base_offset = rnd() % 4096; p.divider_mask = rnd(); p.guest_ptr = gp;
        for (int k = 0; k < 16; k++) {
            rsx_vertex_fetch_attr* f = &p.attr[k];
            f->desc.type = rnd() % 9; f->desc.size = rnd() % 5;
            f->desc.stride = rnd() % 3 ? (rnd() % 64) : 0;
            f->desc.frequency = rnd() % 4 ? 0 : rnd() % 5;
            f->desc.offset = rnd() % 8 ? rnd() % (MEM / 2) : rnd();
            f->elem_size = f->desc.type == 6 ? 4 : f->desc.size * (f->desc.type == 2 ? 4 : f->desc.type == 1 || f->desc.type == 3 || f->desc.type == 5 ? 2 : 1);
            f->stride = f->desc.stride ? f->desc.stride : f->elem_size;
            for (int c = 0; c < 4; c++) f->default_value[c] = (float)(rnd() % 100);
        }
        u32 n = 1 + rnd() % (rnd() % 4 ? 64 : 4096);
        u32 base = rnd() % 3 ? 0 : rnd() % 1000, span = 1 + rnd() % (rnd() % 2 ? n : 100000);
        int mode = rnd() % 3;
        for (u32 i = 0; i < n; i++) {
            refs[i].vertex_id = mode == 0 ? 100 + i : rnd() % span;
            refs[i].base_index = rnd() % 50 ? base : base + 1;
        }
        /* remap: old vs new */
        memcpy(r1, refs, n * sizeof *refs); memcpy(r2, refs, n * sizeof *refs);
        static rsx_vertex_remap mo, mn; u32 uo = 0, un = 0;
        int ro = old_remap_build(&mo, r1, n, &uo), rn = rsx_vertex_remap_build(&mn, r2, n, &un);
        if (ro != rn || uo != un || memcmp(r1, r2, uo * sizeof *r1) ||
            (uo < n && memcmp(mo.occurrence_to_unique, mn.occurrence_to_unique, n * 4))) {
            if (fails++ < 5) printf("remap mismatch t=%ld n=%u uo=%u un=%u\n", t, n, uo, un);
        }
        remaps++;
        /* fetch: per-vertex vs range */
        rsx_vertex_fetch_plan_prepare(&p, r2, un);
        memset(a, 0xCD, (size_t)un * p.layout.stride); memset(b, 0xCD, (size_t)un * p.layout.stride);
        int ok1 = 1;
        for (u32 i = 0; i < un && ok1; i++) ok1 = rsx_vertex_fetch_one(&p, &r2[i], a + (size_t)i * p.layout.stride);
        int ok2 = rsx_vertex_fetch_range(&p, r2, un, b);
        if (!ok1) zero_ret++;
        if (ok1 != ok2 || (ok1 && memcmp(a, b, (size_t)un * p.layout.stride))) {
            if (fails++ < 5) printf("fetch mismatch t=%ld ok %d %d\n", t, ok1, ok2);
        }
    }
    printf("%ld trials, %ld fetch failures (ATTR0) agreed, %ld mismatches\n", trials, zero_ret, fails);
    return fails != 0;
}
