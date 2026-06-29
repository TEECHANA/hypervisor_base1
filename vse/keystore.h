/* ── vse/keystore.h ──
 *
 * Single source of truth for the VSE master HMAC key.
 *
 * Audit fix #3: previously this 32-byte key was duplicated verbatim in
 * config_check.c, component_check.c, and seal.c. It is now defined once
 * here and retrieved via vse_get_master_key().
 *
 * SECURITY SCOPE (read this):
 *   Consolidation removes duplication and creates a single hardware seam.
 *   It does NOT make the key unextractable. In the QEMU/dev build the key
 *   is still a compile-time constant in .rodata of keystore.o and can be
 *   recovered from the binary. The genuine fix is to derive the key from
 *   per-device OTP fuses (NXP S32G / RPi4) at boot via the VSE_HW_FUSE_KEY
 *   path below, which requires real hardware not present under QEMU.
 */
#ifndef VSE_KEYSTORE_H
#define VSE_KEYSTORE_H

#include "../include/types.h"
#include "../include/error.h"

#define VSE_MASTER_KEY_LEN 32u

/* Fill `out` (at least VSE_MASTER_KEY_LEN bytes) with the master key.
 * Returns E_OK on success, E_INVAL if out is NULL or len too small. */
err_t vse_get_master_key(u8 *out, u32 len);

#endif /* VSE_KEYSTORE_H */
