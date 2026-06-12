#ifndef HYP_REGS_H
#define HYP_REGS_H
#include "../../../include/types.h"

/*
 * guest_regs_t  (272 bytes, 16-byte aligned)
 * Layout MUST match SAVE_GUEST / RESTORE_GUEST in entry.S
 *
 *  +0x000  x[0..30]   (31 * 8 = 248)
 *  +0x0F8  sp_el0     (+8)
 *  +0x100  elr_el2    (+8)
 *  +0x108  spsr_el2   (+8)
 */
 /*
typedef struct __aligned(16) {
    u64 x[31];
    u64 sp_el0;
    u64 elr_el2;
    u64 spsr_el2;
} guest_regs_t;
*/
typedef struct {
    u64 x[31];        /* x0-x30 */

    /*
     * Guest stack pointer
     * Saved from SP_EL0 during exception entry
     */
    u64 sp;

    /*
     * Exception return state
     */
    u64 elr_el2;
    u64 spsr_el2;

} guest_regs_t;
/*
 * el1_sysregs_t  (152 bytes)
 * Offsets used by context.S — keep in sync.
 *
 *  +0x00 sctlr   +0x08 ttbr0  +0x10 ttbr1  +0x18 tcr
 *  +0x20 mair    +0x28 amair  +0x30 vbar    +0x38 sp_el1
 *  +0x40 elr_el1 +0x48 spsr1  +0x50 esr_el1 +0x58 far_el1
 *  +0x60 tpidr0  +0x68 tpidr1 +0x70 tpidrro +0x78 cpacr
 *  +0x80 cntkctl +0x88 ctxidr +0x90 par_el1
 */
typedef struct {
    u64 sctlr_el1;       /* 0x00 */
    u64 ttbr0_el1;       /* 0x08 */
    u64 ttbr1_el1;       /* 0x10 */
    u64 tcr_el1;         /* 0x18 */
    u64 mair_el1;        /* 0x20 */
    u64 amair_el1;       /* 0x28 */
    u64 vbar_el1;        /* 0x30 */
    u64 sp_el1;          /* 0x38 */
    u64 elr_el1;         /* 0x40 */
    u64 spsr_el1;        /* 0x48 */
    u64 esr_el1;         /* 0x50 */
    u64 far_el1;         /* 0x58 */
    u64 tpidr_el0;       /* 0x60 */
    u64 tpidr_el1;       /* 0x68 */
    u64 tpidrro_el0;     /* 0x70 */
    u64 cpacr_el1;       /* 0x78 */
    u64 cntkctl_el1;     /* 0x80 */
    u64 contextidr_el1;  /* 0x88 */
    u64 par_el1;         /* 0x90 */
} el1_sysregs_t;

#endif /* HYP_REGS_H */
