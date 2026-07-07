.global _start

_start:
    mov x0, #'H'
1:
    wfe
    b 1b
