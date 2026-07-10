.global _start
_start:
    /* write(1, msg, 16) — fd 1 pre-opened by kernel to /dev/console */
    mov x0, #1
    adr x1, msg
    mov x2, #16
    mov x8, #64
    svc #0

    /* write(1, msg2, 16) */
    mov x0, #1
    adr x1, msg2
    mov x2, #16
    mov x8, #64
    svc #0

    /* pause() — keep init alive, don't exit */
1:  mov x8, #34
    svc #0
    b 1b

msg:
    .ascii "\r\n[ASM_INIT_OK]\r\n"
msg2:
    .ascii "[FD1_WORKS!]\r\n\r\n"
