.section ".text.start","ax"
.global _rtos_start

_rtos_start:
    /* stack grows down from 0x00008000 */
    ldr x10, =0x00008000
    mov sp, x10
    
    adr x10, __bss_start
    adr x11, __bss_end
    
.Lz: 
    cmp x10,x11; b.ge .Ld; str xzr,[x10],#8; b .Lz
    
.Ld: 
    bl rtos_main
    
.Lh: 
    wfi; b .Lh
