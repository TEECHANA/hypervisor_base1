/*
 * psci.c — PSCI handler for EL2 hypervisor
 * Handles PSCI calls arriving via HVC from Linux guest.
 */

#include "../../include/types.h"
#include "../../lib/log/log.h"

/* ── PSCI function IDs (ARM SMC Calling Convention) ── */
#define PSCI_VERSION            0x84000000UL
#define PSCI_CPU_SUSPEND_32     0x84000001UL
#define PSCI_CPU_OFF            0x84000002UL
#define PSCI_CPU_ON_32          0x84000003UL
#define PSCI_AFFINITY_INFO_32   0x84000004UL
#define PSCI_MIGRATE_32         0x84000005UL
#define PSCI_MIGRATE_INFO_TYPE  0x84000006UL
#define PSCI_MIGRATE_INFO_CPU32 0x84000007UL
#define PSCI_SYSTEM_OFF         0x84000008UL
#define PSCI_SYSTEM_RESET       0x84000009UL
#define PSCI_PSCI_FEATURES      0x8400000AUL
#define PSCI_CPU_FREEZE         0x8400000BUL
#define PSCI_CPU_DEFAULT_SUSPEND 0x8400000CUL
#define PSCI_NODE_HW_STATE      0x8400000DUL
#define PSCI_SYSTEM_SUSPEND     0x8400000EUL
#define PSCI_SET_SUSPEND_MODE   0x8400000FUL
#define PSCI_STAT_RESIDENCY_32  0x84000010UL
#define PSCI_STAT_COUNT_32      0x84000011UL
#define PSCI_SYSTEM_RESET2      0x84000012UL
#define PSCI_MEM_PROTECT        0x84000013UL
#define PSCI_MEM_PROTECT_CHECK  0x84000014UL

#define PSCI_CPU_SUSPEND_64     0xC4000001UL
#define PSCI_CPU_ON_64          0xC4000003UL
#define PSCI_AFFINITY_INFO_64   0xC4000004UL
#define PSCI_MIGRATE_64         0xC4000005UL
#define PSCI_MIGRATE_INFO_CPU64 0xC4000007UL
#define PSCI_SYSTEM_SUSPEND_64  0xC400000EUL
#define PSCI_STAT_RESIDENCY_64  0xC4000010UL
#define PSCI_STAT_COUNT_64      0xC4000011UL
#define SMCCC_VERSION           0x80000000UL
#define SMCCC_ARCH_FEATURES     0x80000001UL
#define SMCCC_VERSION_1_1       0x00010001UL
/* ── PSCI return codes ── */
#define PSCI_RET_SUCCESS            0
#define PSCI_RET_NOT_SUPPORTED     -1
#define PSCI_RET_INVALID_PARAMS    -2
#define PSCI_RET_DENIED            -3
#define PSCI_RET_ALREADY_ON        -4
#define PSCI_RET_ON_PENDING        -5
#define PSCI_RET_INTERNAL_FAILURE  -6
#define PSCI_RET_NOT_PRESENT       -7
#define PSCI_RET_DISABLED          -8
#define PSCI_RET_INVALID_ADDRESS   -9

/* ── PSCI MIGRATE_INFO_TYPE values ── */
#define PSCI_TOS_UP_MIGRATE_CAPABLE     0
#define PSCI_TOS_UP_NOT_MIGRATE_CAPABLE 1
#define PSCI_TOS_MP                     2   /* No TOS, no migration needed */

/* ── PSCI AFFINITY_INFO values ── */
#define PSCI_AFFINITY_LEVEL_ON          0
#define PSCI_AFFINITY_LEVEL_OFF         1
#define PSCI_AFFINITY_LEVEL_ON_PENDING  2

/*
 * PSCI VERSION encoding: major<<16 | minor
 * We report v1.1 which covers all calls Linux uses.
 */
#define PSCI_VERSION_1_1  ((1UL << 16) | 1UL)

/* Remove 'static inline' — use static only, forces a real function */
static bool psci_is_supported(u64 func_id)
{
    switch (func_id) {
    case PSCI_VERSION:
    case PSCI_CPU_SUSPEND_32:
    case PSCI_CPU_SUSPEND_64:
    case PSCI_CPU_OFF:
    case PSCI_CPU_ON_32:
    case PSCI_CPU_ON_64:
    case PSCI_AFFINITY_INFO_32:
    case PSCI_AFFINITY_INFO_64:
    case PSCI_MIGRATE_INFO_TYPE:
    case PSCI_SYSTEM_OFF:
    case PSCI_SYSTEM_RESET:
    case PSCI_PSCI_FEATURES:
        return true;
    default:
        return false;
    }
}

