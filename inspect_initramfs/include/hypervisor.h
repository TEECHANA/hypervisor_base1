#ifndef HYP_HYPERVISOR_H
#define HYP_HYPERVISOR_H

#include "types.h"
#include "config.h"
#include "error.h"

struct vm;
struct vcpu;

typedef struct {
    u32         num_vms;
    struct vm  *vms[MAX_VMS];
    u32         num_cpus;
    u64         timer_freq_hz;
    bool        initialized;
} hypervisor_t;

extern hypervisor_t g_hyp;

/* Boot entries (called from assembly) */
void hyp_main(u32 cpu_id, paddr_t dtb_pa)  __noreturn;
void hyp_secondary_main(u32 cpu_id)         __noreturn;

/* Trap handlers */
void hyp_sync_handler(void *regs, u64 esr);
void hyp_irq_handler (void *regs);

/* Panic */
void hyp_panic(const char *msg)             __noreturn;

#endif
