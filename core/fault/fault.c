/*
 * fault.c — Guest data/instruction abort handler
 *
 * Routes DABT faults to MMIO emulation engine or logs
 * stage-2 translation faults.
 */
#include "../../include/types.h"
#include "../../include/error.h"
#include "../../arch/arm64/include/arm_regs.h"
#include "../../lib/log/log.h"
#include "../../arch/arm64/s2_debug.h"
#include "../../core/vcpu/vcpu.h"
extern void mmio_handle(u64 fault_addr, u64 esr, void *regs);

void fault_dabt(void *regs, u64 esr)
{
    u64 far  = READ_SYSREG(far_el2);
    u64 elr  = READ_SYSREG(elr_el2);
    u32 dfsc = (u32)(esr & 0x3F);

    if ((far & 0xFF000000) == 0x08000000) {
        LOG_INFO("MMIO TRAP FAR=0x%lx ESR=0x%lx", far, esr);
        mmio_handle(far, esr, regs);
        return;
    }
    /*
    if (dfsc >= 0x10 && dfsc <= 0x13) {
        mmio_handle(far, esr, regs);
        return;
    }*/
    
    /* Only log translation faults for now */
    if (dfsc >= 0x10 && dfsc <= 0x13) {
        LOG_WARN("Stage-2 translation fault: FAR=0x%lx ESR=0x%lx DFSC=0x%x", far, esr, dfsc);

        u64 hpfar = READ_SYSREG(hpfar_el2);
        u64 ipa = ((hpfar & ~0xFULL) << 8) | (far & 0xFFFULL);

        LOG_WARN("IPA=0x%lx", ipa);

        return;
     }

    /* Advance saved ELR, not the hardware register */
    extern vcpu_t *g_current_vcpu[];
    if (g_current_vcpu[0])
        g_current_vcpu[0]->regs.elr_el2 = elr + 4;

   // LOG_WARN("DABT unhandled at 0x%lx, Advancing...", far);
    LOG_WARN(
        "DABT: FAR=0x%lx ESR=0x%lx DFSC=0x%x ELR=0x%lx",
        far,
        esr,
        dfsc,
        elr
    );

}	
void fault_iabt(void *regs, u64 esr)
{
    UNUSED(regs);

    u64 far   = READ_SYSREG(far_el2);
    u64 hpfar = READ_SYSREG(hpfar_el2);
    u64 ipa   = ((hpfar & ~0xFULL) << 8) | (far & 0xFFFULL);
    u64 vttbr = READ_SYSREG(vttbr_el2);
    u64 *root = (u64 *)(uintptr_t)(vttbr & 0x0000FFFFFFFFF000ULL);

    LOG_WARN("IABT: FAR=0x%lx HPFAR=0x%lx IPA=0x%lx ESR=0x%lx EC=0x%lx ISS=0x%lx",
             far, hpfar, ipa, esr, (esr >> 26) & 0x3F, esr & 0xFFFFFF);

    LOG_WARN("Walking S2 tables for IPA=0x%lx", ipa);
    s2_walk(root, ipa);

    /*
     * Advance the vCPU's SAVED ELR so RESTORE_GUEST_REGS picks up
     * the incremented value.  Writing only the hardware register
     * is overwritten by RESTORE_GUEST_REGS and has no effect.
     */
    extern vcpu_t *g_current_vcpu[];
    if (g_current_vcpu[0]) {
        u64 old_elr = g_current_vcpu[0]->regs.elr_el2;
        g_current_vcpu[0]->regs.elr_el2 = old_elr + 4;
        LOG_WARN("Advanced ELR_EL2: 0x%lx -> 0x%lx", old_elr, old_elr + 4);
    }
}