void psci_handler(u64 *regs)
{
    u64 func = regs[0];
    LOG_INFO("PSCI/SMCCC: func=0x%lx", func);

    switch (func) {

    /* ── Version ── */
    case PSCI_VERSION:
        LOG_INFO("PSCI: VERSION -> 1.1");
        regs[0] = PSCI_VERSION_1_1;
        break;

    /* ── Feature probe ── */
    case PSCI_PSCI_FEATURES: {
    	u64 query = regs[1];
    	u64 result;
    	/* Explicit comparison — not optimizable away */
    	if (query == PSCI_VERSION        ||
    	    query == PSCI_CPU_SUSPEND_32 ||
    	    query == PSCI_CPU_SUSPEND_64 ||
    	    query == PSCI_CPU_OFF        ||
    	    query == PSCI_CPU_ON_32      ||
    	    query == PSCI_CPU_ON_64      ||
    	    query == PSCI_AFFINITY_INFO_32 ||
    	    query == PSCI_AFFINITY_INFO_64 ||
    	    query == PSCI_MIGRATE_INFO_TYPE ||
    	    query == PSCI_SYSTEM_OFF     ||
    	    query == PSCI_SYSTEM_RESET   ||
    	    query == PSCI_PSCI_FEATURES  ||
    	    query == SMCCC_VERSION       ||
    	    query == SMCCC_ARCH_FEATURES) {
    	    LOG_INFO("PSCI: FEATURES 0x%lx -> supported (0)", query);
    	    result = (u64)PSCI_RET_SUCCESS;   /* must be 0 */
    	} else {
    	    LOG_INFO("PSCI: FEATURES 0x%lx -> NOT supported", query);
    	    result = (u64)(s64)PSCI_RET_NOT_SUPPORTED;  /* -1 */
    	}
    	regs[0] = result;
    	break;}
    

    /* ── Migrate info: no TOS, MP safe ── */
    case PSCI_MIGRATE_INFO_TYPE:
        LOG_INFO("PSCI: MIGRATE_INFO_TYPE -> TOS_MP (no migration)");
        regs[0] = PSCI_TOS_MP;
        break;

    /* ── CPU off: vCPU halts itself ── */
    case PSCI_CPU_OFF:
        LOG_INFO("PSCI: CPU_OFF");
        regs[0] = PSCI_RET_SUCCESS;
        /* TODO Phase 3: mark vCPU offline, yield scheduler */
        __asm__ volatile("wfi");
        break;

    /* ── CPU on: secondary CPU bring-up ── */
    case PSCI_CPU_ON_32:
    case PSCI_CPU_ON_64: {
        u64 target_mpidr = regs[1];
        u64 entry_pa     = regs[2];
        u64 context_id   = regs[3];
        LOG_INFO("PSCI: CPU_ON mpidr=0x%lx entry=0x%lx ctx=0x%lx",
                 target_mpidr, entry_pa, context_id);
        /* Single-vCPU: secondary CPUs not supported yet */
        regs[0] = (u64)PSCI_RET_NOT_SUPPORTED;
        break;
    }

    /* ── Affinity info: report all secondaries as OFF ── */
    case PSCI_AFFINITY_INFO_32:
    case PSCI_AFFINITY_INFO_64: {
        u64 target_mpidr    = regs[1];
        u64 lowest_affinity = regs[2];
        LOG_DEBUG("PSCI: AFFINITY_INFO mpidr=0x%lx lvl=%lu",
                  target_mpidr, lowest_affinity);
        /* Only CPU 0 (mpidr=0) is ON; all others are OFF */
        regs[0] = (target_mpidr == 0) ? PSCI_AFFINITY_LEVEL_ON
                                       : PSCI_AFFINITY_LEVEL_OFF;
        break;
    }

    /* ── CPU suspend: treat as WFI ── */
    case PSCI_CPU_SUSPEND_32:
    case PSCI_CPU_SUSPEND_64:
        LOG_DEBUG("PSCI: CPU_SUSPEND");
        __asm__ volatile("wfi");
        regs[0] = PSCI_RET_SUCCESS;
        break;

    /* ── System power control ── */
    case PSCI_SYSTEM_OFF:
        LOG_INFO("PSCI: SYSTEM_OFF — halting");
        __asm__ volatile("msr daifset, #0xF");
        while (1) __asm__ volatile("wfi");
        break;

    case PSCI_SYSTEM_RESET:
        LOG_INFO("PSCI: SYSTEM_RESET — halting (no reset impl)");
        __asm__ volatile("msr daifset, #0xF");
        while (1) __asm__ volatile("wfi");
        break;
        /* ── SMCCC arch calls ── */
    case SMCCC_VERSION:
    	LOG_INFO("PSCI: SMCCC_VERSION -> 1.1");
    	regs[0] = SMCCC_VERSION_1_1;
    	break;

    case SMCCC_ARCH_FEATURES:
    	LOG_INFO("PSCI: SMCCC_ARCH_FEATURES func=0x%lx -> not supported", regs[1]);
    	regs[0] = (u64)(s64)PSCI_RET_NOT_SUPPORTED;
    	break;

    default:
        LOG_WARN("PSCI: unhandled func=0x%lx", func);
        regs[0] = (u64)PSCI_RET_NOT_SUPPORTED;
        break;
    }
}
