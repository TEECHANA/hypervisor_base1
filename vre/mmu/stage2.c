/*
 * stage2.c — ARM Stage-2 (IPA→PA) page table manager
 *
 * Phase 1 — 2MB block fast-path REMOVED.
 * The 2MB block mapping caused device-address overlap bugs:
 * large RAM regions (e.g. RTOS 240MB) covered device IPAs
 * (UART 0x09000000, GIC 0x08000000) with normal-memory block
 * descriptors, shadowing the subsequent device-memory 4KB page
 * mappings. Guest UART writes were cached and never reached hardware.
 *
 * Keeping: s2_unmap(), s2_ipa_is_mapped(), pool exhaustion panic.
 * All mappings use 4KB pages only — correct and safe.
 * 2MB block support can be re-added in Phase 2 with proper
 * device-hole exclusion logic in s2_map().
 */

#include "stage2.h"
#include "../../core/vm/vm.h"
#include "../../include/config.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"
#include "../../arch/arm64/include/arm_regs.h"

/* ── Page-table memory pool ── */
#define PT_POOL_PAGES 1024
static u64 _pt_pool[PT_POOL_PAGES][512] __aligned(4096);
static u32 _pt_idx = 0;

static u64 *pt_alloc(void)
{
    if (_pt_idx >= PT_POOL_PAGES) {
        LOG_ERROR("S2: page-table pool EXHAUSTED (%u pages used)", _pt_idx);
        LOG_ERROR("S2: increase PT_POOL_PAGES in stage2.c and rebuild");
        for (;;) asm volatile("wfi");
    }
    u64 *p = _pt_pool[_pt_idx++];
    memset(p, 0, 4096);
    return p;
}

static u8 _vmid = 1;

err_t s2_create(struct vm *vm)
{
    u64 *pgd = pt_alloc();
    vm->s2_pgd = (paddr_t)(uintptr_t)pgd;
    vm->vttbr   = ((u64)_vmid++ << 48) | (vm->s2_pgd & ~0xFFFULL);
    LOG_DEBUG("S2 create: VM %d PGD=%lx VTTBR=%lx",
              vm->id, vm->s2_pgd, vm->vttbr);
    return E_OK;
}

static inline void s2_dsb_tlb_isb(void)
{
    asm volatile(
        "dsb ish\n"
        "tlbi vmalls12e1is\n"
        "dsb ish\n"
        "isb\n"
        ::: "memory");
}

/*
 * s2_map — map [ipa, ipa+sz) → [pa, pa+sz) using 4KB pages only.
 *
 * NOTE: 2MB block mapping deliberately omitted here.
 * If a RAM region covers a device IPA (e.g. UART at 0x09000000),
 * a 2MB block would map it as normal memory, shadowing the correct
 * device-memory 4KB page added afterward. Use 4KB pages throughout
 * until s2_map has proper device-hole exclusion logic.
 */
