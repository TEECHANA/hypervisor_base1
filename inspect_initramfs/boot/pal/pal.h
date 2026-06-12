#ifndef HYP_PAL_H
#define HYP_PAL_H
#include "../../include/types.h"
#include "../../include/error.h"
typedef enum { PLAT_QEMU=0, PLAT_RPI4=1, PLAT_S32G=2 } plat_id_t;
typedef struct {
    plat_id_t   id;
    const char *name;
    u64  uart_base, gicd_base, gicr_base;
    u64  ram_base,  ram_size;
    u64  timer_freq;
} plat_t;
extern plat_t g_plat;
err_t pal_init(paddr_t dtb_pa);
void  pal_delay_us(u32 us);
#endif
