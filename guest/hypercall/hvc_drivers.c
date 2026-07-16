/*
 * hvc_drivers.c — Hypervisor virtual driver HVC dispatch (Phase 5)
 *
 * This file implements the hypervisor-side handlers for all virtual
 * driver HVC calls. It is #included from hvc_handler.c or compiled
 * as a separate translation unit.
 *
 * State storage:
 *   The hypervisor maintains one global fuel_state_t and audio_state_t.
 *   RTOS writes fuel state via HVC_FUEL_UPDATE.
 *   Linux reads fuel state via HVC_OBD_READ_ALL (converted to OBD format).
 *   Android writes audio state via HVC_AUDIO_STATUS.
 *
 * Context switch logging:
 *   sched_on_driver_ctx() is called from sched.c on every context switch.
 *   It prints the most recent driver state alongside the CTX[N] line:
 *
 *     HYP [INF] CTX[5]: rtos -> linux (500ms, wfi)
 *     HYP [INF]   [FUEL→OBD] RPM=3200 speed=87km/h temp=92C DTC=none
 *
 *     HYP [INF] CTX[6]: linux -> rtos (2000ms, wfi)
 *     HYP [INF]   [OBD→FUEL] seq=847 data_age=499ms
 */

#include "../../include/types.h"
#include "../../include/error.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"
#include "../../core/vcpu/vcpu.h"
#include "../../core/vm/vm.h"
#include "../../vre/dma/dma_guard.h"   /* real DMA-isolation check + violation report */

/* ── Shared driver state (hypervisor-side storage) ── */

typedef struct {
    u32  rpm;
    u32  injection_us;
    u32  coolant_c;
    u32  throttle_pct;
    u32  map_kpa;
    u32  lambda_x100;     /* λ×100, e.g. 98 = λ0.98 */
    u32  cyl_status;      /* bits 0-3 = cyl 1-4 OK flags */
    u32  inj_count;
    u32  fault_code;      /* 0=none, P0xxx */
    u32  seq;
    bool valid;           /* true once RTOS has sent first update */
} hyp_fuel_state_t;

typedef struct {
    u32  frames_played;
    u32  frames_dropped;
    u32  latency_ms;
    bool playing;
    bool valid;
} hyp_audio_state_t;

static hyp_fuel_state_t  g_fuel  = { .valid = false };
static hyp_audio_state_t g_audio = { .valid = false };

/* ── Fuel state decode ── */
/*
 * Called from hvc_dispatch() case HVC_FUEL_UPDATE (0x0060).
 * x[1]-x[4] contain packed fuel ECU state from rtos.c.
 *
 * Encoding (matches rtos.c fuel_hvc_update()):
 *   x1 = rpm | (inj_us << 16) | (coolant_c << 32) | (throttle << 48)
 *   x2 = cyl_bits[3:0] | (inj_count << 8) | (fault_code << 32)
 *   x3 = seq | (lambda_x100 << 32)
 *   x4 = map_kpa
 */
void hvc_fuel_update(u64 *x)
{
    u64 x1 = x[1], x2 = x[2], x3 = x[3], x4 = x[4];
    LOG_INFO("FUEL RAW: x1=%lx x2=%lx x3=%lx x4=%lx", x1, x2, x3, x4);

    g_fuel.rpm           = (u32)(x1 & 0xFFFF);
    g_fuel.injection_us  = (u32)((x1 >> 16) & 0xFFFF);
    g_fuel.coolant_c     = (u32)((x1 >> 32) & 0xFFFF);
    g_fuel.throttle_pct  = (u32)((x1 >> 48) & 0xFFFF);

    g_fuel.cyl_status    = (u32)(x2 & 0xF);
    g_fuel.inj_count     = (u32)((x2 >> 8) & 0xFFFFFF);
    g_fuel.fault_code    = (u32)((x2 >> 32) & 0xFFFF);

    g_fuel.seq           = (u32)(x3 & 0xFFFFFFFF);
    g_fuel.lambda_x100   = (u32)((x3 >> 32) & 0xFFFF);

    g_fuel.map_kpa       = (u32)(x4 & 0xFFFF);
    g_fuel.valid         = true;
    LOG_INFO("FUEL HVC: rpm=%u cool=%u thr=%u map=%u seq=%u",
             g_fuel.rpm, g_fuel.coolant_c, g_fuel.throttle_pct,
             g_fuel.map_kpa, g_fuel.seq);

    /* Phase 5: export to shared memory so Linux /dev/mem can read it */
    #define OBD_SHMEM_PA  0x40F00000UL
    #define OBD_MAGIC     0x4F424400U
    typedef struct {
        u32 magic; u32 rpm; u32 speed_kmh; u32 coolant_c;
        u32 throttle_pct; u32 map_kpa; u32 dtc_count; u32 dtc_code;
        u32 seq; u32 cyl_status; u32 inj_count;
    } obd_shmem_t;
    volatile obd_shmem_t *shmem = (volatile obd_shmem_t *)(uintptr_t)OBD_SHMEM_PA;
    shmem->magic        = OBD_MAGIC;
    shmem->rpm          = g_fuel.rpm;
    shmem->speed_kmh    = g_fuel.rpm / 37;
    shmem->coolant_c    = g_fuel.coolant_c;
    shmem->throttle_pct = g_fuel.throttle_pct;
    shmem->map_kpa      = g_fuel.map_kpa;
    shmem->dtc_count    = (g_fuel.fault_code != 0) ? 1 : 0;
    shmem->dtc_code     = g_fuel.fault_code;
    shmem->seq          = g_fuel.seq;
    shmem->cyl_status   = g_fuel.cyl_status;
    shmem->inj_count    = g_fuel.inj_count;

    x[0] = (u64)E_OK;
}

