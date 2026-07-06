/*
 * entry.S — EL2 hypervisor entry + exception vector table
 */
#define HCR_COMMON \
    (1UL << 0)  |   /* VM */ \
    (1UL << 31) |   /* RW */ \
    (1UL << 4)  |   /* IMO */ \
    (1UL << 3)  |   /* FMO */ \
    (1UL << 5)      /* AMO */
.section .text.entry, "ax"
.align 2
.global _hyp_start
.type _hyp_start, %function

_hyp_start:

    msr daifset, #0xF

    ldr x20, =0x09000000

    mov w0, #'A'
    strb w0, [x20]

    mrs x10, CurrentEL
    lsr x10, x10, #2

    mov w0, #'B'
    strb w0, [x20]

    cmp x10, #2
    b.ne _spin_forever

    mov w0, #'C'
    strb w0, [x20]

    adrp x10, __primary_stack_top
    add  x10, x10, :lo12:__primary_stack_top
    mov  sp, x10

    mov w0, #'D'
    strb w0, [x20]
    adrp x10, _hyp_vectors
    add  x10, x10, :lo12:_hyp_vectors
    msr  vbar_el2, x10
    isb

    /* Initialize HCR_EL2 */
    ldr x10, =HCR_COMMON
    msr hcr_el2, x10
    isb
    
    bl mmu_el2_init
    mov w0, #'H'
    strb w0, [x20]
    bl hyp_main
    mov w0, #'R'
    strb w0, [x20]
    
    mov w0, #'E'
    strb w0, [x20]
_spin_forever:
    wfe
    b _spin_forever
/* ───────────────── SECONDARY CPU ENTRY ───────────────── */

.global _hyp_secondary_start
_hyp_secondary_start:
    msr  daifset, #0xF

    /* Stack setup per CPU */
    adrp x10, __secondary_stacks_top
    add  x10, x10, :lo12:__secondary_stacks_top

    and  x11, x0, #0xFF
    lsl  x11, x11, #13
    sub  sp, x10, x11

    /* Set vectors */
    adrp x10, _hyp_vectors
    add  x10, x10, :lo12:_hyp_vectors
    msr  vbar_el2, x10
    isb
    mov w0, #'C'
    str w0, [x20]
    /* HCR_EL2 */
    ldr x10, =HCR_COMMON
    msr hcr_el2, x10
    isb


    bl hyp_secondary_main


1:
    wfe
    b 1b


/* ───────────────── VECTOR TABLE ───────────────── */

.section .text.vectors, "ax"
.align 11
.global _hyp_vectors

_hyp_vectors:

    .align 7
    b _el2_sp0_sync
    nop

    .align 7
    b _el2_sp0_irq
    nop

    .align 7
    b _el2_sp0_fiq
    nop

    .align 7
    b _el2_sp0_serr
    nop

    .align 7
    b _el2_spx_sync
    nop

    .align 7
    b _el2_spx_irq
    nop

    .align 7
    b _el2_spx_fiq
    nop

    .align 7
    b _el2_spx_serr
    nop

    .align 7
    b _guest64_sync
    nop

    .align 7
    b _guest64_irq
    nop

    .align 7
    b _guest64_fiq
    nop

    .align 7
    b _guest64_serr
    nop

    .align 7
    b _guest32_sync
    nop

    .align 7
    b _guest32_irq
    nop

    .align 7
    b _guest32_fiq
    nop

    .align 7
    b _guest32_serr
    nop

/* -------------------------------------------------
 * Save guest GPRs into current vcpu->regs
 * ------------------------------------------------- */

