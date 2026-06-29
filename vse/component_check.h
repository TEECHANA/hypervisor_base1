/*
 * component_check.h — VSE Phase 2: runtime component integrity
 *
 * Phase 1 (config_check.c) proves the *configuration constants* match
 * a trusted build. Phase 2 proves the actual *code and read-only data*
 * loaded in memory match a trusted build — it detects tampering of the
 * hypervisor image itself (a modified .text section, a patched function,
 * a corrupted .rodata table).
 *
 * What it measures:
 *   - .text   (executable code)            → component 0
 *   - .rodata (constant tables, strings)   → component 1
 *
 * How:
 *   Each component is HMAC-SHA256'd over its byte range using the same
 *   master key as Phase 1. The result is compared against a golden value
 *   embedded in .rodata. A mismatch means the in-memory image differs
 *   from the trusted build → hyp_panic().
 *
 * Boot position (in hyp_main, AFTER config_check_init):
 *   pal_init()
 *   → config_check_init()       (Phase 1 — config constants)
 *   → component_check_init()    (Phase 2 — code + rodata)   ← THIS FILE
 *   → vm_subsys_init()
 *
 * Golden derivation (same learn-mode workflow as Phase 1):
 *   1. Set COMPONENT_CHECK_LEARN_MODE 1
 *   2. Boot once — the per-component golden HMACs are logged to UART
 *   3. Paste them into _golden_components[] in component_check.c
 *   4. Set COMPONENT_CHECK_LEARN_MODE back to 0, rebuild
 *
 * IMPORTANT: the golden values depend on the exact compiled binary.
 * ANY code change (even a one-line edit anywhere in the hypervisor)
 * changes .text and requires re-deriving the golden values in learn mode.
 * This is expected — that is precisely the property being enforced.
 */

#ifndef VSE_COMPONENT_CHECK_H
#define VSE_COMPONENT_CHECK_H

#include "../include/types.h"
#include "../include/error.h"
#include "hmac.h"

/*
 * Set to 1 during bring-up to derive and print per-component golden HMACs.
 * MUST be 0 in production — learn mode disables enforcement.
 */
#ifndef COMPONENT_CHECK_LEARN_MODE
#define COMPONENT_CHECK_LEARN_MODE  0
#endif

/* Number of measured components */
#define VSE_NUM_COMPONENTS   2u
#define VSE_COMPONENT_TEXT   0u
#define VSE_COMPONENT_RODATA 1u

/*
 * Descriptor for one measured component.
 * start/end are filled at runtime from linker-defined symbols.
 */
typedef struct {
    const char *name;
    const u8   *start;
    const u8   *end;
} vse_component_t;

/*
 * component_check_init — measure all components and enforce integrity.
 *
 * Returns E_OK if every component matches its golden HMAC.
 * In learn mode: always returns E_OK (logs the derived values instead).
 * In enforce mode: calls hyp_panic() on mismatch — never returns failure.
 *
 * Call from hyp_main() after config_check_init(), before vm_subsys_init().
 */
err_t component_check_init(void);

/*
 * component_check_measure — measure one component into out[HMAC_SIZE].
 * Exposed for the VSE update / re-verification path (later phases).
 */
err_t component_check_measure(u32 component_idx, u8 out[HMAC_SIZE]);

#endif /* VSE_COMPONENT_CHECK_H */
