OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH("aarch64")
ENTRY(_hyp_start)

SECTIONS
{
    . = 0x40000000;

    /* ── Entry ───────────────────────────── */
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

    /* ── Read-only data ──────────────────── */
    . = ALIGN(64);
    .rodata :
    {
        *(.rodata*)
    }

    /* ── Data ───────────────────────────── */
    . = ALIGN(8);
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
