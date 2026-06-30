/*
 * mmu.S — EL2 Stage-1 identity-map bootstrap with section-level protection
 *
 * L1 table covers 4 GB (4 × 1 GB slots):
 *   [0] 0x00000000–0x3FFFFFFF  Device nGnRnE          (R/W)
 *   [1] 0x40000000–0x7FFFFFFF  → L2 table (see below)
 *   [2] 0x80000000–0xBFFFFFFF  Normal WB               (R/W NX)
 *   [3] 0xC0000000–0xFFFFFFFF  Normal WB               (R/W NX)
 *
 * L2 table covers 0x40000000–0x7FFFFFFF (512 × 2 MB slots):
 *   [0] 0x40000000–0x401FFFFF  .text   AP[2]=1 XN=0    (R only, executable)
 *   [1] 0x40200000–0x403FFFFF  .rodata AP[2]=1 XN=1    (R only, NOT executable)
 *   [2–511] 0x40400000–…       R/W NX                  (data, BSS, guest phys)
 *
 * Descriptor bit summary (Normal WB inner-shareable, AF pre-set, block):
 *   base (R/W NX):  0x701 = AttrIndx=0, AP[2:1]=00, SH=11, AF=1, block
 *   R-only:         base | (1<<7)  →  0x781
 *   XN:             base | (1<<54) →  0x0040_0000_0000_0000
 *
 * Linker script (hypervisor.ld) guarantees:
 *   __rodata_start = 0x40200000 (2 MB aligned, asserted at link time)
 *   __data_start  >= 0x40400000 (2 MB aligned)
 */

.section .text.mmu, "ax"
.global mmu_el2_init

mmu_el2_init:

    /* MAIR_EL2: attr0 = Normal WB WA, attr1 = Device nGnRnE */
    ldr  x0, =0x00000000000000FF
    msr  mair_el2, x0

    /* TCR_EL2 */
    ldr  x0, =0x80823520
    msr  tcr_el2, x0
    isb

    ldr  x1, =__el2_l1_table          /* x1 = L1 table PA */
    ldr  x2, =__el2_l2_text_rodata   /* x2 = L2 table PA */

    /* ── L1[0]: Device memory 0x00000000–0x3FFFFFFF ── */
    ldr  x3, =0x0000000000000401
    orr  x3, x3, #(1 << 10)          /* AF (already in 0x401 but harmless) */
    str  x3, [x1, #0]

    /* ── L1[1]: 0x40000000–0x7FFFFFFF → L2 table ── */
    orr  x3, x2, #3                   /* table descriptor: bits[1:0]=11 */
    str  x3, [x1, #8]

    /* ── L1[2]: Normal R/W 0x80000000–0xBFFFFFFF ── */
    ldr  x3, =0x0000000080000701
    orr  x3, x3, #(1 << 10)
    str  x3, [x1, #16]

    /* ── L1[3]: Normal R/W 0xC0000000–0xFFFFFFFF ── */
    ldr  x3, =0x00000000C0000701
    orr  x3, x3, #(1 << 10)
    str  x3, [x1, #24]

    /* ── L2[0]: .text — Read-only, Executable ──────────────────────────
     * PA=0x40000000, AP[2]=1 (bit 7), XN=0
     * descriptor = 0x40000000 | 0x781
     *            = 0x0000000040000781
     */
    ldr  x3, =0x0000000040000781
    str  x3, [x2, #0]

    /* ── L2[1]: .rodata — Read-only, NOT executable ─────────────────────
     * PA=0x40200000, AP[2]=1 (bit 7), XN=1 (bit 54)
     * descriptor = 0x40200000 | 0x781 | (1<<54)
     *            = 0x0040000040200781
     */
    ldr  x3, =0x0040000040200781
    str  x3, [x2, #8]

    /* ── L2[2..511]: R/W NX — data, BSS, page tables, guest phys ────────
     * Start PA=0x40400000, increment 2 MB per slot.
     * descriptor = PA | 0x701 | (1<<54)
     * slot 2 base = 0x0040000040400701
     */
    ldr  x3, =0x0040000040400701   /* slot 2 descriptor */
    ldr  x4, =0x0000000000200000   /* 2 MB PA step */
    mov  x5, #510                   /* remaining 510 slots (2..511) */
    add  x6, x2, #16               /* write pointer: &L2[2] */

.L_l2_fill:
    str  x3, [x6], #8
    add  x3, x3, x4
    subs x5, x5, #1
    b.ne .L_l2_fill

    msr  ttbr0_el2, x1
    isb

    /* Enable MMU: M | C | I | SA */
    mrs  x0, sctlr_el2

    mov  w0, #'M'
    strb w0, [x20]

    ldr  x1, =0x100D
    orr  x0, x0, x1

    msr  sctlr_el2, x0
    isb

    ret

/* ── Page tables (BSS, 4 KB each, 4 KB aligned) ── */
.section .bss.ptables, "aw"
.align 12
__el2_l1_table:
    .space 4096

.align 12
__el2_l2_text_rodata:
    .space 4096
