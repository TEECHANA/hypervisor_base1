/*
 * gicv3.c — GICv3 distributor init + virtual/physical IRQ routing
 *
 * Phase 1 additions:
 *   ① gic_init() reads ICH_VTR_EL2 to discover how many list registers
 *     the hardware actually provides (min 1, max 16).  Previously the
 *     code hard-coded 2 LRs.
 *   ② gic_inject_virq_lr() — new exported function.  Scans all available
 *     LRs for one in INVALID state (bits[63:62]==0b00) and writes the
 *     virtual IRQ there.  Falls back to LR0 if all are occupied (rare).
 *   ③ The old static gic_inject_virq_lr1() is removed; callers that
 *     used it (UART injection in gic_handle_irq, irq_router) now call
 *     the generic gic_inject_virq_lr() instead.
 */

#include "gicv3.h"
#include "../../vre/irq/vgic.h"    /* Phase 4B */
#include "../../core/vm/vm.h"        /* Phase 4B: vm_t->vgic */
#include "../../core/vcpu/vcpu.h"    /* Phase 4B */
#include "../../lib/log/log.h"
#include "../../arch/arm64/include/arm_regs.h"
#include "../timer/timer.h"

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
#define ICC_IGRPEN1_EN      BIT(0)

/* ICH (Virtual CPU Interface) */
#define ICH_HCR_EN          BIT(0)

/*
 * ICH_VTR_EL2[4:0] = ListRegs — number of list registers minus 1.
 * Valid range: 0b00000 (1 LR) to 0b01111 (16 LRs).
 */
#define ICH_VTR_LISTREGS_MASK   0x1Fu

#define CNTHP_PPI   26u
#define UART_SPI    33u
#define LINUX_VM_ID 1u

/* ICH_LR state field [63:62] */
#define ICH_LR_STATE_INVALID    (0ULL << 62)
#define ICH_LR_STATE_PENDING    (1ULL << 62)

static volatile u32 *_gicd;
static u32           _num_lrs = 2;   /* updated in gic_init() from ICH_VTR */

static void gd_wr(u32 o, u32 v) { *(volatile u32 *)((u8 *)_gicd + o) = v; }
static u32  gd_rd(u32 o)        { return *(volatile u32 *)((u8 *)_gicd + o); }

extern bool g_sched_stopped;

static void udelay(u32 us)
{
    u64 f = READ_SYSREG(cntfrq_el0);
    u64 s = READ_SYSREG(cntpct_el0);
    u64 t = (f / 1000000ULL) * us;
    while ((READ_SYSREG(cntpct_el0) - s) < t) {}
}

/* ── LR read/write helpers (LR 0..15) ─────────────────────────────── */

/*
 * We have to switch on the LR index at compile time because ARM
 * system register instructions encode the register number as an
 * immediate — you cannot pass it in a GPR.  A switch is ugly but
 * correct and generates compact straight-line code per case.
 */
static u64 lr_read(u32 n)
{
    u64 v = 0;
    switch (n) {
    case  0: asm volatile("mrs %0, ich_lr0_el2"  : "=r"(v)); break;
    case  1: asm volatile("mrs %0, ich_lr1_el2"  : "=r"(v)); break;
    case  2: asm volatile("mrs %0, ich_lr2_el2"  : "=r"(v)); break;
    case  3: asm volatile("mrs %0, ich_lr3_el2"  : "=r"(v)); break;
    case  4: asm volatile("mrs %0, ich_lr4_el2"  : "=r"(v)); break;
    case  5: asm volatile("mrs %0, ich_lr5_el2"  : "=r"(v)); break;
    case  6: asm volatile("mrs %0, ich_lr6_el2"  : "=r"(v)); break;
    case  7: asm volatile("mrs %0, ich_lr7_el2"  : "=r"(v)); break;
    case  8: asm volatile("mrs %0, ich_lr8_el2"  : "=r"(v)); break;
    case  9: asm volatile("mrs %0, ich_lr9_el2"  : "=r"(v)); break;
    case 10: asm volatile("mrs %0, ich_lr10_el2" : "=r"(v)); break;
    case 11: asm volatile("mrs %0, ich_lr11_el2" : "=r"(v)); break;
    case 12: asm volatile("mrs %0, ich_lr12_el2" : "=r"(v)); break;
    case 13: asm volatile("mrs %0, ich_lr13_el2" : "=r"(v)); break;
    case 14: asm volatile("mrs %0, ich_lr14_el2" : "=r"(v)); break;
    case 15: asm volatile("mrs %0, ich_lr15_el2" : "=r"(v)); break;
    default: break;
    }
    return v;
}

