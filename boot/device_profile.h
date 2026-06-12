/*
 * device_profile.h — Device profile manager (Phase 2 §3.1.4)
 *
 * Provides a C-struct device profile table that mirrors the YAML configs
 * (configs/linux_vm.yaml, rtos_vm.yaml, android_vm.yaml).
 *
 * Instead of parsing YAML at runtime (impractical in a bare-metal hypervisor),
 * the profile is defined as a static C table here. platform_init.c reads
 * this table instead of hard-coding device assignments inline.
 *
 * To add a new device or reassign one to a different VM, edit the table
 * in device_profile.c — no changes to platform_init.c needed.
 *
 * Structure mirrors the YAML:
 *   vm_id    : which VM owns this device
 *   pa_base  : physical address base
 *   ipa_base : IPA (usually == PA for passthrough)
 *   size     : region size in bytes
 *   flags    : MEM_R | MEM_W | MEM_IO etc.
 *   irq      : SPI interrupt number (0 = no IRQ)
 *   stream_id: SMMU stream ID (0 = no DMA / not SMMU-managed)
 *   name     : human-readable name for log output
 */

#ifndef HYP_DEVICE_PROFILE_H
#define HYP_DEVICE_PROFILE_H

#include "../include/types.h"
#include "../include/error.h"

#define DEVPROF_MAX_ENTRIES 32
#define DEVPROF_NAME_LEN    32

typedef struct {
    u32  vm_id;                    /* owning VM (1=Linux, 2=RTOS, 3=Android) */
    u64  pa_base;                  /* physical address */
    u64  ipa_base;                 /* IPA (== pa_base for passthrough) */
    u64  size;                     /* region size in bytes */
    u32  flags;                    /* MEM_R | MEM_W | MEM_IO | MEM_X */
    u32  irq;                      /* SPI number, 0 = none */
    u32  stream_id;                /* SMMU stream ID, 0 = none */
    char name[DEVPROF_NAME_LEN];   /* human-readable name */
} dev_profile_entry_t;

/*
 * device_profile_init — apply the device profile to all VMs.
 *
 * Calls vm_add_mem() and vm_add_dev() for each entry in the profile table,
 * and registers each device with the MMIO trap handler (mmio_register_vm_device)
 * and SMMU (smmu_assign_stream) if applicable.
 *
 * Must be called after all VMs are created but before vm_finalize().
 * Returns E_OK if all entries applied successfully.
 */
err_t device_profile_init(void);

/*
 * device_profile_get — return pointer to the profile table and count.
 * Used by platform_init.c to iterate entries without duplicating logic.
 */
const dev_profile_entry_t *device_profile_get(u32 *count_out);

#endif /* HYP_DEVICE_PROFILE_H */
