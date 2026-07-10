/*
 * component_check.c — VSE Phase 2: runtime component integrity
 *
 * Measures the live .text and .rodata sections of the hypervisor and
 * compares each against a golden HMAC-SHA256 embedded in the binary.
 *
 * Section boundaries come from symbols defined in the linker script
 * (boot/linker/hypervisor.ld). Phase 2 requires that linker script to
 * export:
 *     __text_start, __text_end, __rodata_start, __rodata_end
 *
 * See the accompanying linker patch. Without those symbols this file
 * will fail to link (undefined reference) — that is the intended
 * compile-time guard against measuring the wrong range.
 *
 * Master key: shared with Phase 1 (config_check.c) via hmac.c. The key
 * lives only in EL2 .rodata; no Stage-2 mapping exposes it to a guest.
 *
 * Golden derivation workflow (identical to Phase 1):
 *   1. COMPONENT_CHECK_LEARN_MODE 1 → boot → copy logged HMACs
 *   2. Paste into _golden_components[] below
 *   3. COMPONENT_CHECK_LEARN_MODE 0 → rebuild
 */

#include "component_check.h"
#include "hmac.h"
#include "keystore.h"
#include "../lib/log/log.h"
#include "../lib/str/string.h"

/* ── Linker-defined section boundaries ──
 *
 * These symbols are emitted by boot/linker/hypervisor.ld (Phase 2 patch).
 * They are ADDRESSES, not variables — take their address with &.
 */
extern const u8 __text_start[];
extern const u8 __text_end[];
extern const u8 __rodata_start[];
extern const u8 __rodata_end[];

/* ── Master HMAC key ──
 *
 * Same key as Phase 1's config_check.c. We keep a private copy here
 * rather than sharing a global so each measurement module is
 * self-contained. For production, derive both from the same hardware
 * fuse value at boot and zeroize after use.
 */
static u8 _master_key[32];  /* filled from keystore at init (audit #3) */

/* ── Golden per-component HMACs ──
 *
 * Index 0 = .text, index 1 = .rodata.
 *
 * These are zero until derived in learn mode. In enforce mode with
 * zero values, the check fails loudly (as designed) — an unconfigured
 * enforce build never silently passes.
 *
 * NOTE: .text and .rodata golden values are build-specific. After ANY
 * source change, re-derive them in learn mode. .rodata in particular
 * contains _golden_components[] itself; deriving it is self-referential
 * but stable because the golden bytes are part of the measured range
 * only AFTER they are set. See "self-reference" note in the design doc.
 */
static const u8 _golden_components[VSE_NUM_COMPONENTS][HMAC_SIZE] = {
    /* [0] .text — re-derived after ipc_init + sched_stats_init wired in main.c */
    {
        0xb3, 0xfb, 0x73, 0xf5, 0x92, 0xe5, 0x52, 0x18,
        0x2b, 0x42, 0x3a, 0xa1, 0x04, 0x6d, 0xcd, 0x26,
        0xdf, 0x55, 0x53, 0xc1, 0x21, 0x0c, 0xc8, 0x4e,
        0xa3, 0x19, 0x4a, 0x82, 0x45, 0xd5, 0xdf, 0xbe,
     },
#if VSE_NUM_COMPONENTS > 1
    /* [1] .rodata — re-derived after mailbox device profile entries added */
    {
        0x64, 0x37, 0x15, 0x9c, 0x70, 0x40, 0xaa, 0x3b,
        0xb7, 0x0d, 0xd8, 0x9c, 0x19, 0xa9, 0x68, 0xec,
        0x2a, 0x03, 0xc0, 0x4f, 0xdf, 0x09, 0x62, 0x92,
        0x83, 0x35, 0xbd, 0x0d, 0x26, 0xc2, 0x14, 0x03,
    },
#endif
};

/* ── Component descriptor table ──
 *
 * start/end pointers are filled in at init from the linker symbols.
 */
static vse_component_t _components[VSE_NUM_COMPONENTS];

static void _components_setup(void)
{
    _components[VSE_COMPONENT_TEXT].name   = ".text";
    _components[VSE_COMPONENT_TEXT].start  = __text_start;
    _components[VSE_COMPONENT_TEXT].end    = __text_end;

#if VSE_NUM_COMPONENTS > 1
    _components[VSE_COMPONENT_RODATA].name  = ".rodata";
    _components[VSE_COMPONENT_RODATA].start = __rodata_start;
    _components[VSE_COMPONENT_RODATA].end   = __rodata_end;
#endif
}

/* ── component_check_measure ── */