/* ── OBD read — Linux requests current engine data ── */
/*
 * Called from hvc_dispatch() case HVC_OBD_READ_ALL (0x0051).
 * Returns OBD snapshot packed into x1-x3.
 *
 * If no RTOS data yet (g_fuel.valid == false), returns zeros in x1-x3
 * with x0=E_OK so Linux can show "waiting for ECU" message.
 *
 * Speed is derived from RPM: speed_kmh = rpm × gear_ratio × wheel_circ
 * Simplified: speed = rpm / 37  (approx for 5th gear, 195/65R15 tyre)
 */
void hvc_obd_read_all(u64 *x)
{
    if (!g_fuel.valid) {
        x[0] = (u64)E_OK;
        x[1] = x[2] = x[3] = 0;
        return;
    }

    /* Derive vehicle speed from RPM (simplified gearbox model) */
    u32 speed_kmh = g_fuel.rpm / 37;
    if (speed_kmh > 250) speed_kmh = 250;

    /* O2 sensor mV: λ=1.00 → 450mV, rich → <450mV, lean → >450mV */
    u32 o2_mv = (g_fuel.lambda_x100 >= 100)
                ? 450 + (g_fuel.lambda_x100 - 100) * 3
                : 450 - (100 - g_fuel.lambda_x100) * 5;

    u32 dtc_count = (g_fuel.fault_code != 0) ? 1 : 0;

    /* Pack into x1-x3 (matches obd_app.c decode) */
    x[1] = (u64)g_fuel.rpm
         | ((u64)speed_kmh         << 16)
         | ((u64)g_fuel.coolant_c  << 32)
         | ((u64)g_fuel.throttle_pct << 48);

    x[2] = (u64)g_fuel.map_kpa
         | ((u64)o2_mv             << 16)
         | ((u64)dtc_count         << 32)
         | ((u64)g_fuel.fault_code << 48);

    x[3] = (u64)g_fuel.seq;
    x[0] = (u64)E_OK;
}

/* ── OBD PID read — Linux requests one specific PID ── */
/*
 * Called from hvc_dispatch() case HVC_OBD_READ_PID (0x0050).
 * x[1] = PID number
 * x[0] = PID value (or 0xFFFFFFFF if not supported)
 */
void hvc_obd_read_pid(u64 *x)
{
    u32 pid = (u32)x[1];
    u32 val = 0xFFFFFFFF;

    if (g_fuel.valid) {
        switch (pid) {
        case 0x0C: val = g_fuel.rpm; break;
        case 0x0D: val = g_fuel.rpm / 37; break;
        case 0x05: val = g_fuel.coolant_c; break;
        case 0x11: val = g_fuel.throttle_pct; break;
        case 0x0B: val = g_fuel.map_kpa; break;
        case 0x01: val = (g_fuel.fault_code != 0) ? 1 : 0; break;
        default:   val = 0xFFFFFFFF; break;
        }
    }

    x[0] = (u64)val;
}

/* ── Audio status update ── */
void hvc_audio_status(u64 *x)
{
    g_audio.frames_played  = (u32)x[1];
    g_audio.frames_dropped = (u32)x[2];
    g_audio.latency_ms     = (u32)x[3];
    g_audio.playing        = true;
    g_audio.valid          = true;
    x[0] = (u64)E_OK;
}

