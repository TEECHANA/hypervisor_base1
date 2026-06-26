/*
 * failover.h — VSE Phase 6: Backup OS Loading & Failover
 *
 * The final VSE phase. It closes the loop opened by Phases 3 and 5:
 *   - Phase 5 detects guest faults.
 *   - Phase 3 escalates trust and QUARANTINES a misbehaving VM (suspends it).
 *   - Phase 6 RECOVERS: when a VM marked critical is quarantined or revoked,
 *     the hypervisor restarts it from a registered backup OS image instead of
 *     leaving the workload dead.
 *
 * "Failover" here means: reset the VM's vCPU entry point to a backup image
 * that was pre-loaded into the VM's memory region, then bring the VM back to
 * RUNNING. The guest effectively reboots into a known-good fallback OS.
 *
 * What this MVP does:
 *   - Lets you register a backup image (PA + size + entry IPA) per VM.
 *   - On failover trigger, resets the VM's primary vCPU to the backup entry
 *     and restarts the VM via the existing lifecycle calls.
 *   - Is driven by Phase 3: trust_quarantine() calls failover_on_quarantine().
 *
 * What a production version would add (documented, not implemented here):
 *   - Actually copying the backup image bytes from backup_pa into the VM's
 *     RAM region (this MVP assumes the image is already resident at the VM's
 *     load address, as your QEMU -device loader does at boot).
 *   - A retry/budget policy so a VM that keeps failing is eventually REVOKED
 *     for good instead of looping.
 */

#ifndef VSE_FAILOVER_H
#define VSE_FAILOVER_H

#include "../include/types.h"
#include "../include/error.h"
#include "../include/config.h"

/* Per-VM backup image descriptor. */
typedef struct {
    bool  registered;
    u64   backup_pa;     /* PA of the PRISTINE backup copy (separate region) */
    u64   live_pa;       /* PA of the VM's live load region (restore target) */
    u64   backup_size;   /* size of the backup image in bytes */
    u64   entry_ipa;     /* guest-physical entry point to reset vCPU to */
    bool  critical;      /* only critical VMs trigger failover */
    u32   failover_count;/* how many times this VM has failed over */
} backup_desc_t;

/* Max failovers before a VM is given up on (REVOKED instead of restarted). */
#ifndef FAILOVER_MAX_ATTEMPTS
#define FAILOVER_MAX_ATTEMPTS  3u
#endif

/* ── Lifecycle ── */
err_t failover_init(void);

/*
 * failover_register — mark a VM as critical and give it a backup image.
 *   vm_id      : which VM
 *   backup_pa  : PA of the backup image (already resident or loadable)
 *   backup_size: size in bytes
 *   entry_ipa  : guest-physical entry the vCPU resets to on failover
 *
 * Call at boot after vm_subsys_init(), for VMs you want protected.
 */
err_t failover_register(u32 vm_id, u64 backup_pa, u64 live_pa,
                        u64 backup_size, u64 entry_ipa);

/* ── Triggers ── */
/*
 * failover_on_quarantine — called by trust_quarantine() when a VM is
 * quarantined. If the VM is registered + critical, performs failover;
 * otherwise leaves the VM suspended (default Phase-3 behaviour).
 * Returns E_OK if failover happened, E_NOTFOUND if no backup registered.
 */
err_t failover_on_quarantine(u32 vm_id);

/*
 * failover_trigger — force a failover for a VM (operator/manual path).
 */
err_t failover_trigger(u32 vm_id);

/* ── Query ── */
bool  failover_is_registered(u32 vm_id);
u32   failover_attempts(u32 vm_id);
void  failover_print_status(void);

#endif /* VSE_FAILOVER_H */
