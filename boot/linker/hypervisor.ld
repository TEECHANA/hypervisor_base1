OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH("aarch64")
ENTRY(_hyp_start)

SECTIONS
{
    . = 0x40000000;

    /* ── Entry ───────────────────────────── */
    __text_start = .;
    .text.entry :
    {
        KEEP(*(.text.entry))
    }

    /* ── Vectors (2KB aligned) ───────────── */
    . = ALIGN(0x800);
    .text.vectors :
    {
        KEEP(*(.text.vectors))
    }

    /* ── MMU ─────────────────────────────── */
    .text.mmu :
    {
        *(.text.mmu)
    }

    /* ── General text ────────────────────── */
    .text :
    {
        *(.text*)
    }
    __text_end = .;

    /* ── Read-only data ──────────────────── */
    /* 2 MB alignment gives .rodata its own L2 slot so mmu.S can set
     * AP[2]=1 (read-only) on exactly [__rodata_start, __rodata_end)
     * without splitting a shared 2 MB block. */
    . = ALIGN(0x200000);
    __rodata_start = .;
    .rodata :
    {
        *(.rodata*)
    }
    __rodata_end = .;

    ASSERT((__rodata_start & 0x1FFFFF) == 0,
           ".rodata must be 2 MB aligned — mmu.S assumes slot boundary")
    ASSERT(__rodata_end <= (__rodata_start + 0x200000),
           ".rodata overflows its 2 MB MMU slot")

    /* ── Data ───────────────────────────── */
    /* 2 MB alignment keeps .data/.bss in separate L2 slots from .rodata,
     * so the R/W mapping does not bleed back into the read-only range. */
    . = ALIGN(0x200000);
    .data :
    {
        *(.data*)
    }

    /* ── Page tables ────────────────────── */
    . = ALIGN(4096);
    .bss.ptables :
    {
        *(.bss.ptables)
    }

    /* ── Stacks ─────────────────────────── */
    . = ALIGN(16);
    .bss.stacks :
    {
        *(.bss.stacks)
    }

    /* ── BSS ────────────────────────────── */
    . = ALIGN(8);
    __bss_start = .;

    .bss :
    {
        *(.bss*)
        *(COMMON)
    }

    __bss_end = .;

    /* ── End marker ─────────────────────── */
    . = ALIGN(4096);
    __hyp_end = .;

    ASSERT(__hyp_end < 0x80000000,
       "Hypervisor overlaps guest memory")
    /* ── Discard ────────────────────────── */
    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
