/*
 * failover.c — VSE Phase 6: Backup OS Loading & Failover implementation
 *
 * Restarts a critical VM from a registered backup image when it is
 * quarantined by the trust engine. Uses the existing VM lifecycle
 * (vm_stop / vm_finalize / vcpu_set_entry / vm_start) so no new low-level
 * machinery is introduced.
 *
 * Failover sequence:
 *   1. Stop the failed VM            (vm_stop → unmaps S2, retires VMID)
 *   2. Re-finalize it                (vm_finalize → rebuilds S2, re-inits vCPU)
 *   3. Reset primary vCPU entry      (vcpu_set_entry → backup OS entry IPA)
 *   4. Start the VM                  (vm_start → state RUNNING)
 *
 * The scheduler then naturally resumes the VM on its next slot, now executing
 * the backup OS.
 */

#include "failover.h"
#include "trust.h"
#include "../core/vm/vm.h"
#include "../core/vcpu/vcpu.h"
#include "../include/hypervisor.h"
#include "../lib/log/log.h"
#include "../lib/str/string.h"

/* Per-VM backup descriptors, indexed by vm_id-1. */
static backup_desc_t _backup[MAX_VMS];
static bool          _initialized = false;

static inline s32 _idx(u32 vm_id)
{
    if (vm_id == 0u || vm_id > MAX_VMS) return -1;
    return (s32)(vm_id - 1u);
}

/* ── failover_init ── */
err_t failover_init(void)
{
    memset(_backup, 0, sizeof(_backup));
    _initialized = true;
    LOG_INFO("VSE Phase 6: failover service ready (max attempts=%u)",
             FAILOVER_MAX_ATTEMPTS);
    return E_OK;
}

/* ── failover_register ── */
err_t failover_register(u32 vm_id, u64 backup_pa, u64 live_pa,
                        u64 backup_size, u64 entry_ipa)
{
    s32 ix = _idx(vm_id);
    if (ix < 0) return E_INVAL;
    if (!_initialized) return E_INVAL;
    if (backup_pa == 0u || backup_size == 0u) return E_INVAL;

    vm_t *vm = vm_by_id(vm_id);
    if (!vm) return E_NOTFOUND;

    _backup[ix].registered     = true;
    _backup[ix].backup_pa      = backup_pa;
    _backup[ix].live_pa        = live_pa;
    _backup[ix].backup_size    = backup_size;
    _backup[ix].entry_ipa      = entry_ipa;
    _backup[ix].critical       = true;
    _backup[ix].failover_count = 0u;

    LOG_INFO("VSE Phase 6: VM%u '%s' registered for failover "
             "(backup PA=%lx live PA=%lx size=%lx entry=%lx)",
             vm_id, vm->name, backup_pa, live_pa, backup_size, entry_ipa);
    return E_OK;
}

