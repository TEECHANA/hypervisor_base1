/*
 * mmio_trap.c — MMIO emulation and device protection (Phase 2 §2.2.1)
 *
 * Phase 2 changes:
 *
 * 1. VM ownership tracking:
 *    Each MMIO region can be tagged with an owner VM ID.
 *    If a different VM accesses the region, mmio_handle() logs a
 *    protection violation and returns a safe default value (0 for reads,
 *    discard for writes) instead of forwarding to the emulation handler.
 *    The offending VM continues running — we advance past the bad access
 *    rather than crashing the VM, which aids debugging.
 *
 * 2. mmio_set_current_vm():
 *    The scheduler calls this on every context switch. mmio_handle()
 *    compares the current VM against the registered owner.
 *
 * 3. Violation counter:
 *    Each region tracks how many times it has been accessed by the wrong VM.
 *    Useful for security auditing.
 */

#include "mmio_trap.h"
#include "../../include/config.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"
#include "../../core/vcpu/vcpu.h"

typedef struct {
    u64      base;
    u64      sz;
    mmio_fn  fn;
    void    *priv;
    bool     active;
    u32      owner_vm_id;   /* 0 = any VM allowed; N = only VM N allowed */
    u32      violation_cnt; /* how many cross-VM accesses were blocked    */
} mmio_reg_t;

static mmio_reg_t _regs[MAX_MMIO_REGIONS];
static u32 _nr            = 0;
static u32 _current_vm_id = 0;   /* updated by scheduler on every switch */

/* ── Registration ── */

err_t mmio_register(u64 base, u64 sz, mmio_fn fn, void *priv)
{
    if (_nr >= MAX_MMIO_REGIONS) return E_NOMEM;
    _regs[_nr++] = (mmio_reg_t){
        .base         = base,
        .sz           = sz,
        .fn           = fn,
        .priv         = priv,
        .active       = true,
        .owner_vm_id  = 0,      /* shared — any VM */
        .violation_cnt = 0,
    };
    LOG_DEBUG("MMIO: registered shared 0x%lx-0x%lx", base, base + sz);
    return E_OK;
}

err_t mmio_register_vm_device(u32 vm_id, u64 base, u64 sz,
                               mmio_fn fn, void *priv)
{
    if (_nr >= MAX_MMIO_REGIONS) return E_NOMEM;
    _regs[_nr++] = (mmio_reg_t){
        .base          = base,
        .sz            = sz,
        .fn            = fn,
        .priv          = priv,
        .active        = true,
        .owner_vm_id   = vm_id,
        .violation_cnt = 0,
    };
    LOG_INFO("MMIO: VM%u owns device 0x%lx-0x%lx", vm_id, base, base + sz);
    return E_OK;
}

void mmio_set_current_vm(u32 vm_id)
{
    _current_vm_id = vm_id;
}

/* ── Dispatch ── */

void mmio_handle(u64 fa, u64 esr, void *regs)
{
    UNUSED(regs);
    bool wr  = (bool)((esr >> 6) & 1);
    u64  val = 0;

    for (u32 i = 0; i < _nr; i++) {
        if (!_regs[i].active) continue;
        if (fa < _regs[i].base || fa >= _regs[i].base + _regs[i].sz) continue;

        /*
         * Phase 2 §2.2.1 — VM ownership check.
         *
         * owner_vm_id == 0 means "shared, any VM allowed".
         * Otherwise the current VM must match the registered owner.
         */
        if (_regs[i].owner_vm_id != 0 &&
            _regs[i].owner_vm_id != _current_vm_id)
        {
            _regs[i].violation_cnt++;
            LOG_WARN("MMIO VIOLATION: VM%u accessed VM%u device @ 0x%lx (%s) [#%u]",
                     _current_vm_id,
                     _regs[i].owner_vm_id,
                     fa,
                     wr ? "WRITE" : "READ",
                     _regs[i].violation_cnt);
            /*
             * Safe default: return 0 for reads, discard writes.
             * Advance ELR in the calling fault handler (fault.c already
             * advances ELR for unhandled DABTs).
             * Do NOT forward to the emulation handler.
             */
            if (!wr) {
                /* Write 0 to the destination register via ISS SRT field */
                u32 srt = (u32)((esr >> 16) & 0x1F);
                if (srt < 31 && g_current_vcpu[0]) {
                    g_current_vcpu[0]->regs.x[srt] = 0;
                }
            }
            return;
        }

        /* Correct VM — dispatch to emulation handler */
        if (_regs[i].fn) {
            err_t e = _regs[i].fn(fa, wr, &val, _regs[i].priv);
            if (e != E_OK) {
                LOG_WARN("MMIO: handler error %d on %s @ 0x%lx",
                         (int)e, wr ? "WR" : "RD", fa);
            }
            /* For reads, write result back to destination register */
            if (!wr) {
                u32 srt = (u32)((esr >> 16) & 0x1F);
                if (srt < 31 && g_current_vcpu[0]) {
                    g_current_vcpu[0]->regs.x[srt] = val;
                }
            }
        }
        return;
    }

    LOG_WARN("MMIO: unhandled %s @ 0x%lx (VM%u)",
             wr ? "WR" : "RD", fa, _current_vm_id);
}

/* Phase 4D */
u32 mmio_get_current_vm(void) { return _current_vm_id; }
