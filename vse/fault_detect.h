/*
 * fault_detect.h — VSE Phase 5: Fault Detection & Recovery
 *
 * Phase 3 built the trust state machine (trust_report_fault → degrade →
 * quarantine). Phase 5 is what FEEDS it: it provides thin detection hooks
 * that the existing hypervisor fault paths call when a guest misbehaves.
 *
 * Hook points (all already exist in the codebase):
 *   - core/fault/fault.c    → Stage-2 translation faults (bad memory access)
 *   - vre/dma/dma_guard.c   → DMA boundary violations
 *   - vre/irq/irq_router.c  → unrouted / spoofed IRQs
 *
 * Each hook records the event and calls trust_report_fault(), which
 * auto-escalates the offending VM's trust level and, at threshold,
 * quarantines it (suspends the VM via the existing vm_suspend()).
 *
 * Design goals:
 *   - Zero behavior change when no faults occur (hooks are cheap, O(1)).
 *   - Never panic from a hook — detection must not crash the hypervisor.
 *   - Rate-limit logging so a fault storm can't flood the UART.
 */

#ifndef VSE_FAULT_DETECT_H
#define VSE_FAULT_DETECT_H

#include "../include/types.h"
#include "../include/error.h"

/* ── Lifecycle ── */
/*
 * fault_detect_init — reset counters. Call at boot after trust_init().
 */
err_t fault_detect_init(void);

/* ── Detection hooks (called from existing fault paths) ── */

/*
 * fdetect_mem_fault — a guest triggered a Stage-2 fault.
 *   vm_id   : offending VM (0 if unknown → treated as hypervisor-context)
 *   ipa     : faulting intermediate-physical address
 *   esr     : exception syndrome (for classification)
 *   is_write: true if the access was a write
 *
 * Distinguishes permission faults (potential cross-VM access — higher
 * severity) from plain translation faults (unmapped IPA).
 */
void fdetect_mem_fault(u32 vm_id, u64 ipa, u64 esr, bool is_write);

/*
 * fdetect_dma_violation — a DMA transfer targeted memory outside the VM.
 *   vm_id, fault_pa, size, stream_id come straight from dma_guard.
 */
void fdetect_dma_violation(u32 vm_id, u64 fault_pa, u64 size, u32 stream_id);

/*
 * fdetect_irq_unrouted — a physical IRQ arrived with no routing entry.
 * Could be a spoofed/unexpected interrupt. vm_id is 0 (hypervisor context).
 */
void fdetect_irq_unrouted(u32 phys_irq);

/* ── Diagnostics ── */
typedef struct {
    u32 mem_faults;
    u32 dma_violations;
    u32 irq_unrouted;
    u32 total;
} fdetect_stats_t;

void fdetect_get_stats(fdetect_stats_t *out);
void fdetect_print_stats(void);

#endif /* VSE_FAULT_DETECT_H */