static void lr_write(u32 n, u64 v)
{
    switch (n) {
    case  0: asm volatile("msr ich_lr0_el2,  %0\nisb" :: "r"(v)); break;
    case  1: asm volatile("msr ich_lr1_el2,  %0\nisb" :: "r"(v)); break;
    case  2: asm volatile("msr ich_lr2_el2,  %0\nisb" :: "r"(v)); break;
    case  3: asm volatile("msr ich_lr3_el2,  %0\nisb" :: "r"(v)); break;
    case  4: asm volatile("msr ich_lr4_el2,  %0\nisb" :: "r"(v)); break;
    case  5: asm volatile("msr ich_lr5_el2,  %0\nisb" :: "r"(v)); break;
    case  6: asm volatile("msr ich_lr6_el2,  %0\nisb" :: "r"(v)); break;
    case  7: asm volatile("msr ich_lr7_el2,  %0\nisb" :: "r"(v)); break;
    case  8: asm volatile("msr ich_lr8_el2,  %0\nisb" :: "r"(v)); break;
    case  9: asm volatile("msr ich_lr9_el2,  %0\nisb" :: "r"(v)); break;
    case 10: asm volatile("msr ich_lr10_el2, %0\nisb" :: "r"(v)); break;
    case 11: asm volatile("msr ich_lr11_el2, %0\nisb" :: "r"(v)); break;
    case 12: asm volatile("msr ich_lr12_el2, %0\nisb" :: "r"(v)); break;
    case 13: asm volatile("msr ich_lr13_el2, %0\nisb" :: "r"(v)); break;
    case 14: asm volatile("msr ich_lr14_el2, %0\nisb" :: "r"(v)); break;
    case 15: asm volatile("msr ich_lr15_el2, %0\nisb" :: "r"(v)); break;
    default: break;
    }
}

/* ── gic_init ─────────────────────────────────────────────────────── */
err_t gic_init(u64 gicd, u64 gicr)
{
    UNUSED(gicr);
    _gicd = (volatile u32 *)(uintptr_t)gicd;

    /* 1. Enable GIC distributor */
    gd_wr(GICD_CTLR, 0);
    udelay(1);
    gd_wr(GICD_CTLR, GICD_CTLR_ARE_NS | GICD_CTLR_EN_GRP0 | GICD_CTLR_EN_GRP1NS);

    /* 2. CPU interface system register access */
    u64 sre;
    asm volatile("mrs %0, icc_sre_el2" : "=r"(sre));
    sre |= ICC_SRE_SRE | ICC_SRE_DFB | ICC_SRE_DIB;
    asm volatile("msr icc_sre_el2, %0\nisb" :: "r"(sre));

    /* 3. Allow all priorities */
    asm volatile("msr icc_pmr_el1,    %0" :: "r"(0xFFULL));
    asm volatile("msr icc_bpr1_el1,   %0" :: "r"(0ULL));
    asm volatile("msr icc_igrpen1_el1,%0\nisb" :: "r"((u64)ICC_IGRPEN1_EN));

    /* 4. Enable virtual CPU interface */
    u64 ich_hcr;
    asm volatile("mrs %0, ich_hcr_el2" : "=r"(ich_hcr));
    ich_hcr |= ICH_HCR_EN;
    asm volatile("msr ich_hcr_el2, %0\nisb" :: "r"(ich_hcr));

    /* Seed ICH_VMCR: VPMR=0xFF, VENG1=1 */
    u64 vmcr = (0xFFULL << 24) | (1ULL << 1);
    asm volatile("msr ich_vmcr_el2, %0\nisb" :: "r"(vmcr));

    /* 5. Phase 1: discover real LR count from ICH_VTR_EL2[4:0] */
    u64 vtr;
    asm volatile("mrs %0, ich_vtr_el2" : "=r"(vtr));
    _num_lrs = (u32)(vtr & ICH_VTR_LISTREGS_MASK) + 1;
    LOG_INFO("GICv3: %u list registers available (ICH_VTR=0x%lx)", _num_lrs, vtr);

    /* 6. Enable PPI 26 (CNTHP) and PPI 27 (virtual timer) */
    volatile u32 *gicr_sgi = (volatile u32 *)(uintptr_t)(gicr + 0x10100ULL);
    *gicr_sgi = BIT(26) | BIT(27);

    /* Priority 0x80 for PPI 26 and 27 */
    volatile u32 *gicr_prio = (volatile u32 *)(uintptr_t)(gicr + 0x10400ULL);
    {
        u32 ppis[2] = { 26u, 27u };
        u32 k;
        for (k = 0; k < 2; k++) {
            u32 ppi = ppis[k];
            u32 pr = gicr_prio[ppi / 4];
            pr &= ~(0xFFu << ((ppi % 4) * 8));
            pr |=  (0x80u << ((ppi % 4) * 8));
            gicr_prio[ppi / 4] = pr;
        }
    }

    gd_wr(GICD_ISENABLER(CNTHP_PPI / 32), BIT(CNTHP_PPI % 32));
    gd_wr(GICD_IPRIORITYR(CNTHP_PPI / 4),
          gd_rd(GICD_IPRIORITYR(CNTHP_PPI / 4))
          | (0x80u << ((CNTHP_PPI % 4) * 8)));

    LOG_INFO("GICv3 init: GICD=0x%lx", gicd);
    return E_OK;
}

/* ── gic_enable_irq / gic_disable_irq ── */
void gic_enable_irq(u32 irq)  { gd_wr(GICD_ISENABLER(irq / 32), BIT(irq % 32)); }
void gic_disable_irq(u32 irq) { gd_wr(GICD_ICENABLER(irq / 32), BIT(irq % 32)); }

