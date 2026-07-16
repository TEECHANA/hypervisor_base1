/*
 * vm.c — VM lifecycle management (Phase 2)
 *
 * Phase 2 changes:
 *   vm_suspend() now calls power_gate_vm() to disable IRQs for the
 *     VM's devices while it is suspended.
 *   vm_resume()  now calls power_ungate_vm() to re-enable them.
 *
 * All other logic unchanged from Phase 1.
 */

#include "vm.h"
#include "../vcpu/vcpu.h"
#include "../../vre/mmu/stage2.h"
#include "../../vre/power/power.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"
#include "../../include/hypervisor.h"

/*
 * vm_state_str — human-readable state name (used in log output)
 */
static const char *_state_names[] = {
    "NONE","CREATED","READY","RUNNING","SUSPENDED","STOPPED"
};
const char *vm_state_str(vm_state_t s)
{
    if ((u32)s < 6) return _state_names[(u32)s];
    return "UNKNOWN";
}

/* Global VM table */
static vm_t _vms[MAX_VMS];
static u32  _nvm = 0;

extern err_t platform_init(void);

err_t vm_subsys_init(void)
{
    memset(_vms, 0, sizeof(_vms));
    _nvm = 0;
    LOG_INFO("VM subsystem init");

    err_t e = platform_init();
    if (FAIL(e)) {
        LOG_ERROR("platform_init failed (err=%d)", (int)e);
        return e;
    }
    return E_OK;
}

vm_t *vm_by_id(u32 id)
{
    for (u32 i = 0; i < _nvm; i++)
        if (_vms[i].id == id && _vms[i].state != VM_NONE)
            return &_vms[i];
    return NULL;
}

err_t vm_create(const char *name, vm_type_t type, vm_t **out)
{
    if (_nvm >= MAX_VMS) return E_NOMEM;

    vm_t *vm = &_vms[_nvm];
    memset(vm, 0, sizeof(*vm));

    vm->id    = _nvm + 1;
    vm->type  = type;
    vm->state = VM_CREATED;
    strncpy(vm->name, name, sizeof(vm->name) - 1);

    /* Default scheduler config */
    /* Slice durations — change these to control switching speed:
     * RTOS:           1000000us = 500ms
     * Linux/Android: 2000000us = 2s
     * For fast visible switching: RTOS=500ms, BE=2s
     */
    vm->sched.time_slice_us = (type == VM_RTOS) ? 500000 : 2000000;
    vm->sched.realtime      = (type == VM_RTOS);
    vm->sched.priority      = (type == VM_RTOS) ? 0 : 2;

    /* Create Stage-2 page tables */
    err_t e = s2_create(vm);
    if (FAIL(e)) return e;

    g_hyp.vms[_nvm] = vm;
    g_hyp.num_vms   = ++_nvm;

    LOG_INFO("VM %u '%s' created (type=%d)", vm->id, vm->name, (int)type);
    return (*out = vm, E_OK);
}

err_t vm_add_mem(vm_t *vm, paddr_t pa, ipa_t ipa, u64 sz, u32 flags)
{
    if (!vm || !sz) return E_INVAL;
    if (vm->num_mem >= MAX_MEM_REGIONS) return E_NOMEM;

    /*
     * Phase 1: overlap detection.
     * s2_create() must have been called first (vm->s2_pgd != 0).
     */
    if (vm->s2_pgd != 0 && s2_ipa_is_mapped(vm, ipa, sz)) {
        LOG_ERROR("VM %d: IPA range %lx+%lx already mapped — overlap rejected",
                  vm->id, ipa, sz);
        return E_BUSY;
    }

    vm->mem[vm->num_mem].pa_base  = pa;
    vm->mem[vm->num_mem].ipa_base = ipa;
    vm->mem[vm->num_mem].size     = sz;
    vm->mem[vm->num_mem].flags    = flags;
    vm->num_mem++;

    LOG_INFO("VM MAP ipa=%lx pa=%lx sz=%lx flags=%lx",
             ipa, pa, sz, (u64)flags);
    return E_OK;
}

err_t vm_add_dev(vm_t *vm, u64 mmio_base, u64 mmio_sz, u32 irq, bool pt)
{
    if (!vm) return E_INVAL;
    if (vm->num_dev >= MAX_DEV_PER_VM) return E_NOMEM;

    vm->dev[vm->num_dev].mmio_base = mmio_base;
    vm->dev[vm->num_dev].mmio_size = mmio_sz;
    vm->dev[vm->num_dev].irq       = irq;
    vm->dev[vm->num_dev].passthru  = pt;
    vm->num_dev++;
    return E_OK;
}

