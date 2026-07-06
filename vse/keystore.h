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

/*
 * Derive a purpose-specific secret from the master key:
 *     out = HMAC-SHA256(master_key, label)[0:len]
 *
 * This is the single provisioning root: the login password pepper and the
 * HOTP/TOTP secret are both derived from the one master key rather than being
 * independent hard-coded blobs. On real hardware the master key comes from the
 * per-device OTP fuse (VSE_HW_FUSE_KEY), so every derived secret is automatically
 * device-unique with no extra provisioning step.
 *
 * `label` is a NUL-terminated domain-separation string (e.g. "vse-hotp-secret-v1").
 * `len` must be 1..VSE_MASTER_KEY_LEN (single HMAC block; ample for our secrets).
 * Returns E_OK, or E_INVAL on bad args, or the master-key provider's error.
 */
err_t vse_derive_secret(const char *label, u8 *out, u32 len);

#endif /* VSE_KEYSTORE_H */