.macro SAVE_GUEST_REGS

    /* Preserve guest x16/x17 first */
    sub sp, sp, #16
    stp x16, x17, [sp]

    /* Load current vcpu pointer */
    adrp x16, g_current_vcpu
    add  x16, x16, :lo12:g_current_vcpu
    ldr  x16, [x16]

    cbz  x16, _spin_forever

    /* Save x0-x15 */
    stp x0,  x1,  [x16, #(0*16)]
    stp x2,  x3,  [x16, #(1*16)]
    stp x4,  x5,  [x16, #(2*16)]
    stp x6,  x7,  [x16, #(3*16)]
    stp x8,  x9,  [x16, #(4*16)]
    stp x10, x11, [x16, #(5*16)]
    stp x12, x13, [x16, #(6*16)]
    stp x14, x15, [x16, #(7*16)]

    /* Save ORIGINAL guest x16/x17 */
    ldp x14, x15, [sp]
    stp x14, x15, [x16, #(8*16)]
    add sp, sp, #16

    /* Save remaining regs */
    stp x18, x19, [x16, #(9*16)]
    stp x20, x21, [x16, #(10*16)]
    stp x22, x23, [x16, #(11*16)]
    stp x24, x25, [x16, #(12*16)]
    stp x26, x27, [x16, #(13*16)]
    stp x28, x29, [x16, #(14*16)]

    str x30, [x16, #(15*16)]

    /* Save guest SP_EL1 */
    mrs x17, sp_el1
    str x17, [x16, #(15*16 + 8)]

    /* Save ELR_EL2 */
    mrs x17, elr_el2
    str x17, [x16, #(16*16)]

    /* Save SPSR_EL2 */
    mrs x17, spsr_el2
    str x17, [x16, #(16*16 + 8)]

.endm


.macro RESTORE_GUEST_REGS
    adrp x16, g_current_vcpu
    add  x16, x16, :lo12:g_current_vcpu
    ldr  x16, [x16]
    cbz  x16, _spin_forever

    /* Restore GPRs x0-x15 first */
    ldp x0,  x1,  [x16, #(0*16)]
    ldp x2,  x3,  [x16, #(1*16)]
    ldp x4,  x5,  [x16, #(2*16)]
    ldp x6,  x7,  [x16, #(3*16)]
    ldp x8,  x9,  [x16, #(4*16)]
    ldp x10, x11, [x16, #(5*16)]
    ldp x12, x13, [x16, #(6*16)]
    ldp x14, x15, [x16, #(7*16)]

    /* Restore x18-x30 */
    ldp x18, x19, [x16, #(9*16)]
    ldp x20, x21, [x16, #(10*16)]
    ldp x22, x23, [x16, #(11*16)]
    ldp x24, x25, [x16, #(12*16)]
    ldp x26, x27, [x16, #(13*16)]
    ldp x28, x29, [x16, #(14*16)]
    ldr x30,      [x16, #(15*16)]

    /* Restore ELR, SPSR, SP using x17 as scratch (x16 still = vcpu ptr) */
    ldr  x17, [x16, #(16*16)]
    msr  elr_el2, x17
    ldr  x17, [x16, #(16*16 + 8)]
    msr  spsr_el2, x17
    ldr  x17, [x16, #(15*16 + 8)]
    msr  sp_el1, x17

    /* Restore x16/x17 last — vcpu ptr gone after this */
    ldr x17, [x16, #(8*16 + 8)]
    ldr x16, [x16, #(8*16)]
.endm
/* ───────────────── HANDLERS ───────────────── */

_guest64_sync:
    /* Save guest GPRs FIRST — must not touch x1 before it is saved, or the
     * guest's x1 (HVC arg) is lost. hvc_dispatch reads args from the saved
     * register array, so x[1] must hold the guest value, not ESR. */
    SAVE_GUEST_REGS
    /* Now read ESR for the C handler (guest x1 already safely saved). */
    mrs x1, esr_el2
    adrp x0, g_current_vcpu
    add  x0, x0, :lo12:g_current_vcpu
    ldr  x0, [x0]
    cbz  x0, _spin_forever
    add  x0, x0, #0
    bl hyp_sync_handler
    RESTORE_GUEST_REGS
    eret

_guest64_irq:

    SAVE_GUEST_REGS

    bl hyp_irq_handler

    RESTORE_GUEST_REGS

    eret

_el2_sp0_sync:
_el2_spx_sync:
    mrs x0, esr_el2
    mrs x1, elr_el2
    bl _hyp_internal_fault

_el2_sp0_irq:
_el2_spx_irq:
_el2_sp0_fiq:
_el2_spx_fiq:
_el2_sp0_serr:
_el2_spx_serr:
_guest64_fiq:
_guest64_serr:
_guest32_sync:
_guest32_irq:
_guest32_fiq:
_guest32_serr:
    b _spin_forever


.global _hyp_internal_fault
_hyp_internal_fault:
    adrp x2, panic_msg
    add  x2, x2, :lo12:panic_msg
    bl hyp_panic


/* ───────────────── RODATA ───────────────── */

.section .rodata, "a"
.align 3

panic_msg:
    .asciz "EL2 internal fault"


/* ───────────────── STACKS ───────────────── */

.section .bss.stacks, "aw"
.align 12

__primary_stack_base:
    .space 0x4000
__primary_stack_top:

__secondary_stacks_base:
    .space (0x2000 * 3)
__secondary_stacks_top:


/* ───────────────── BSS ───────────────── */