void hvc_audio_play(u64 *x)
{
    g_audio.playing = true;
    g_audio.valid   = true;
    x[0] = (u64)E_OK;
}

void hvc_audio_stop(u64 *x)
{
    g_audio.playing = false;
    x[0] = (u64)E_OK;
}

/* ── Virtual DMA controller — a guest requests a DMA transfer ── */
/*
 * Called from hvc_dispatch() case HVC_DMA_XFER (0x0068).
 *   x[1] = target IPA, x[2] = size (bytes), x[3] = stream id
 *
 * The hypervisor validates the target against the CALLING VM's Stage-2 map via
 * the DMA guard. A legitimate in-bounds transfer returns E_OK. An out-of-bounds
 * target is a genuine DMA-isolation violation: it is reported to the trust
 * engine via dma_guard_log_violation() — the SAME real path the SMMU fault
 * handler uses — which drives fdetect -> IDS -> quarantine. Nothing is
 * fabricated: the offending address comes from the guest's own request.
 */
void hvc_dma_xfer(u64 *x)
{
    ipa_t   ipa  = (ipa_t)x[1];
    u64     size = x[2] ? x[2] : 0x1000ULL;
    u32     sid  = (u32)x[3];

    vcpu_t *vc = g_current_vcpu[0];
    vm_t   *vm = vc ? vc->vm : NULL;
    if (!vm) { x[0] = (u64)(s64)E_INVAL; return; }

    err_t e = dma_guard_check_ipa(vm, ipa, size);
    if (OK(e)) {
        /* In-bounds: a real DMA engine would program the descriptor here. */
        x[0] = (u64)E_OK;
        return;
    }

    /* Out-of-bounds DMA request — funnel through the real violation path. */
    dma_guard_log_violation(sid, vm->id, (paddr_t)ipa, size);
    x[0] = (u64)(s64)E_DENIED;
}

/* ── Context switch driver log ── */
/*
 * Called from sched.c after every CTX[N] log line.
 * Prints a summary of the current driver state to show what each VM
 * was doing during its slice. This is the "output during context switch"
 * requirement from the spec.
 *
 * prev_vm_id: VM that just ran
 * next_vm_id: VM about to run
 */
void sched_on_driver_ctx(u32 prev_vm_id, u32 next_vm_id)
{
    if (!g_fuel.valid && !g_audio.valid) return;

    /* When switching away from RTOS: show what fuel ECU was doing */
    if (prev_vm_id == 2 && g_fuel.valid) {
        LOG_INFO("  [FUEL→HYP] RPM=%u inj=%uus thr=%u%% temp=%uC seq=%u",
                 g_fuel.rpm, g_fuel.injection_us,
                 g_fuel.throttle_pct, g_fuel.coolant_c, g_fuel.seq);

        /* Cylinder status */
        u32 cs = g_fuel.cyl_status;
        LOG_INFO("  [FUEL→HYP] Cyl1:%s Cyl2:%s Cyl3:%s Cyl4:%s DTC:%s",
                 (cs & 1) ? "OK" : "MISS",
                 (cs & 2) ? "OK" : "MISS",
                 (cs & 4) ? "OK" : "MISS",
                 (cs & 8) ? "OK" : "MISS",
                 g_fuel.fault_code ? "P0300" : "none");
    }

    /* When switching away from Linux: show what OBD app was reading */
    if (prev_vm_id == 1 && g_fuel.valid) {
        u32 speed = g_fuel.rpm / 37;
        LOG_INFO("  [OBD→HYP] RPM=%u speed=%ukm/h cool=%uC seq=%u",
                 g_fuel.rpm, speed, g_fuel.coolant_c, g_fuel.seq);
    }

    /* When switching away from Android: show audio status */
    if (prev_vm_id == 3 && g_audio.valid) {
        LOG_INFO("  [AUDIO→HYP] frames=%u dropped=%u lat=%ums %s",
                 g_audio.frames_played, g_audio.frames_dropped,
                 g_audio.latency_ms,
                 g_audio.playing ? "playing" : "stopped");
    }

    /* When switching TO RTOS: remind what Linux needs */
    if (next_vm_id == 2 && g_fuel.valid) {
        LOG_INFO("  [HYP→FUEL] Last seq=%u — RTOS will update",
                 g_fuel.seq);
    }
}

/* ── Getters for use by sched.c without including this header directly ── */
bool driver_fuel_valid(void)  { return g_fuel.valid; }
bool driver_audio_valid(void) { return g_audio.valid; }