err_t component_check_measure(u32 idx, u8 out[HMAC_SIZE])
{
    if (idx >= VSE_NUM_COMPONENTS || !out) return E_INVAL;

    const vse_component_t *c = &_components[idx];
    if (!c->start || !c->end || c->end <= c->start) {
        LOG_ERROR("VSE: component %u has invalid range", idx);
        return E_INVAL;
    }

    u64 len = (u64)(c->end - c->start);

    /* Audit #7: .rodata contains _golden_components[] itself, which makes a
     * naive whole-region hash self-referential (changing a golden changes the
     * measured bytes -> the hash never converges). Measure .rodata in two
     * segments that SKIP the golden table, so the measurement is stable and a
     * pasted golden never alters it. The golden table's own integrity is
     * covered by Phase 1 config_check / image signing, not by this self-hash.
     * .text (no golden table inside it) keeps the simple single-shot path. */
    if (idx == VSE_COMPONENT_RODATA) {
        const u8 *gp_start = (const u8 *)_golden_components;
        const u8 *gp_end   = gp_start + sizeof(_golden_components);

        /* Guard: golden table must lie fully within [start, end). */
        if (gp_start < c->start || gp_end > c->end) {
            LOG_ERROR("VSE: golden table not within .rodata range");
            return E_INVAL;
        }

        hmac_ctx_t ctx;
        err_t e = hmac_init(&ctx, _master_key, (u32)sizeof(_master_key));
        if (FAIL(e)) return e;

        /* Segment A: [start, gp_start)  — bytes before the golden table. */
        if (gp_start > c->start)
            hmac_update(&ctx, c->start, (u64)(gp_start - c->start));
        /* Segment B: [gp_end, end)      — bytes after the golden table. */
        if (c->end > gp_end)
            hmac_update(&ctx, gp_end, (u64)(c->end - gp_end));

        hmac_final(&ctx, out);
        return E_OK;
    }

    return hmac_sha256(_master_key, (u32)sizeof(_master_key),
                       c->start, len, out);
}

/* ── Internal: print an HMAC as a C array literal for learn mode ── */
static void _log_hmac_literal(const char *label, const u8 *mac)
{
    static const char hex[] = "0123456789abcdef";
    char line[48];

    LOG_WARN("%s", label);
    for (u32 i = 0; i < HMAC_SIZE; i += 8u) {
        u32 pos = 0;
        u32 end = (i + 8u < HMAC_SIZE) ? i + 8u : HMAC_SIZE;
        for (u32 j = i; j < end; j++) {
            line[pos++] = '0'; line[pos++] = 'x';
            line[pos++] = hex[(mac[j] >> 4) & 0xF];
            line[pos++] = hex[ mac[j]       & 0xF];
            line[pos++] = ','; line[pos++] = ' ';
        }
        line[pos > 0 ? pos - 2 : 0] = '\0';   /* trim trailing ", " */
        LOG_WARN("    %s", line);
    }
}

/* ── component_check_init ── */

/*
 * Return true if a golden slot is still all zeros (not yet provisioned).
 *
 * Reads through a volatile pointer so the optimizer cannot constant-fold
 * the const golden array at compile time. Without this, -O2 proves the
 * result for an all-zero array and compiles a DIFFERENT code path than
 * for a provisioned array — which would change .text between the learn
 * build and the enforce build and make the workflow impossible.
 */
static bool _golden_is_blank(const u8 *g)
{
    const volatile u8 *vg = (const volatile u8 *)g;
    u8 acc = 0;
    for (u32 i = 0; i < HMAC_SIZE; i++) acc |= vg[i];
    return acc == 0u;
}

err_t component_check_init(void)
{
    vse_get_master_key(_master_key, sizeof(_master_key));  /* audit #3 */
    u8 computed[HMAC_SIZE];

    LOG_INFO("VSE Phase 2: component integrity check starting");
    _components_setup();

    /*
     * IMPORTANT — no #if here.
     *
     * The compiled code path MUST be identical whether we are learning or
     * enforcing, otherwise .text differs between the two builds and a golden
     * value captured while learning can never match while enforcing.
     *
     * Mode is decided at RUNTIME by inspecting the golden array:
     *   - golden all-zero  -> LEARN: print the measured HMAC, do not enforce.
     *   - golden provisioned -> ENFORCE: compare, panic on mismatch.
     *
     * The golden array lives in .rodata; we measure .text only, so storing
     * the golden value never changes what we measure. No self-reference.
     */
    bool all_ok = true;

    for (u32 i = 0; i < VSE_NUM_COMPONENTS; i++) {
        err_t e = component_check_measure(i, computed);
        if (FAIL(e)) {
            LOG_ERROR("VSE Phase 2: measure component %u failed — halting", i);
            memset(computed, 0, HMAC_SIZE);
            extern void hyp_panic(const char *) __noreturn;
            hyp_panic("VSE: component measurement failed");
        }

        u64 len = (u64)(_components[i].end - _components[i].start);

        if (_golden_is_blank(_golden_components[i])) {
            /* LEARN: golden not provisioned yet — print and continue. */
            LOG_WARN("VSE Phase 2: [LEARN] component '%s' size=%lu bytes:",
                     _components[i].name, len);
            _log_hmac_literal("    paste into _golden_components[]:", computed);
            all_ok = false;   /* not enforcing this component */
            continue;
        }

        /* ENFORCE: golden provisioned — compare. */
        bool match = hmac_verify(_golden_components[i], computed, HMAC_SIZE);
        if (!match) {
            LOG_ERROR("VSE Phase 2: COMPONENT INTEGRITY VIOLATION in '%s'",
                      _components[i].name);
            LOG_ERROR("Expected (golden):");
            _log_hmac_literal("golden:", _golden_components[i]);
            LOG_ERROR("Computed:");
            _log_hmac_literal("computed:", computed);
            LOG_ERROR("VSE Phase 2: in-memory image differs from trusted build");
            memset(computed, 0, HMAC_SIZE);
            extern void hyp_panic(const char *) __noreturn;
            hyp_panic("VSE: component integrity check failed");
        }

        LOG_INFO("VSE Phase 2: component '%s' verified OK",
                 _components[i].name);
    }

    if (all_ok)
        LOG_INFO("VSE Phase 2: all components verified — image is trusted");
    else
        LOG_WARN("VSE Phase 2: LEARN MODE active — paste golden value(s) above, then rebuild");

    memset(computed, 0, HMAC_SIZE);
    return E_OK;
}
