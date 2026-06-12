#ifndef HYP_VCPU_H
#define HYP_VCPU_H
#include "../../include/types.h"
#include "../../include/error.h"
#include "../../include/config.h"
#include "../../arch/arm64/include/hyp_regs.h"

typedef enum {
    VCPU_IDLE=0, VCPU_READY, VCPU_RUNNING, VCPU_BLOCKED, VCPU_STOPPED
} vcpu_state_t;

/*
 * vcpu_t — MUST be 16-byte aligned; assembly offsets are fixed:
 *   +0x000  guest_regs_t  regs      (272 B)
 *   +0x120  el1_sysregs_t sysregs   (152 B)
 *   +0x1C0  u64           vttbr_el2
 *
 * guest_regs_t:  x[0..30] @ 0, sp_el0 @ 248, elr_el2 @ 256, spsr_el2 @ 264
 *   (total 272 = 0x110 bytes, padded to 0x120)
 */
typedef struct __aligned(16) vcpu {
    guest_regs_t    regs;               /* +0x000 */
    u8              _pad0[0x120 - sizeof(guest_regs_t)];
    el1_sysregs_t   sysregs;            /* +0x120 */
    u8              _pad1[0x1C0 - 0x120 - sizeof(el1_sysregs_t)];
    u64             vttbr_el2;          /* +0x1C0 */

    /* Metadata (not accessed from ASM) */
    u32             vcpu_id;
    u32             phys_cpu;
    vcpu_state_t    state;
    struct vm      *vm;
    paddr_t         reset_pc;
    u64             reset_x0;
} vcpu_t;

/*
 * g_current_vcpu[cpu_id] — pointer to the vcpu currently running
 * on each physical CPU.  The entry.S SAVE_GUEST_REGS macro reads
 * g_current_vcpu[0] to find where to store guest registers.
 * The scheduler updates this before returning from the IRQ handler,
 * so RESTORE_GUEST_REGS picks up the new vcpu automatically.
 */
extern vcpu_t *g_current_vcpu[MAX_PHYS_CPUS];

/* Assembly stubs */
extern void vcpu_save_sysregs   (el1_sysregs_t *p);
extern void vcpu_restore_sysregs(const el1_sysregs_t *p);

/* C API */
err_t vcpu_init    (vcpu_t *vc, struct vm *vm, u32 id);
void  vcpu_stop    (vcpu_t *vc);
void  vcpu_set_entry(vcpu_t *vc, paddr_t entry, u64 arg0);

/* Low-level context switch: save prev sysregs, load next sysregs + VTTBR.
 * GPRs are handled by entry.S macros using g_current_vcpu[0]. */
void  vcpu_do_switch(vcpu_t *prev, vcpu_t *next);

#endif /* HYP_VCPU_H */
