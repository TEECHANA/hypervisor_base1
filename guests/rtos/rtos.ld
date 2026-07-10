OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_rtos_start)

/* Link image at guest IPA 0x00008000 */
SECTIONS {
    . = 0x00008000;
    .text.start : { KEEP(*(.text.start)) }
    .text : { *(.text) }
    .rodata : { *(.rodata) }
    .data : { *(.data) }
    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.bss) *(COMMON) }
    __bss_end = .;
}
