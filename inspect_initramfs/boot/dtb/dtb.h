#ifndef HYP_DTB_H
#define HYP_DTB_H
#include "../../include/types.h"
#include "../../include/error.h"

/* Minimal DTB for Linux guest */
err_t dtb_build_linux(void *buf, u64 buf_sz, u64 ram_base, u64 ram_size,
                      u64 uart_base, u32 uart_irq, u64 *out_sz);
#endif
