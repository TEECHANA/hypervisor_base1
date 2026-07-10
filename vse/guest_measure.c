/*
 * guest_measure.c — VSE guest image genuineness verification (see .h)
 *
 * Streaming SHA-256 over each guest's loaded image, compared to a golden hash.
 * Streaming (not one-shot) because the Linux image is tens of MB — we hash it
 * in chunks without copying.
 */

#include "guest_measure.h"
#include "hmac.h"
#include "../lib/log/log.h"
#include "../lib/str/string.h"

/*
 * Learn mode: when 1, measured hashes are logged and trust is NOT blocked on
 * mismatch (so you can provision goldens). Set to 0 to enforce.
 */
#ifndef GUEST_MEASURE_LEARN_MODE
#define GUEST_MEASURE_LEARN_MODE 0
#endif

/*
 * Per-guest descriptor: where the image is loaded (physical address) and how
 * many bytes to hash (the image file size — NOT the whole RAM region).
 *
 * These load addresses/sizes must match the platform loader (QEMU -device
 * loader / platform_init.c). image_size is the size of the on-disk image
 * (e.g. `ls -l guests/linux/Image`).
 */
typedef struct {
    const char *name;        /* matches vm->name */
    u64         load_pa;     /* physical address the image was loaded to */
    u64         image_size;  /* bytes to hash (file size) */
    bool        provisioned; /* is a golden hash set below? */
    u8          golden[SHA256_DIGEST];
} guest_desc_t;

/*
 * Golden table. image_size values come from `ls -l guests/<g>/<image>`:
 *   linux/Image        = 41656832
 *   rtos/rtos.bin      = 4904
 *   android_stub/...   = 744
 * Load PAs come from platform_init.c (LINUX_RAM_PA_BASE etc.). RTOS and Android
 * load PAs are their RAM bases (0x60000000, 0x70000000) per the boot DevProfile.
 *
 * golden[] starts unprovisioned (provisioned=false) → learn mode logs the hash.
 * After learning, paste the 32 bytes and set provisioned=true.
 */
static guest_desc_t _guests[] = {
    { "linux", 0x41200000ULL, 41656832ULL, true, {
        0x27, 0x40, 0x7c, 0xef, 0xc6, 0x0c, 0xe5, 0x6c,
        0xe1, 0x0b, 0xaf, 0xbb, 0xa4, 0x0b, 0x84, 0x3e,
        0x69, 0x35, 0xe5, 0x0e, 0x5d, 0x1d, 0xf1, 0x2e,
        0x57, 0x58, 0xe1, 0xbc, 0x5f, 0xff, 0xf0, 0x19,
    } },
    { "rtos", 0x60008000ULL, 5176ULL, true, {
        0xed, 0x99, 0x23, 0x14, 0xba, 0x4f, 0xff, 0x02,
        0xdf, 0x62, 0xd4, 0xbf, 0xf7, 0x1d, 0xa2, 0x74,
        0x47, 0x4f, 0xcd, 0xf9, 0xe0, 0xe9, 0x27, 0x19,
        0x52, 0x92, 0x05, 0x2d, 0x8c, 0xfc, 0xd6, 0x59,
    } },
    { "android", 0x70200000ULL, 2376ULL, true, {
        0x38, 0x4e, 0xa4, 0xbb, 0x94, 0x2c, 0x61, 0xd3,
        0xbd, 0xd5, 0x61, 0x6b, 0xf6, 0x53, 0xd4, 0xb3,
        0x67, 0x25, 0xc7, 0x1d, 0xdb, 0x37, 0x09, 0x83,
        0x0c, 0x0b, 0x28, 0x2a, 0x1e, 0xc8, 0xcb, 0x60,
    } },
};
#define NUM_GUESTS (sizeof(_guests) / sizeof(_guests[0]))

/* Hash chunk size for streaming. */
#define GMEAS_CHUNK 4096u

static guest_desc_t *_find(const char *name)
{
    for (u32 i = 0; i < NUM_GUESTS; i++)
        if (strcmp(_guests[i].name, name) == 0)
            return &_guests[i];
    return (guest_desc_t *)0;
}

static void _hash_region(u64 pa, u64 len, u8 out[SHA256_DIGEST])
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    u64 done = 0;
    while (done < len) {
        u64 n = (len - done) < GMEAS_CHUNK ? (len - done) : GMEAS_CHUNK;
        sha256_update(&ctx, (const u8 *)(uintptr_t)(pa + done), n);
        done += n;
    }
    sha256_final(&ctx, out);
}

static void _log_hash(const char *name, const u8 h[SHA256_DIGEST])
{
    /* Print as the same 4x8 byte layout used by component_check learn mode. */
    LOG_WARN("VSE: [GUEST-MEASURE] '%s' SHA-256:", name);
    for (u32 r = 0; r < 4; r++) {
        LOG_WARN("    0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x,",
                 h[r*8+0], h[r*8+1], h[r*8+2], h[r*8+3],
                 h[r*8+4], h[r*8+5], h[r*8+6], h[r*8+7]);
    }
}

gmeas_result_t guest_measure_vm(u32 vm_id, const char *name)
{
    (void)vm_id;
    guest_desc_t *g = _find(name);
    if (!g) {
        LOG_WARN("VSE: guest '%s' has no measurement descriptor — SKIP", name);
        return GMEAS_SKIP;
    }

    u8 measured[SHA256_DIGEST];
    _hash_region(g->load_pa, g->image_size, measured);

#if GUEST_MEASURE_LEARN_MODE
    LOG_WARN("VSE: [LEARN] measuring guest '%s' (%lu bytes @ 0x%lx)",
             name, g->image_size, g->load_pa);
    _log_hash(name, measured);
    memset(measured, 0, sizeof(measured));
    return GMEAS_LEARN;
#else
    if (!g->provisioned) {
        LOG_WARN("VSE: guest '%s' golden not provisioned — measured:", name);
        _log_hash(name, measured);
        memset(measured, 0, sizeof(measured));
        return GMEAS_LEARN;
    }
    /* Constant-time compare against golden. */
    bool ok = hmac_verify(g->golden, measured, SHA256_DIGEST);
    memset(measured, 0, sizeof(measured));
    if (ok) {
        LOG_INFO("VSE: guest '%s' genuineness VERIFIED", name);
        return GMEAS_MATCH;
    }
    LOG_ERROR("VSE: guest '%s' GENUINENESS CHECK FAILED — image not trusted", name);
    return GMEAS_MISMATCH;
#endif
}