/* ── gic_inject_virq (LR0 — timer path, unchanged) ── */
void gic_inject_virq(u32 virq, u32 prio)
{
    u64 lr = ((u64)virq & 0xFFFFFFFFULL)
           | ((u64)(prio & 0xFF) << 48)
           | (1ULL << 60)   /* Group 1 */
           | (1ULL << 62);  /* State = Pending */
    lr_write(0, lr);
}

/*
 * gic_inject_virq_lr — Phase 1: inject using the first free LR.
 *
 * Scans LR0..LR(n-1) for a slot whose state field [63:62] is INVALID
 * (0b00).  Writes the virtual IRQ there.  If all LRs are busy (all
 * pending/active), falls back to LR0 with a warning — the guest will
 * see whichever vIRQ was in LR0 overwritten, which is better than
 * silently dropping the injection.
 *
 * Called by irq_route_to_vm() and the UART path in gic_handle_irq().
 */
void gic_inject_virq_lr(u32 virq, u32 prio)
{
    /* Phase 4B: use guest-programmed priority and check enable/mask */
    extern vcpu_t *g_current_vcpu[];
    vcpu_t *vc = g_current_vcpu[0];
    if (vc && vc->vm) {
        vgic_dist_t *dist = &vc->vm->vgic;
        if (!vgic_is_enabled(dist, virq)) return;   /* disabled in vdist */
        u8 vprio = vgic_get_priority(dist, virq);
        if (vgic_is_masked(&vc->vgic_cpu, vprio)) return; /* PMR masked */
        prio = vprio;
    }
    u64 lr_val = ((u64)virq & 0xFFFFFFFFULL)
               | ((u64)(prio & 0xFF) << 48)
               | (1ULL << 60)   /* Group 1 */
               | (1ULL << 62);  /* State = Pending */

    for (u32 i = 0; i < _num_lrs; i++) {
        u64 cur = lr_read(i);
        /* INVALID state = bits[63:62] == 0b00 */
        if ((cur >> 62) == 0) {
            lr_write(i, lr_val);
            LOG_DEBUG("GIC: injected vIRQ %u via LR%u", virq, i);
            return;
        }
    }

    /* All LRs occupied — overwrite LR0 as last resort */
    LOG_WARN("GIC: all %u LRs busy, overwriting LR0 with vIRQ %u", _num_lrs, virq);
    lr_write(0, lr_val);
}

/* ── gic_handle_irq ────────────────────────────────────────────────── */
void gic_handle_irq(void *regs)
{
    UNUSED(regs);

    /*
     * Check CNTHP_CTL_EL2.ISTATUS first, before ICC_IAR1.
     * When CNTHP fires during guest WFI, the GIC may return a spurious
     * ID (1023) from ICC_IAR1 because the EL2 timer bypasses normal
     * GIC active-state tracking. Reading ISTATUS directly is reliable.
     */
    u64 ctl;
    __asm__ volatile("mrs %0, cnthp_ctl_el2" : "=r"(ctl));
    if ((ctl & CNTHP_ENABLE) && (ctl & CNTHP_ISTATUS)) {
        if (!g_sched_stopped) {
            extern void sched_on_timer(void);
            sched_on_timer();   /* re-arms timer internally */
        }
        /* Mask to clear ISTATUS so it doesn't re-enter immediately */
        WRITE_SYSREG(cnthp_ctl_el2, CNTHP_IMASK);
    }

    /* Handle any GIC-signalled interrupt */
    u64 iar;
    __asm__ volatile("mrs %0, icc_iar1_el1" : "=r"(iar));
    u32 id = (u32)(iar & 0xFFFFFFu);

    if (id >= 1020) goto eoi;   /* spurious */

    if (id == CNTHP_PPI) {
        /* Already handled via ISTATUS above; skip to avoid double-switch */
    }
    else if (id == 27 || id == 30) {
        if (id == 27)
            WRITE_SYSREG(cntv_ctl_el0, CNTHP_ENABLE | CNTHP_IMASK);
        gic_inject_virq(id, 0x80);
    }
    else if (id == UART_SPI) {
        gic_inject_virq_lr(UART_SPI, 0x80);
        gic_disable_irq(UART_SPI);
    }
    else {
        extern void irq_route_to_vm(u32 phys_irq);
        irq_route_to_vm(id);
    }

eoi:
    __asm__ volatile("msr icc_eoir1_el1, %0" :: "r"(iar));
}
/* ── GIC distributor emulation (for MMIO trap) — unchanged ── */
err_t gic_dist_emulate(u64 addr, bool write, u64 *val, void *priv)
{
    UNUSED(priv);
    u32 offset = (u32)(addr & 0xFFFF);
    if (!write) {
        switch (offset) {
        case 0x000: *val = 0x00000003; break;
        case 0x004: *val = 0x0000003B; break;
        case 0x008: *val = 0x0000043B; break;
        case 0x00C: *val = 0x0;        break;
        case 0xFFE: *val = 0x0000003B; break;
        default:    *val = 0;          break;
        }
    }
    return E_OK;
}
