    .section ".text.start","ax"
    .global _android_start
_android_start:
    /* Stack grows down from the image base (IPA 0x00200000) into the mapped
     * RAM below it — within Android's IPA window [0, 0x20000000). The old
     * 0x48FF0000 was outside that window and faulted on any real stack use. */
    ldr  x10, =0x00200000
    mov  sp, x10
    adr  x10, __bss_start
    adr  x11, __bss_end
.Lz: cmp x10,x11; b.ge .Ld; str xzr,[x10],#8; b .Lz
.Ld:
    bl   android_main
.Lh: wfi; b .Lh

    .section ".bss","aw"
    .global __bss_start; .global __bss_end
__bss_start: .space 0
__bss_end:   .space 0
