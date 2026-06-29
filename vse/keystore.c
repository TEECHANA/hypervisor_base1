/* ── vse/keystore.c ──
 *
 * Audit fix #3: single definition of the VSE master HMAC key.
 * Formerly duplicated verbatim in config_check.c, component_check.c, seal.c.
 *
 * SECURITY SCOPE: see keystore.h. The dev key below still resides in
 * .rodata of this object and is extractable from the binary. The
 * VSE_HW_FUSE_KEY path is the seam for per-device OTP-derived keys
 * (NXP S32G / RPi4), which requires hardware not present under QEMU.
 */
#include "keystore.h"
#include "../include/types.h"
#include "../include/error.h"
#include "../lib/str/string.h"

#ifdef VSE_HW_FUSE_KEY
/* Hardware path (S32G/Pi4): derive the key from on-die OTP fuses at boot.
 * Stubbed — fill in with the SoC-specific fuse read when on real hardware.
 * Must write exactly VSE_MASTER_KEY_LEN bytes into out. */
extern err_t plat_read_fuse_key(u8 *out, u32 len);  /* provided by PAL */
#else
/* Development/QEMU key. Identical to the bytes formerly hardcoded in the
 * three VSE source files, so all existing HMAC verifications still match. */
static const u8 _dev_master_key[VSE_MASTER_KEY_LEN] = {
    0xa3, 0x7f, 0x2c, 0x91, 0xd4, 0x58, 0xbe, 0x06,
    0x1e, 0x82, 0x4a, 0xf3, 0x70, 0xc9, 0x55, 0x3d,
    0x08, 0xb1, 0xe7, 0x6f, 0x29, 0xda, 0x44, 0x9c,
    0x87, 0x5e, 0x13, 0xab, 0xfc, 0x60, 0x2b, 0x77,
};
#endif

err_t vse_get_master_key(u8 *out, u32 len)
{
    if (!out || len < VSE_MASTER_KEY_LEN) return E_INVAL;
#ifdef VSE_HW_FUSE_KEY
    return plat_read_fuse_key(out, VSE_MASTER_KEY_LEN);
#else
    memcpy(out, _dev_master_key, VSE_MASTER_KEY_LEN);
    return E_OK;
#endif
}
