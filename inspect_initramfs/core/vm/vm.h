#ifndef HYP_VM_H
#define HYP_VM_H
#include "../../include/types.h"
#include "../../include/error.h"
#include "../../include/config.h"

typedef enum {
    VM_NONE=0, VM_CREATED, VM_READY, VM_RUNNING, VM_SUSPENDED, VM_STOPPED
} vm_state_t;

typedef enum { VM_LINUX=0, VM_RTOS=1, VM_ANDROID=2 } vm_type_t;

typedef struct {
    paddr_t pa_base; ipa_t ipa_base; u64 size; u32 flags;
} mem_region_t;
#define MEM_R  BIT(0)
#define MEM_W  BIT(1)
#define MEM_X  BIT(2)
#define MEM_IO BIT(3)

typedef struct {
    u64 mmio_base; u64 mmio_size; u32 irq; bool passthru;
} dev_assign_t;

typedef struct {
    u32  time_slice_us;
    u32  priority;       /* 0=highest (RTOS), 3=lowest */
    bool realtime;
} sched_cfg_t;

struct vcpu;

typedef struct vm {
    u32         id;
    char        name[32];
    vm_state_t  state;
    vm_type_t   type;

    u32         num_vcpus;
    struct vcpu *vcpus[MAX_VCPU_PER_VM];

    u32         num_mem;
    mem_region_t mem[MAX_MEM_REGIONS];

    u32         num_dev;
    dev_assign_t dev[MAX_DEV_PER_VM];

    paddr_t     s2_pgd;     /* Stage-2 PGD physical address    */
    u64         vttbr;      /* VTTBR_EL2 value (VMID | pgd)   */

    ipa_t       entry_ipa;  /* Kernel entry physical address   */
    u64         dtb_ipa;    /* DTB guest-physical (IPA)        */

    sched_cfg_t sched;
} vm_t;

err_t  vm_subsys_init(void);
err_t  vm_create(const char *name, vm_type_t type, vm_t **out);
err_t  vm_add_mem(vm_t *vm, paddr_t pa, ipa_t ipa, u64 sz, u32 flags);
err_t  vm_add_dev(vm_t *vm, u64 mmio_base, u64 mmio_sz, u32 irq, bool pt);
err_t  vm_finalize(vm_t *vm);   /* build S2 tables, init vCPUs */
err_t  vm_start(vm_t *vm);
err_t  vm_stop(vm_t *vm);
err_t  vm_suspend(vm_t *vm);
err_t  vm_resume(vm_t *vm);
vm_t  *vm_by_id(u32 id);
const char *vm_state_str(vm_state_t s);
#endif
