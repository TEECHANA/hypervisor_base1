#ifndef HYP_UART_H
#define HYP_UART_H
#include "../../include/types.h"
#include "../../include/error.h"
err_t uart_init(u64 base);
void  uart_putc(char c);
void  uart_puts(const char *s);
void  uart_puthex(u64 v);
void  uart_putdec(s64 v);
#endif
