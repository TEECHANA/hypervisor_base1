/*
 * context.S
 *
 * EL1 system-register save/restore for guest context switching.
 *
 * GPRs are saved/restored separately in entry.S.
 *
 * This file handles:
 *   - MMU registers
 *   - Translation registers
 *   - EL1 control state
 *   - Thread-local registers
 *   - EL1 exception configuration
 *
 * IMPORTANT:
 *   SCTLR_EL1 must be restored LAST because enabling
 *   MMU/caches before TTBR/TCR/MAIR are valid can
 *   immediately trigger synchronous aborts.
 */

.section ".text", "ax"

/* =========================================================
 * void vcpu_save_sysregs(el1_sysregs_t *p)
 * x0 = pointer to save area
 * =========================================================
 */

.global vcpu_save_sysregs
.type vcpu_save_sysregs, %function

vcpu_save_sysregs:

    /* MMU + translation state */
    mrs x1, sctlr_el1
    str x1, [x0, #0x00]

    mrs x1, ttbr0_el1
    str x1, [x0, #0x08]

    mrs x1, ttbr1_el1
    str x1, [x0, #0x10]

    mrs x1, tcr_el1
    str x1, [x0, #0x18]

    mrs x1, mair_el1
    str x1, [x0, #0x20]

    mrs x1, amair_el1
    str x1, [x0, #0x28]

    /* Exception/vector state */
    mrs x1, vbar_el1
    str x1, [x0, #0x30]

    mrs x1, sp_el1
    str x1, [x0, #0x38]

    mrs x1, elr_el1
    str x1, [x0, #0x40]

    mrs x1, spsr_el1
    str x1, [x0, #0x48]

    /* Fault/debug state */
    mrs x1, esr_el1
    str x1, [x0, #0x50]

    mrs x1, far_el1
    str x1, [x0, #0x58]

    /* Thread-local storage */
    mrs x1, tpidr_el0
    str x1, [x0, #0x60]

    mrs x1, tpidr_el1
    str x1, [x0, #0x68]

    mrs x1, tpidrro_el0
    str x1, [x0, #0x70]

    /* FP/SIMD access control */
    mrs x1, cpacr_el1
    str x1, [x0, #0x78]

    /* Timer access */
    mrs x1, cntkctl_el1
    str x1, [x0, #0x80]

    /* Context ID */
    mrs x1, contextidr_el1
    str x1, [x0, #0x88]

    isb
    ret


/* =========================================================
 * void vcpu_restore_sysregs(const el1_sysregs_t *p)
 * x0 = pointer to restore area
 * =========================================================
 */

.global vcpu_restore_sysregs
.type vcpu_restore_sysregs, %function

vcpu_restore_sysregs:

    /*
     * Restore translation attributes FIRST
     */

    ldr x1, [x0, #0x20]
    msr mair_el1, x1

    ldr x1, [x0, #0x28]
    msr amair_el1, x1

    ldr x1, [x0, #0x18]
    msr tcr_el1, x1

    ldr x1, [x0, #0x08]
    msr ttbr0_el1, x1

    ldr x1, [x0, #0x10]
    msr ttbr1_el1, x1

    /*
     * Synchronize translation changes
     */

    dsb ish
    isb

    /*
     * Restore exception state
     */

    ldr x1, [x0, #0x30]
    msr vbar_el1, x1

    ldr x1, [x0, #0x38]
    msr sp_el1, x1

    ldr x1, [x0, #0x40]
    msr elr_el1, x1

    ldr x1, [x0, #0x48]
    msr spsr_el1, x1

    /*
     * Restore TLS registers
     */

    ldr x1, [x0, #0x60]
    msr tpidr_el0, x1

    ldr x1, [x0, #0x68]
    msr tpidr_el1, x1

    ldr x1, [x0, #0x70]
    msr tpidrro_el0, x1

    /*
     * Restore FP/SIMD access
     */

    ldr x1, [x0, #0x78]
    msr cpacr_el1, x1

    /*
     * Restore timer control
     */

    ldr x1, [x0, #0x80]
    msr cntkctl_el1, x1

    /*
     * Restore context ID
     */

    ldr x1, [x0, #0x88]
    msr contextidr_el1, x1

    /*
     * IMPORTANT:
     * Restore SCTLR_EL1 LAST
     */

    ldr x1, [x0, #0x00]
    msr sctlr_el1, x1

    isb
    ret
