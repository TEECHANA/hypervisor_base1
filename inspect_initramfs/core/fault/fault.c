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
extern void mmio_handle(u64 fault_addr, u64 esr, void *regs);

void fault_dabt(void *regs, u64 esr)
{
    u64 far   = READ_SYSREG(far_el2);
    u32 dfsc  = (u32)(esr & 0x3F);
    u64 hpfar = READ_SYSREG(hpfar_el2);

    LOG_WARN("DABT: FAR=0x%lx HPFAR=0x%lx ESR=0x%lx DFSC=0x%x",
             far, hpfar, esr, dfsc);

    bool is_write = (bool)((esr >> 6) & 1);

    /* DFSC 0x10–0x13: stage-2 translation fault → MMIO trap */
    if (dfsc >= 0x10 && dfsc <= 0x13) {
        if ((far & ~0xFFFULL) == 0x09000000ULL)
            LOG_DEBUG("Guest UART write at 0x%lx", far);   /* optional trace */
        mmio_handle(far, esr, regs);
        return;
    }

    LOG_WARN("DABT unhandled: FAR=0x%lx ESR=0x%lx DFSC=0x%x", far, esr, dfsc);

    /* Advance ELR_EL2 past the faulting instruction directly via sysreg.
     * Prevents infinite fault loops without needing vcpu headers here. */
    u64 elr = READ_SYSREG(elr_el2);
    WRITE_SYSREG(elr_el2, elr + 4);
    LOG_WARN("Advanced ELR_EL2: 0x%lx -> 0x%lx", elr, elr + 4);
}	
void fault_iabt(void *regs, u64 esr)
{
    UNUSED(regs);

    u64 far   = READ_SYSREG(far_el2);
    u64 hpfar = READ_SYSREG(hpfar_el2);

    /*
     * HPFAR_EL2[63:4] holds IPA[51:12] of the faulting access.
     * Reconstruct the full IPA: shift left by 8, then add the
     * page offset from FAR (bits [11:0]).
     */
    u64 ipa = ((hpfar & ~0xFULL) << 8) | (far & 0xFFFULL);

    u64 vttbr = READ_SYSREG(vttbr_el2);
    u64 *root = (u64 *)(uintptr_t)(vttbr & 0x0000FFFFFFFFF000ULL);

    LOG_WARN("IABT: FAR=0x%lx HPFAR=0x%lx IPA=0x%lx ESR=0x%lx EC=0x%lx ISS=0x%lx",
             far, hpfar, ipa,
             esr, (esr >> 26) & 0x3F, esr & 0xFFFFFF);

    LOG_WARN("Walking S2 tables for IPA=0x%lx", ipa);
    s2_walk(root, ipa);

    /*
     * Advance ELR past the faulting instruction so we don't
     * infinite-loop. In a real hypervisor you'd inject a
     * guest abort here instead.
     */
    u64 elr = READ_SYSREG(elr_el2);
    WRITE_SYSREG(elr_el2, elr + 4);
    LOG_WARN("Advanced ELR_EL2: 0x%lx -> 0x%lx", elr, elr + 4);
}
