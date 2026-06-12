/*
 * mmu.S — Minimal EL2 Stage-1 identity-map bootstrap
 */

.section .text.mmu, "ax"
.global mmu_el2_init

mmu_el2_init:

    /* MAIR_EL2: attr0=0xFF Normal WB-WA, attr1=0x00 Device */
    /*
    mov  x0, #0xFF
    msr  mair_el2, x0 */
    /*
 * Attr0 = 0xFF -> Normal WB WA Cacheable
 * Attr1 = 0x00 -> Device nGnRnE
 */

    ldr x0, =0x00000000000000FF
    msr mair_el2, x0

    /* TCR_EL2 */
    ldr  x0, =0x80823520
    msr  tcr_el2, x0
    isb

    adr  x1, __el2_l1_table

    /* Entry 0: Device memory */
    //ldr  x2, =0x0000000000000401
    /*
     * 1GB block
     * Device memory
     */

    ldr x2, =0x0000000000000401
    orr x2, x2, #(1 << 10)     /* AF */
    str  x2, [x1, #0]

    /* Entry 1: Normal memory */
    //ldr  x2, =0x0000000040000701
        /*
     * 1GB block
     * Normal WB memory
     */

    ldr x2, =0x0000000040000701
    orr x2, x2, #(1 << 10)     /* AF */
    str  x2, [x1, #8]
        /* Entry 2 : 0x80000000 */
    ldr x2, =0x80000000
    mov x3, #0x701
    orr x2, x2, x3
    orr x2, x2, #(1 << 10)
    str x2, [x1, #16]

    ldr x2, =0xC0000000
    mov x3, #0x701
    orr x2, x2, x3
    orr x2, x2, #(1 << 10)
    str x2, [x1, #24]

    msr  ttbr0_el2, x1
    isb

    /* Enable MMU */
    mrs  x0, sctlr_el2
    
    mov w0, #'M'
    strb w0, [x20]

    /*
 * SCTLR_EL2
 * M = MMU enable
 * C = Data cache enable
 * I = Instruction cache enable
 */

    ldr x1, =0x100D
    orr  x0, x0, x1

    msr  sctlr_el2, x0
    isb

    ret

.section .bss.ptables, "aw"
.align 12
__el2_l1_table:
    .space 4096
