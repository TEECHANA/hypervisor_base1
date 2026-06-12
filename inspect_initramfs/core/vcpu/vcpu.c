#include "vcpu.h"
#include "../vm/vm.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"
#include "../../arch/arm64/include/arm_regs.h"

/* Defined here; declared extern in entry.S via global symbol */
vcpu_t *g_current_vcpu[MAX_PHYS_CPUS];

err_t vcpu_init(vcpu_t *vc, struct vm *vm, u32 id)
{
    if(!vc || !vm) return E_INVAL;
    memset(vc, 0, sizeof(*vc));

    vc->vcpu_id    = id;
    vc->vm         = vm;
    vc->state      = VCPU_READY;
    vc->vttbr_el2  = vm->vttbr;

    /* EL1 reset state: MMU off, caches off */
    vc->sysregs.sctlr_el1   = 0x00C50838ULL;
    vc->sysregs.cpacr_el1   = (3ULL << 20);   /* FPEN: FP/SIMD permitted */
    vc->sysregs.cntkctl_el1 = 0ULL;
    vc->sysregs.mair_el1 = 0xFFULL;
    vc->sysregs.tcr_el1  = 0ULL;
    vc->sysregs.ttbr0_el1 = 0ULL;
    vc->sysregs.ttbr1_el1 = 0ULL;
    /* Entry point */
    vc->regs.elr_el2  = vm->entry_ipa;
    vc->regs.spsr_el2 = SPSR_EL1H;            /* EL1h, all interrupts unmasked */
    vc->regs.x[0]     = vm->dtb_ipa;          /* Linux convention: DTB in x0 */
    vc->regs.x[1] = 0ULL;
    vc->regs.x[2] = 0ULL;
    vc->regs.x[3] = 0ULL;

    LOG_INFO("vCPU %d init VM %d", id, vm->id);
    LOG_INFO("  entry IPA : 0x%lx", vm->entry_ipa);
    LOG_INFO("  DTB IPA   : 0x%lx", vm->dtb_ipa);
    LOG_INFO("  VTTBR     : 0x%lx", vm->vttbr);
    return E_OK;
}

void vcpu_set_entry(vcpu_t *vc, paddr_t entry, u64 arg0)
{
    vc->reset_pc      = entry;
    vc->reset_x0      = arg0;
    vc->regs.elr_el2  = entry;
    vc->regs.spsr_el2 = SPSR_EL1H;
    vc->regs.x[0]     = arg0;
}

void vcpu_stop(vcpu_t *vc)
{
    if (vc) vc->state = VCPU_STOPPED;
}

/*
 * vcpu_do_switch — called by the scheduler (from IRQ context) to
 * perform the actual hardware context switch between two vCPUs.
 *
 * At this point the SAVE_GUEST_REGS macro has already saved prev's
 * GPRs into prev->regs.  We just need to:
 *   1. Save prev's EL1 system registers
 *   2. Update g_current_vcpu[0] to next
 *   3. Load next's EL1 system registers
 *   4. Switch VTTBR_EL2 (Stage-2 page table + VMID)
 *   5. Flush TLBs for old VMID
 *
 * RESTORE_GUEST_REGS in the vector exit path will then load next's
 * GPRs from next->regs automatically.
 */
void vcpu_do_switch(vcpu_t *prev, vcpu_t *next)
{
    if (!next) return;

    /* 1. Save prev EL1 state */
    if (prev && prev != next) {
        vcpu_save_sysregs(&prev->sysregs);
        prev->state = VCPU_READY;
    }

    /* 2. Update current vcpu pointer — RESTORE_GUEST_REGS reads this */
    g_current_vcpu[0] = next;
    next->state = VCPU_RUNNING;

    /* 3. Switch VTTBR_EL2: new VMID + Stage-2 page table */
    __asm__ volatile(
        "dsb ish\n"
        "msr vttbr_el2, %0\n"
        "isb\n"
        "tlbi vmalls12e1is\n"
        "dsb ish\n"
        "isb\n"
        :
        : "r"(next->vttbr_el2)
        : "memory");

    /* 4. Restore next EL1 system registers */
    vcpu_restore_sysregs(&next->sysregs);
}