err_t s2_map(struct vm *vm, ipa_t ipa, paddr_t pa, u64 sz, u64 flags)
{
    if (!vm
        || !IS_ALIGNED(ipa, 4096)
        || !IS_ALIGNED(pa,  4096)
        || !IS_ALIGNED(sz,  4096))
        return E_INVAL;

    u64 *l1 = (u64 *)(uintptr_t)vm->s2_pgd;

    LOG_INFO("S2 MAP START IPA=%lx -> PA=%lx size=%lx", ipa, pa, sz);

    u64 off = 0;
    while (off < sz) {
        ipa_t   c_ipa = ipa + off;
        paddr_t c_pa  = pa  + off;

        if (c_ipa == 0x02BFF000ULL)
            LOG_INFO("Mapped Linux fault page IPA=%lx -> PA=%lx",
                     c_ipa, c_pa);

        u32 i1 = (u32)((c_ipa >> 30) & 0x1FF);
        u32 i2 = (u32)((c_ipa >> 21) & 0x1FF);
        u32 i3 = (u32)((c_ipa >> 12) & 0x1FF);

        if (!(l1[i1] & 1)) {
            u64 *l2 = pt_alloc();
            l1[i1] = ((u64)(uintptr_t)l2 & ~0xFFFULL)
                   | S2_DESC_VALID | S2_DESC_TABLE;
        }
        u64 *l2 = (u64 *)(uintptr_t)(l1[i1] & ~0xFFFULL);

        if (!(l2[i2] & 1)) {
            u64 *l3 = pt_alloc();
            l2[i2] = ((u64)(uintptr_t)l3 & ~0xFFFULL)
                   | S2_DESC_VALID | S2_DESC_TABLE;
        }
        u64 *l3 = (u64 *)(uintptr_t)(l2[i2] & ~0xFFFULL);

        u64 desc = (c_pa & ~0xFFFULL) | flags | S2_DESC_VALID | S2_DESC_PAGE;
        l3[i3] = desc;

        if (c_ipa == 0x2236000ULL)
            LOG_INFO("FAULT PAGE MAP IPA=%lx PA=%lx DESC=%lx",
                     c_ipa, c_pa, desc);

        LOG_DEBUG("MAP L3[%u] = %lx", i3, desc);
        off += 4096;
    }

    s2_dsb_tlb_isb();
    return E_OK;
}

err_t s2_unmap(struct vm *vm, ipa_t ipa, u64 sz)
{
    if (!vm
        || !IS_ALIGNED(ipa, 4096)
        || !IS_ALIGNED(sz,  4096))
        return E_INVAL;

    u64 *l1 = (u64 *)(uintptr_t)vm->s2_pgd;

    LOG_INFO("S2 UNMAP IPA=%lx size=%lx VM=%d", ipa, sz, vm->id);

    u64 off = 0;
    while (off < sz) {
        ipa_t c_ipa = ipa + off;
        u32 i1 = (u32)((c_ipa >> 30) & 0x1FF);
        u32 i2 = (u32)((c_ipa >> 21) & 0x1FF);
        u32 i3 = (u32)((c_ipa >> 12) & 0x1FF);

        if (!(l1[i1] & 1)) { off += 4096; continue; }
        u64 *l2 = (u64 *)(uintptr_t)(l1[i1] & ~0xFFFULL);

        if (!(l2[i2] & 1)) { off += 4096; continue; }
        u64 *l3 = (u64 *)(uintptr_t)(l2[i2] & ~0xFFFULL);

        if (l3[i3] & 1) l3[i3] = 0;
        off += 4096;
    }

    s2_dsb_tlb_isb();
    return E_OK;
}

bool s2_ipa_is_mapped(struct vm *vm, ipa_t ipa, u64 sz)
{
    if (!vm) return false;
    u64 *l1 = (u64 *)(uintptr_t)vm->s2_pgd;
    u64  off;

    for (off = 0; off < sz; off += 4096) {
        ipa_t c_ipa = ipa + off;
        u32 i1 = (u32)((c_ipa >> 30) & 0x1FF);
        if (!(l1[i1] & 1)) continue;
        u64 *l2 = (u64 *)(uintptr_t)(l1[i1] & ~0xFFFULL);

        u32 i2 = (u32)((c_ipa >> 21) & 0x1FF);
        if (!(l2[i2] & 1)) continue;
        if (!(l2[i2] & 2)) return true;   /* block descriptor */

        u64 *l3 = (u64 *)(uintptr_t)(l2[i2] & ~0xFFFULL);
        u32 i3  = (u32)((c_ipa >> 12) & 0x1FF);
        if (l3[i3] & 1) return true;
    }
    return false;
}

void s2_flush_tlb(struct vm *vm)
{
    if (!vm) return;
    asm volatile(
        "msr vttbr_el2, %0\n"
        "isb\n"
        "tlbi vmalls12e1is\n"
        "dsb ish\n"
        "isb\n"
        :: "r"(vm->vttbr) : "memory");
}

void s2_destroy(struct vm *vm)
{
    if (!vm) return;
    s2_flush_tlb(vm);
    vm->s2_pgd = 0;
    vm->vttbr  = 0;
}
