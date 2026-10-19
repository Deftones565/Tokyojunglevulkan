/* Inline guest memory access for the lifted PPU code (force-included into the
 * generated ppu_recomp_*.cpp only; CMake option TJ_VM_FAST, default ON).
 *
 * The lifted code calls vm_read32/vm_write32 and friends for every guest load
 * and store. The runtime's versions (ppu_loader.cpp) are out-of-line functions
 * that also run a dozen env-gated debug hooks (read/write watches, value
 * watches, a per-thread spin detector, forced reads) on every access: ~6% of
 * all CPU in Tokyo Jungle gameplay. The LasVola/tokyojungle port inlines the
 * same accessors for its lifted PPU code (TJ_GENERATED_VM_FAST_ACCESS).
 *
 * Here the common case is a plain big-endian load or store. Everything the
 * runtime accessors do besides debugging still goes through them:
 *   - stores to a line an SPU holds a reservation on (VM_WRITE_COH: the
 *     store must be ordered with the reservation and reported to it);
 *   - the raw SPU problem-state window (mailbox reads pop, register stores
 *     run the SPU), the GCM fence register (a read publishes queued fences),
 *     the first 64 KB (null-pointer reporting) and anything past ppu_vm_size.
 * Loads and stores are volatile: an inline load in a guest spin loop must not
 * be hoisted out of it, which the old out-of-line call guaranteed implicitly.
 * Built with TJ_VM_FAST=OFF, the debug hooks (PPU_RWATCH, PPU_WWATCH, ...)
 * see the lifted code's accesses again. */
#pragma once
#include "ppu_recomp.h"

extern "C" {
extern uint8_t* vm_base;
extern uint32_t ppu_vm_size;
extern uint32_t ppu_hle_inject_base;
int spu_coh_is_reserved(uint32_t addr);
}

namespace tj_vm {

/* Unaligned, aliasing-safe views of guest memory (PPC loads need no alignment). */
typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;

/* Guest addresses the runtime accessors must see (see above). */
static inline bool special(uint32_t a, uint32_t n)
{
    if (__builtin_expect(a < 0x10000u, 0)) return true;                       /* null page */
    if (__builtin_expect(a - 0xE0000000u < 6u * 0x00100000u, 0)) return true;  /* raw SPU */
    if (__builtin_expect(((a ^ (ppu_hle_inject_base + 0x2008u)) & ~0xFu) == 0, 0))
        return true;                                                            /* GCM ref */
    if (__builtin_expect(ppu_vm_size != 0, 0) && (uint64_t)a + n > ppu_vm_size) return true;
    return false;
}

static inline uint8_t read8(uint64_t ea)
{
    const uint32_t a = (uint32_t)ea;
    if (special(a, 1)) return ::vm_read8(ea);
    return *(const volatile uint8_t*)(vm_base + a);
}
static inline uint16_t read16(uint64_t ea)
{
    const uint32_t a = (uint32_t)ea;
    if (special(a, 2)) return ::vm_read16(ea);
    return __builtin_bswap16(*(const volatile u16u*)(vm_base + a));
}
static inline uint32_t read32(uint64_t ea)
{
    const uint32_t a = (uint32_t)ea;
    if (special(a, 4)) return ::vm_read32(ea);
    return __builtin_bswap32(*(const volatile u32u*)(vm_base + a));
}
static inline uint64_t read64(uint64_t ea)
{
    const uint32_t a = (uint32_t)ea;
    if (special(a, 8)) return ::vm_read64(ea);
    return __builtin_bswap64(*(const volatile u64u*)(vm_base + a));
}

/* A reserved line takes the runtime's coherent store (spu_coh_is_reserved is
 * the same check VM_WRITE_COH makes first). */
static inline void write8(uint64_t ea, uint8_t v)
{
    const uint32_t a = (uint32_t)ea;
    if (special(a, 1) || spu_coh_is_reserved(a)) { ::vm_write8(ea, v); return; }
    *(volatile uint8_t*)(vm_base + a) = v;
}
static inline void write16(uint64_t ea, uint16_t v)
{
    const uint32_t a = (uint32_t)ea;
    if (special(a, 2) || spu_coh_is_reserved(a)) { ::vm_write16(ea, v); return; }
    *(volatile u16u*)(vm_base + a) = __builtin_bswap16(v);
}
static inline void write32(uint64_t ea, uint32_t v)
{
    const uint32_t a = (uint32_t)ea;
    if (special(a, 4) || spu_coh_is_reserved(a)) { ::vm_write32(ea, v); return; }
    *(volatile u32u*)(vm_base + a) = __builtin_bswap32(v);
}
static inline void write64(uint64_t ea, uint64_t v)
{
    const uint32_t a = (uint32_t)ea;
    if (special(a, 8) || spu_coh_is_reserved(a)) { ::vm_write64(ea, v); return; }
    *(volatile u64u*)(vm_base + a) = __builtin_bswap64(v);
}

} /* namespace tj_vm */

#define vm_read8   tj_vm::read8
#define vm_read16  tj_vm::read16
#define vm_read32  tj_vm::read32
#define vm_read64  tj_vm::read64
#define vm_write8  tj_vm::write8
#define vm_write16 tj_vm::write16
#define vm_write32 tj_vm::write32
#define vm_write64 tj_vm::write64
