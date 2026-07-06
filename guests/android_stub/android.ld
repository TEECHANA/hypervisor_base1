OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_android_start)

/* Linked at the guest IPA the hypervisor enters at: ANDROID_ENTRY_IPA in
 * boot/platform_init.c = 0x00200000 (image loaded to PA 0x70200000, Android
 * RAM IPA 0 -> PA 0x70000000). Linking here keeps absolute addresses (string
 * pointers, the stack literal) valid at run time. */
SECTIONS {
    . = 0x00200000;
    .text.start : { KEEP(*(.text.start)) }
    .text       : { *(.text*) }
    .rodata     : { *(.rodata*) }
    .data       : { *(.data*) }
    . = ALIGN(16);
    __bss_start = .;
    .bss        : { *(.bss*) *(COMMON) }
    __bss_end   = .;
}
