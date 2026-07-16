/*
 * fault_detect.c — VSE Phase 5: Fault Detection & Recovery
 *
 * Bridges the existing hypervisor fault paths into the Phase 3 trust engine.
 * Each hook is O(1): bump a counter, optionally log (rate-limited), and call
 * trust_report_fault() so the trust state machine can escalate.
 *
 * Hooks NEVER panic — a detection path that crashed the hypervisor would be
 * worse than the fault it is reporting.
 */

#include "fault_detect.h"
#include "trust.h"
#include "ids.h"
#include "../lib/log/log.h"
#include "../lib/str/string.h"

/* ── Module state ── */
static fdetect_stats_t _stats;
static bool            _initialized = false;

/*
 * Log rate limiting: after this many events of a given class, stop logging
 * each one (still count them) to avoid flooding the UART during a storm.
 * trust_report_fault() still runs every time so escalation is unaffected.
 */
#define FDETECT_LOG_LIMIT  16u

/* ── ESR decode helpers (ARMv8 Data/Instruction Abort) ──
 *
 * DFSC (Data Fault Status Code) is ESR bits [5:0].
 *   0x04–0x07 : translation fault (level 0–3)
 *   0x08–0x0B : access flag fault
 *   0x0C–0x0F : permission fault (level 0–3)  ← cross-VM / NX violation
 *   0x10–0x13 : (this codebase logs these as "Stage-2 translation faults")
 */
static inline bool _is_permission_fault(u64 esr)
{
    u32 dfsc = (u32)(esr & 0x3Fu);
    return (dfsc >= 0x0Cu && dfsc <= 0x0Fu);
}

/* ── fault_detect_init ── */
err_t fault_detect_init(void)
{
    memset(&_stats, 0, sizeof(_stats));
    _initialized = true;
    LOG_INFO("VSE Phase 5: fault detection ready "
             "(hooks: mem, dma, irq -> trust engine)");
    return E_OK;
}

/* ── fdetect_mem_fault ── */
void fdetect_mem_fault(u32 vm_id, u64 ipa, u64 esr, bool is_write)
{
    if (!_initialized) return;

    _stats.mem_faults++;
    _stats.total++;

    bool perm = _is_permission_fault(esr);

    if (_stats.mem_faults <= FDETECT_LOG_LIMIT) {
        if (perm)
            LOG_WARN("VSE Phase 5: VM%u PERMISSION fault IPA=%lx %s "
                     "— possible cross-VM/NX violation",
                     vm_id, ipa, is_write ? "write" : "read");
        else
            LOG_WARN("VSE Phase 5: VM%u memory fault IPA=%lx ESR=%lx",
                     vm_id, ipa, esr);
    }

    /* A vm_id of 0 means hypervisor context — do not penalize a guest. */
    if (vm_id == 0u || vm_id > MAX_VMS) return;

    /* Feed the trust engine. Permission faults pack the write bit into the
     * detail field's top byte so policy can distinguish later if needed. */
    u64 detail = ipa | (is_write ? (1ULL << 63) : 0ULL);
    trust_report_fault(vm_id, TRUST_FAULT_MEM, detail);
    ids_notify_fault(vm_id, TRUST_FAULT_MEM, detail);
}

/* ── fdetect_dma_violation ── */
void fdetect_dma_violation(u32 vm_id, u64 fault_pa, u64 size, u32 stream_id)
{
    if (!_initialized) return;

    _stats.dma_violations++;
    _stats.total++;

    if (_stats.dma_violations <= FDETECT_LOG_LIMIT)
        LOG_ERROR("VSE Phase 5: VM%u DMA violation stream=%u PA=%lx size=%lx",
                  vm_id, stream_id, fault_pa, size);

    if (vm_id == 0u || vm_id > MAX_VMS) return;

    /* DMA violations are serious — report to trust engine. */
    trust_report_fault(vm_id, TRUST_FAULT_DMA, fault_pa);
    ids_notify_fault(vm_id, TRUST_FAULT_DMA, fault_pa);
}

/* ── fdetect_irq_unrouted ── */
void fdetect_irq_unrouted(u32 phys_irq)
{
    if (!_initialized) return;

    _stats.irq_unrouted++;
    _stats.total++;

    if (_stats.irq_unrouted <= FDETECT_LOG_LIMIT)
        LOG_WARN("VSE Phase 5: unrouted IRQ %u (no route entry)", phys_irq);

    /*
     * An unrouted IRQ has no owning VM by definition — it is a
     * hypervisor-context anomaly. We count it but do not penalize any
     * single guest. If you later add per-VM IRQ ownership tracking, this
     * is where you would call trust_report_fault() for the spoofing VM.
     */
}

/* ── Diagnostics ── */
void fdetect_get_stats(fdetect_stats_t *out)
{
    if (!out) return;
    *out = _stats;
}

void fdetect_print_stats(void)
{
    LOG_INFO("=== VSE Phase 5: Fault Detection Stats ===");
    LOG_INFO("  mem_faults=%u dma_violations=%u irq_unrouted=%u total=%u",
             _stats.mem_faults, _stats.dma_violations,
             _stats.irq_unrouted, _stats.total);
    LOG_INFO("==========================================");
}
