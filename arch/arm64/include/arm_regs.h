#ifndef ARM_REGS_H
#define ARM_REGS_H
#include "../../../include/types.h"
#define EC_WFI   0x01   /* WFI/WFE trap */
#define READ_SYSREG(r)    ({u64 _v;__asm__ volatile("mrs %0,"#r:"=r"(_v));_v;})
#define WRITE_SYSREG(r,v) __asm__ volatile("msr "#r",%0"::"r"((u64)(v)))
#define ISB()             __asm__ volatile("isb":::"memory")
#define DSB_SY()          __asm__ volatile("dsb sy":::"memory")
#define DSB_ISH()         __asm__ volatile("dsb ish":::"memory")
#define WFI()             __asm__ volatile("wfi")

/* HCR_EL2 */
#define HCR_VM    BIT(0)
#define HCR_FMO   BIT(3)
#define HCR_IMO   BIT(4)
#define HCR_AMO   BIT(5)
#define HCR_TSC   BIT(19)
#define HCR_RW    BIT(31)
/* HCR_GUEST is defined canonically in core/sched/sched.c (its only user),
 * which additionally sets TWI (bit 13) to trap guest WFI; do not define a
 * competing version here. */

/* SPSR to ERET into EL1h, all interrupts unmasked */
#define SPSR_EL1H   0x000003C5ULL

/* VTCR_EL2: 4 KB, 40-bit IPA, L1 start, WB-WA IS */
/* VTCR_EL2 fields for:
 * 4KB granule
 * 40-bit IPA
 * Start level = L1
 * Inner-shareable
 * WBWA cacheable walks
 */

#define VTCR_T0SZ          (25ULL)
#define VTCR_SL0_L1        (1ULL << 6)
#define VTCR_IRGN0_WBWA    (1ULL << 8)
#define VTCR_ORGN0_WBWA    (1ULL << 10)
#define VTCR_SH0_INNER     (3ULL << 12)
#define VTCR_TG0_4K        (0ULL << 14)
#define VTCR_PS_40BIT      (2ULL << 16)
#define VTCR_RES1          (1ULL << 31)   /* RES1 — must always be 1 */

#define VTCR_DEFAULT \
    (VTCR_T0SZ          | \
     VTCR_SL0_L1        | \
     VTCR_IRGN0_WBWA    | \
     VTCR_ORGN0_WBWA    | \
     VTCR_SH0_INNER     | \
     VTCR_TG0_4K        | \
     VTCR_PS_40BIT      | \
     VTCR_RES1)

/* Exception class extraction */
#define ESR_EC(e)  (((e)>>26)&0x3F)
#define EC_HVC64   0x16
#define EC_SMC64   0x17
#define EC_DABT_L  0x24
#define EC_IABT_L  0x20
#define S2_DESC_VALID   (1ULL << 0)
#define S2_DESC_TABLE   (1ULL << 1)
#define S2_DESC_PAGE    (1ULL << 1)
/* CNTHP control */
#define CNTHP_ENABLE  BIT(0)
#define CNTHP_IMASK   BIT(1)
#define CNTHP_ISTATUS BIT(2)

/* ------------------------------------------------ */
/* ARMv8-A Stage-2 page descriptor bits             */
/* ------------------------------------------------ */

/* Descriptor valid */
/* Stage-2 descriptor bits */

/* Stage-2 descriptor bits */

#define S2_VALID       (1ULL << 0)

/* Memory attribute index */
/* S2_MEMATTR_DEV is defined canonically in vre/mmu/stage2.h (the actual user,
 * core/vm/vm.c, includes that header); do not redefine it here. */
#define S2_MEMATTR_WB  (0xFULL << 2)

/* Stage-2 access permissions */
#define S2_S2AP_RW     (3ULL << 6)
#define S2_S2AP_RO     (1ULL << 6)

/* Shareability */
#define S2_SH_NONE     (0ULL << 8)
#define S2_SH_OUTER    (2ULL << 8)
#define S2_SH_INNER    (3ULL << 8)

/* Access flag */
#define S2_AF          (1ULL << 10)

/* Execute-never: S2_XN is defined canonically in vre/mmu/stage2.h
 * (numerically identical: 2<<53 == 1<<54); do not redefine it here. */

#endif /* ARM_REGS_H */
