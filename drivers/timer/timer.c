/*
 * timer.c — ARM EL2 physical timer (CNTHP)
 *
 * CNTHP fires as PPI 26, routed to EL2 by HCR_EL2.
 * HCR_EL2.IMO=1 means guest IRQs go to EL2 — the timer fires
 * in the guest world and traps to our IRQ vector.
 */
#include "timer.h"
#include "../../arch/arm64/include/arm_regs.h"

void timer_set_us(u32 us)
{
    u64 freq  = READ_SYSREG(cntfrq_el0);
    u64 ticks = (freq / 1000000ULL) * (u64)us;
    if (ticks == 0) ticks = 1000;          /* safety floor */

    /* Clear any pending interrupt, set new countdown */
    WRITE_SYSREG(cnthp_ctl_el2, CNTHP_IMASK); /* mask while reprogramming */
    WRITE_SYSREG(cnthp_tval_el2, ticks);
    WRITE_SYSREG(cnthp_ctl_el2, CNTHP_ENABLE); /* unmask + enable */
    ISB();
}

/*
void timer_disable(void)
{
    WRITE_SYSREG(cnthp_ctl_el2, CNTHP_IMASK);
    ISB();
}
*/

void timer_disable(void)
{
    /* disable + mask */
    WRITE_SYSREG(cnthp_ctl_el2, 0x0);
    ISB();
}

u64 timer_now_us(void)
{
    u64 cnt  = READ_SYSREG(cntpct_el0);
    u64 freq = READ_SYSREG(cntfrq_el0);
    return freq ? (cnt * 1000000ULL) / freq : 0;
}