/* ── Internal: perform the actual failover restart ── */
static err_t _do_failover(u32 vm_id, backup_desc_t *bd)
{
    vm_t *vm = vm_by_id(vm_id);
    if (!vm) return E_NOTFOUND;

    /* Budget check — give up after too many attempts. */
    if (bd->failover_count >= FAILOVER_MAX_ATTEMPTS) {
        LOG_ERROR("VSE Phase 6: VM%u exceeded %u failover attempts — REVOKING",
                  vm_id, FAILOVER_MAX_ATTEMPTS);
        trust_revoke(vm_id);
        return E_BUSY;
    }

    bd->failover_count++;
    LOG_ERROR("VSE Phase 6: FAILOVER VM%u '%s' attempt %u — "
              "restarting from backup OS",
              vm_id, vm->name, bd->failover_count);

    /*
     * Step 1: stop the failed VM. This unmaps its Stage-2 regions and
     * retires the VMID, clearing any corrupted guest state.
     */
    err_t e = vm_stop(vm);
    if (FAIL(e)) {
        LOG_ERROR("VSE Phase 6: vm_stop(VM%u) failed (err=%d)", vm_id, (int)e);
        return e;
    }

    /*
     * Step 2a: restore the guest image from the pristine backup copy.
     * Copy backup_size bytes from bd->backup_pa (a separate, untouched
     * backup region loaded at boot) into the VM's live load region. This
     * overwrites any corruption in the running image with known-good bytes
     * before the VM is restarted — this is what makes it a real backup-OS
     * restore rather than a same-image restart.
     */
    if (bd->backup_pa && bd->live_pa && bd->backup_size &&
        bd->backup_pa != bd->live_pa) {
        memcpy((void *)(uintptr_t)bd->live_pa,
               (const void *)(uintptr_t)bd->backup_pa,
               bd->backup_size);
        LOG_INFO("VSE Phase 6: VM%u restored %lu bytes from backup "
                 "%lx -> live %lx",
                 vm_id, bd->backup_size, bd->backup_pa, bd->live_pa);
    } else {
        LOG_WARN("VSE Phase 6: VM%u no distinct backup region — "
                 "restarting same image (no restore)", vm_id);
    }

    /*
     * Step 2b: re-finalize. Rebuilds the Stage-2 tables from the VM's
     * mem_region list and re-initializes the vCPU pool entry.
     */
    e = vm_finalize(vm);
    if (FAIL(e)) {
        LOG_ERROR("VSE Phase 6: vm_finalize(VM%u) failed (err=%d)",
                  vm_id, (int)e);
        return e;
    }

    /*
     * Step 3: reset the primary vCPU entry point to the backup OS entry.
     * arg0 = 0 (no DTB) for the backup; adjust if your backup needs one.
     */
    if (vm->num_vcpus > 0 && vm->vcpus[0]) {
        vcpu_set_entry(vm->vcpus[0], (paddr_t)bd->entry_ipa, 0u);
        LOG_INFO("VSE Phase 6: VM%u vCPU0 entry reset to %lx",
                 vm_id, bd->entry_ipa);
    }

    /*
     * Step 4: start the VM. The scheduler resumes it on its next slot,
     * now running the backup OS.
     */
    e = vm_start(vm);
    if (FAIL(e)) {
        LOG_ERROR("VSE Phase 6: vm_start(VM%u) failed (err=%d)", vm_id, (int)e);
        return e;
    }

    /*
     * Recovery succeeded — bring trust back to DEGRADED (not full TRUSTED;
     * the VM has a fault history) so it is watched but allowed to run.
     */
    trust_set(vm_id, TRUST_DEGRADED);

    LOG_INFO("VSE Phase 6: VM%u '%s' recovered via backup OS (attempt %u)",
             vm_id, vm->name, bd->failover_count);
    return E_OK;
}

/* ── failover_on_quarantine ── */
err_t failover_on_quarantine(u32 vm_id)
{
    s32 ix = _idx(vm_id);
    if (ix < 0) return E_INVAL;
    if (!_initialized) return E_NOTFOUND;

    backup_desc_t *bd = &_backup[ix];
    if (!bd->registered || !bd->critical) {
        /* No backup for this VM — leave it suspended (Phase 3 default). */
        return E_NOTFOUND;
    }

    LOG_WARN("VSE Phase 6: quarantine of critical VM%u — invoking failover",
             vm_id);
    return _do_failover(vm_id, bd);
}

/* ── failover_trigger (manual) ── */
err_t failover_trigger(u32 vm_id)
{
    s32 ix = _idx(vm_id);
    if (ix < 0) return E_INVAL;
    if (!_initialized) return E_INVAL;

    backup_desc_t *bd = &_backup[ix];
    if (!bd->registered) {
        LOG_WARN("VSE Phase 6: VM%u has no backup registered", vm_id);
        return E_NOTFOUND;
    }
    return _do_failover(vm_id, bd);
}

/* ── Query ── */
bool failover_is_registered(u32 vm_id)
{
    s32 ix = _idx(vm_id);
    if (ix < 0 || !_initialized) return false;
    return _backup[ix].registered;
}

u32 failover_attempts(u32 vm_id)
{
    s32 ix = _idx(vm_id);
    if (ix < 0 || !_initialized) return 0u;
    return _backup[ix].failover_count;
}

void failover_print_status(void)
{
    LOG_INFO("=== VSE Phase 6: Failover Status ===");
    for (u32 i = 0; i < g_hyp.num_vms; i++) {
        vm_t *vm = (vm_t *)g_hyp.vms[i];
        if (!vm) continue;
        s32 ix = _idx(vm->id);
        if (ix < 0) continue;
        backup_desc_t *bd = &_backup[ix];
        if (bd->registered)
            LOG_INFO("  VM%u %-8s backup=%lx entry=%lx attempts=%u",
                     vm->id, vm->name, bd->backup_pa, bd->entry_ipa,
                     bd->failover_count);
        else
            LOG_INFO("  VM%u %-8s (no backup registered)", vm->id, vm->name);
    }
    LOG_INFO("===================================");
}
