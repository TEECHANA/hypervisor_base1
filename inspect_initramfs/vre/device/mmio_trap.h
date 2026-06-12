#ifndef HYP_MMIO_TRAP_H
#define HYP_MMIO_TRAP_H
#include "../../include/types.h"
#include "../../include/error.h"
typedef err_t (*mmio_fn)(u64 addr, bool is_write, u64 *val, void *priv);
err_t mmio_register(u64 base, u64 sz, mmio_fn fn, void *priv);
void  mmio_handle  (u64 fault_addr, u64 esr, void *regs);
#endif
