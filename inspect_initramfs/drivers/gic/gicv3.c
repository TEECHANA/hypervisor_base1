/*
 * gicv3.c — GICv3 distributor init + virtual/physical IRQ routing
 */
#include "gicv3.h"
#include "../../lib/log/log.h"
#include "../../arch/arm64/include/arm_regs.h"

/* GICD register offsets */
#define GICD_CTLR           0x000u
#define GICD_ISENABLER(n)   (0x100u + (n)*4u)
#define GICD_ICENABLER(n)   (0x180u + (n)*4u)
#define GICD_IPRIORITYR(n)  (0x400u + (n)*4u)
#define GICD_ITARGETSR(n)   (0x800u + (n)*4u)
#define GICD_ICFGR(n)       (0xC00u + (n)*4u)

#define GICD_CTLR_EN_GRP0   BIT(0)
#define GICD_CTLR_EN_GRP1NS BIT(1)
#define GICD_CTLR_ARE_NS    BIT(4)

/* ICC (CPU interface) system registers */
#define ICC_SRE_SRE         BIT(0)
#define ICC_SRE_DFB         BIT(1)
#define ICC_SRE_DIB         BIT(2)
#define ICC_CTLR_CBPR       BIT(0)
#define ICC_IGRPEN1_EN      BIT(0)

/* ICH (Virtual CPU Interface) */
#define ICH_HCR_EN          BIT(0)

/* CNTHP EL2 physical timer — PPI 26 on most ARM platforms */
#define CNTHP_PPI           26u

static volatile u32 *_gicd;
static void gd_wr(u32 o, u32 v) { *(volatile u32*)((u8*)_gicd + o) = v; }
static u32  gd_rd(u32 o)        { return *(volatile u32*)((u8*)_gicd + o); }

static void udelay(u32 us)
{
    u64 f = READ_SYSREG(cntfrq_el0);
    u64 s = READ_SYSREG(cntpct_el0);
    u64 t = (f / 1000000ULL) * us;
    while ((READ_SYSREG(cntpct_el0) - s) < t) {}
}

err_t gic_init(u64 gicd, u64 gicr)
{
    UNUSED(gicr);
    _gicd = (volatile u32*)(uintptr_t)gicd;

    /* 1. Enable GIC distributor: affinity routing + both groups */
    gd_wr(GICD_CTLR, 0);
    udelay(1);
    gd_wr(GICD_CTLR, GICD_CTLR_ARE_NS | GICD_CTLR_EN_GRP0 | GICD_CTLR_EN_GRP1NS);

    /* 2. Enable CPU interface system register access */
    u64 sre; __asm__ volatile("mrs %0, icc_sre_el2" : "=r"(sre));
    sre |= ICC_SRE_SRE | ICC_SRE_DFB | ICC_SRE_DIB;
    __asm__ volatile("msr icc_sre_el2, %0\nisb" :: "r"(sre));

    /* 3. Set priority mask: allow all priorities */
    __asm__ volatile("msr icc_pmr_el1, %0" :: "r"(0xFFULL));

    /* 4. Set binary point */
    __asm__ volatile("msr icc_bpr1_el1, %0" :: "r"(0ULL));

    /* 5. Enable Group 1 interrupts */
    __asm__ volatile("msr icc_igrpen1_el1, %0\nisb" :: "r"((u64)ICC_IGRPEN1_EN));

    /* 6. Enable virtual CPU interface at EL2 */
    u64 ich_hcr; __asm__ volatile("mrs %0, ich_hcr_el2" : "=r"(ich_hcr));
    ich_hcr |= ICH_HCR_EN;
    __asm__ volatile("msr ich_hcr_el2, %0\nisb" :: "r"(ich_hcr));

    /* 7. Enable CNTHP PPI 26 and virtual timer PPI 27 at SGI/PPI bank
     * PPIs are in GICR_ISENABLER0 (offset 0x10100 from GICR base)
     * For QEMU virt with one CPU, GICR base = 0x080A0000
     * GICR_ISENABLER0 = GICR_base + 0x10100 (SGI/PPI frame) */
    volatile u32 *gicr_sgi = (volatile u32*)(uintptr_t)(0x080A0000ULL + 0x10100ULL);
    *gicr_sgi = BIT(26) | BIT(27);   /* enable PPI 26 (CNTHP) + PPI 27 (virtual timer) */

    /* Set priority for PPI 27 (virtual timer) = 0x80 (medium) */
    volatile u32 *gicr_prio = (volatile u32*)(uintptr_t)(0x080A0000ULL + 0x10400ULL);
    u32 prio_reg = gicr_prio[27/4];
    prio_reg &= ~(0xFFu << ((27 % 4) * 8));
    prio_reg |=  (0x80u << ((27 % 4) * 8));
    gicr_prio[27/4] = prio_reg;

    /* Also set PPI 26 priority */
    prio_reg = gicr_prio[26/4];
    prio_reg &= ~(0xFFu << ((26 % 4) * 8));
    prio_reg |=  (0x80u << ((26 % 4) * 8));
    gicr_prio[26/4] = prio_reg;

    /* Keep GICD enable for SPIs */
    gd_wr(GICD_ISENABLER(CNTHP_PPI / 32), BIT(CNTHP_PPI % 32));
    /* Priority 0x80 (mid) for timer */
    gd_wr(GICD_IPRIORITYR(CNTHP_PPI / 4),
          gd_rd(GICD_IPRIORITYR(CNTHP_PPI / 4)) | (0x80u << ((CNTHP_PPI % 4) * 8)));

    LOG_INFO("GICv3 init: GICD=0x%lx", gicd);
    return E_OK;
}

