/*
 * guest_measure.h — VSE: guest image genuineness verification
 *
 * Requirement (Activity Tracker Image 1, rows 2-3 "Security & Trust Services" /
 * "Guest Enablement"): the VSE must check the genuineness of each guest VM by
 * hashing it before activation, and only authorize genuine guests.
 *
 * Before this module, trust_init() blindly marked every created VM as TRUSTED
 * with no measurement. This module measures each guest's loaded image
 * (SHA-256 over the image-sized prefix of its load region) and compares it
 * against a golden hash provisioned per guest. trust_init() now marks a VM
 * TRUSTED only if its measurement matches; a mismatch marks it QUARANTINE.
 *
 * Golden provisioning (same learn-mode pattern as component_check):
 *   1. Build with GUEST_MEASURE_LEARN_MODE 1
 *   2. Boot — the measured SHA-256 of each guest is logged
 *   3. Confirm it matches scripts/gen_vm_hash.py over the same image file
 *   4. Paste the hashes into _golden_guests[] below
 *   5. Set GUEST_MEASURE_LEARN_MODE 0, rebuild — mismatches now block trust
 *
 * NOTE on hashing the LOAD REGION vs the FILE: gen_vm_hash.py hashes the image
 * file. The guest's RAM region is larger than the file (tail is zero/unused),
 * so we hash exactly the first image_size bytes of the load region — which
 * equals the file bytes if the loader placed them unmodified. The per-guest
 * image_size is configured in the descriptor table in guest_measure.c.
 */

#ifndef VSE_GUEST_MEASURE_H
#define VSE_GUEST_MEASURE_H

#include "../include/types.h"
#include "../include/error.h"
#include "hmac.h"   /* SHA256_DIGEST / sha256 */

/* Result of measuring one guest. */
typedef enum {
    GMEAS_MATCH    = 0,   /* hash matches golden — genuine                  */
    GMEAS_MISMATCH = 1,   /* hash differs — tampered/unknown image          */
    GMEAS_LEARN    = 2,   /* learn mode or golden not provisioned — logged  */
    GMEAS_SKIP     = 3,   /* no descriptor for this VM — not measured        */
} gmeas_result_t;

/*
 * guest_measure_vm — hash one VM's loaded image and compare to its golden.
 * Returns GMEAS_MATCH only if a golden is provisioned AND the hash matches.
 * Logs the measured hash. Never panics.
 */
gmeas_result_t guest_measure_vm(u32 vm_id, const char *name);

#endif /* VSE_GUEST_MEASURE_H */