err_t vm_finalize(vm_t *vm)
{
    if (!vm) return E_INVAL;
    LOG_INFO("VM finalize start");

    /* Build Stage-2 page tables from mem_region list */
    for (u32 i = 0; i < vm->num_mem; i++) {
        mem_region_t *r = &vm->mem[i];

        u64 s2flags = S2_AF | S2_SH_INNER | S2_S2AP_RW;
        u64 mem_x   = (u64)(r->flags & MEM_X);

        LOG_INFO("MEM FLAGS raw=%lx MEM_X=%lx result=%d",
                 (u64)r->flags, mem_x, (int)(!mem_x));

        if (r->flags & MEM_IO)
            s2flags |= S2_MEMATTR_DEV;
        else
            s2flags |= S2_MEMATTR_WB;

        if (!(r->flags & MEM_X))
            s2flags |= S2_XN;

        err_t e = s2_map(vm, r->ipa_base, r->pa_base, r->size, s2flags);
        if (FAIL(e)) {
            LOG_ERROR("vm_finalize: s2_map failed for region %u", i);
            return e;
        }
    }

    /*
     * Linux: map one extra page at IPA 0x2BF000 to handle early kernel
     * fault before the full page tables are established.
     */
    if (vm->type == VM_LINUX) {
        paddr_t fault_pa = vm->mem[0].pa_base + 0x2BF000ULL;
        u64 s2f = S2_AF | S2_SH_INNER | S2_S2AP_RW | S2_MEMATTR_WB;
        s2_map(vm, 0x2BF000ULL, fault_pa, 0x1000ULL, s2f);
        LOG_INFO("Mapped Linux page IPA=0x2BF000");
    }

    /* Initialise vCPUs */
    extern err_t vcpu_init(struct vcpu *vc, vm_t *vm, u32 id);
    u32 nvc = (vm->type == VM_ANDROID) ? 2 : 1;
    vm->num_vcpus = 0;

    for (u32 v = 0; v < nvc; v++) {
        static struct vcpu _vcpu_pool[MAX_VMS * MAX_VCPU_PER_VM];
        static u32 _vcpu_idx = 0;
        if (_vcpu_idx >= MAX_VMS * MAX_VCPU_PER_VM) return E_NOMEM;

        struct vcpu *vc = &_vcpu_pool[_vcpu_idx++];
        err_t e = vcpu_init(vc, vm, v);
        if (FAIL(e)) return e;
        vm->vcpus[v] = vc;
        vm->num_vcpus++;
    }

    vm->state = VM_READY;
    LOG_INFO("VM %u '%s' finalized (%u vCPUs)", vm->id, vm->name, vm->num_vcpus);
    return E_OK;
}

err_t vm_start(vm_t *vm)
{
    if (!vm) return E_INVAL;
    if (vm->state != VM_READY) return E_INVAL;
    vm->state = VM_RUNNING;
    LOG_INFO("VM %u '%s' started", vm->id, vm->name);
    return E_OK;
}

/*
 * vm_stop — Phase 1: unmap all Stage-2 regions before TLB flush.
 *
 * Now we walk every mem_region the VM owns and call s2_unmap() so the
 * PTEs are explicitly cleared before the VMID is retired.
 * This prevents stale mappings if the VMID is ever reused.
 */
err_t vm_stop(vm_t *vm)
{
    if (!vm) return E_INVAL;
    if (vm->state == VM_STOPPED) return E_OK;

    for (u32 i = 0; i < vm->num_vcpus; i++)
        if (vm->vcpus[i]) vcpu_stop(vm->vcpus[i]);

    /* Unmap all Stage-2 regions */
    for (u32 i = 0; i < vm->num_mem; i++) {
        err_t e = s2_unmap(vm, vm->mem[i].ipa_base, vm->mem[i].size);
        if (FAIL(e))
            LOG_WARN("VM %d: s2_unmap region %d failed (err=%d)",
                     vm->id, i, (int)e);
    }

    s2_flush_tlb(vm);
    vm->state = VM_STOPPED;
    LOG_INFO("VM %u '%s' stopped", vm->id, vm->name);
    return E_OK;
}

/*
 * vm_suspend — Phase 2: gate device clocks/IRQs while suspended.
 */
err_t vm_suspend(vm_t *vm)
{
    if (!vm) return E_INVAL;
    if (vm->state != VM_RUNNING) return E_INVAL;

    /* Phase 2: disable all device IRQs for this VM */
    err_t e = power_gate_vm(vm);
    if (FAIL(e))
        LOG_WARN("VM %d: power_gate_vm failed (err=%d)", vm->id, (int)e);

    vm->state = VM_SUSPENDED;
    LOG_INFO("VM %u '%s' suspended", vm->id, vm->name);
    return E_OK;
}

/*
 * vm_resume — Phase 2: restore device clocks/IRQs on resume.
 */
err_t vm_resume(vm_t *vm)
{
    if (!vm) return E_INVAL;
    if (vm->state != VM_SUSPENDED) return E_INVAL;

    /* Phase 2: re-enable device IRQs */
    err_t e = power_ungate_vm(vm);
    if (FAIL(e))
        LOG_WARN("VM %d: power_ungate_vm failed (err=%d)", vm->id, (int)e);

    vm->state = VM_RUNNING;
    LOG_INFO("VM %u '%s' resumed", vm->id, vm->name);
    return E_OK;
}