void gic_enable_irq(u32 irq)
{
    gd_wr(GICD_ISENABLER(irq / 32), BIT(irq % 32));
}

void gic_disable_irq(u32 irq)
{
    gd_wr(GICD_ICENABLER(irq / 32), BIT(irq % 32));
}

/* Inject a virtual IRQ into the currently running guest */
void gic_inject_virq(u32 virq, u32 prio)
{
    /* ICH_LR<n>_EL2 format:
     * [62:61] State: 01=Pending
     * [60]    HW: 0=virtual only
     * [59]    Group: 1=Group1
     * [55:48] Priority
     * [31:0]  vINTID */
    u64 lr = (u64)(virq & 0xFFFFFFu)
           | ((u64)(prio & 0xFF) << 48)
           | (1ULL << 59)    /* Group 1 */
           | (1ULL << 61);   /* State = Pending (bits[62:61]=01) */
    __asm__ volatile("msr ich_lr0_el2, %0\nisb" :: "r"(lr));
}

/*
 * gic_handle_irq — called from hyp_irq_handler.
 * Reads IAR1 to ack, routes the IRQ, then EOIs.
 */
void gic_handle_irq(void *regs)
{
    UNUSED(regs);
    u64 iar;
    __asm__ volatile("mrs %0, icc_iar1_el1" : "=r"(iar));
    u32 id = (u32)(iar & 0xFFFFFFu);

    if (id >= 1020) return;  /* spurious */

    /* CNTHP physical timer PPIs: 26 (EL2) and 27 (virtual) */
    if (id == CNTHP_PPI || id == 30) {
        /* EL2 physical timer — hypervisor scheduler tick */
        __asm__ volatile("msr cnthp_ctl_el2, %0" :: "r"(CNTHP_IMASK));
        extern void sched_on_timer(void);
        sched_on_timer();
    } else if (id == 27) {
        /* Virtual timer PPI 27 — inject into current guest via List Register */
        gic_inject_virq(27, 0x80);
        /* Do NOT EOI here — guest will EOI via ICC_EOIR1_EL1 */
        return;
    } else {
        extern void irq_route_to_vm(u32 phys_irq);
        irq_route_to_vm(id);
    }

    /* EOI + deactivate */
    __asm__ volatile("msr icc_eoir1_el1, %0" :: "r"(iar));
    __asm__ volatile("msr icc_dir_el1,   %0" :: "r"(iar));
}
