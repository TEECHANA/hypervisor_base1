/*
 * dtb_gen.c — Minimal Flattened Device Tree builder
 *
 * Changes from original:
 *   - dtb_build_linux() now takes initrd_start_ipa and initrd_end_ipa
 *     instead of hard-coding 0x06000000 / 0x06800000.
 *   - Callers must pass the correct IPA values derived from where
 *     QEMU placed the initramfs in physical memory.
 *
 *   - FIX (root-node ordering): all root-level properties
 *     (#address-cells, #size-cells, compatible, model, ranges) are now
 *     emitted BEFORE the first subnode ("aliases").  Per the FDT spec a
 *     node's properties must precede its subnodes; libfdt stops property
 *     iteration at the first child BEGIN_NODE, so emitting #address-cells
 *     / #size-cells after the aliases node caused them to be dropped for
 *     the root.  Linux then fell back to default cell sizes and
 *     mis-parsed every "reg" (memory + GIC), which produced the faked
 *     1 GB memory node and the "GICD region ... has overlapping address"
 *     ioremap failure.
 *
 *   - FIX (warning): include the UART header so uart_puts() is declared
 *     (was an implicit-declaration warning).
 */

#include "dtb.h"
#include "../../lib/str/string.h"
#include "../../drivers/uart/uart.h"   /* for uart_puts() prototype */

#define FDT_MAGIC      0xD00DFEEDU
#define FDT_BEGIN_NODE 0x00000001U
#define FDT_END_NODE   0x00000002U
#define FDT_PROP       0x00000003U
#define FDT_END        0x00000009U

static void be32(void *p, u32 v)
{
    u8 *b = (u8 *)p;
    b[0] = (u8)(v >> 24);
    b[1] = (u8)(v >> 16);
    b[2] = (u8)(v >>  8);
    b[3] = (u8)(v);
}

static void be64(void *p, u64 v)
{
    be32(p,       (u32)(v >> 32));
    be32((u8*)p + 4, (u32)(v));
}

typedef struct {
    u8  *buf;
    u64  cap;
    u64  pos;
    u64  str_pos;
    u8  *strtab;
    u64  strtab_cap;
} fdt_ctx_t;

static void emit32(fdt_ctx_t *c, u32 v)
{
    if (c->pos + 4 <= c->cap) be32(c->buf + c->pos, v);
    c->pos += 4;
}

static void emit_str(fdt_ctx_t *c, const char *s)
{
    u64 len = strlen(s) + 1;
    if (c->pos + len <= c->cap) memcpy(c->buf + c->pos, s, len);
    c->pos += len;
    while (c->pos & 3) {
        if (c->pos < c->cap) c->buf[c->pos] = 0;
        c->pos++;
    }
}

static u32 strtab_add(fdt_ctx_t *c, const char *s)
{
    u32 off = (u32)c->str_pos;
    u64 len = strlen(s) + 1;
    if (c->str_pos + len <= c->strtab_cap)
        memcpy(c->strtab + c->str_pos, s, len);
    c->str_pos += len;
    return off;
}

static void begin_node(fdt_ctx_t *c, const char *name)
{
    emit32(c, FDT_BEGIN_NODE);
    emit_str(c, name);
}

static void end_node(fdt_ctx_t *c) { emit32(c, FDT_END_NODE); }

static void prop_str(fdt_ctx_t *c, const char *name, const char *val)
{
    u64 len = strlen(val) + 1;
    emit32(c, FDT_PROP);
    emit32(c, (u32)len);
    emit32(c, strtab_add(c, name));
    if (c->pos + len <= c->cap) memcpy(c->buf + c->pos, val, len);
    c->pos += len;
    while (c->pos & 3) {
        if (c->pos < c->cap) c->buf[c->pos] = 0;
        c->pos++;
    }
}

static void prop_stringlist(fdt_ctx_t *c, const char *name,
                            const char *val, u64 total_len)
{
    emit32(c, FDT_PROP);
    emit32(c, (u32)total_len);
    emit32(c, strtab_add(c, name));
    if (c->pos + total_len <= c->cap)
        memcpy(c->buf + c->pos, val, total_len);
    c->pos += total_len;
    while (c->pos & 3) {
        if (c->pos < c->cap) c->buf[c->pos] = 0;
        c->pos++;
    }
}

static void prop_u32(fdt_ctx_t *c, const char *name, u32 val)
{
    emit32(c, FDT_PROP);
    emit32(c, 4);
    emit32(c, strtab_add(c, name));
    emit32(c, val);
}

static void prop_cells2(fdt_ctx_t *c, const char *name, u64 hi, u64 lo)
{
    emit32(c, FDT_PROP);
    emit32(c, 16);
    emit32(c, strtab_add(c, name));
    be64(c->buf + c->pos, hi); c->pos += 8;
    be64(c->buf + c->pos, lo); c->pos += 8;
}

static void prop_empty(fdt_ctx_t *c, const char *name)
{
    emit32(c, FDT_PROP);
    emit32(c, 0);
    emit32(c, strtab_add(c, name));
}

/*
 * dtb_build_linux — build a minimal DTB for the Linux guest.
 *
 * initrd_start_ipa: IPA of the start of the initramfs image
 * initrd_end_ipa:   IPA of the end   of the initramfs image
 *
 * Derivation for the default QEMU run-with-guests setup:
 *   QEMU loads initramfs.cpio.gz at PA 0x47000000.
 *   Linux RAM region 1 maps IPA 0x0 → PA 0x41000000.
 *   initrd_start_ipa = 0x47000000 - 0x41000000 = 0x06000000
 *   initrd_end_ipa   = initrd_start_ipa + actual_size
 *                    = 0x06800000  (8 MB upper bound — safe for busybox)
 */
err_t dtb_build_linux(void *buf, u64 buf_sz,
                      u64 ram_base, u64 ram_size,
                      u64 uart_base, u32 uart_irq,
                      u64 initrd_start_ipa, u64 initrd_end_ipa,
                      u64 *out_sz)
{
    if (!buf || !buf_sz || !out_sz) return E_INVAL;
    memset(buf, 0, buf_sz);

    u8 *b = (u8 *)buf;
    u8  strbuf[1024];
    memset(strbuf, 0, sizeof(strbuf));

    fdt_ctx_t c = {
        .buf        = b + 48 + 16,
        .cap        = buf_sz - 48 - 16 - 1024,
        .pos        = 0,
        .strtab     = strbuf,
        .strtab_cap = sizeof(strbuf),
        .str_pos    = 0
    };

    /* ── Root node ──
     *
     * IMPORTANT: emit ALL root properties here, BEFORE any subnode.
     * libfdt stops reading a node's properties at the first child
     * BEGIN_NODE, so anything emitted after the "aliases" subnode would
     * be silently dropped for the root.
     */
    begin_node(&c, "");
    prop_u32(&c, "#address-cells", 2);
    prop_u32(&c, "#size-cells",    2);
    prop_str(&c, "compatible",    "linux,dummy-virt");
    prop_str(&c, "model",         "Tessolve-Hypervisor-Guest");
    prop_empty(&c, "ranges");

    /* ── Subnodes start here ── */

    /* aliases */
    begin_node(&c, "aliases"); prop_str(&c, "serial0", "/pl011@9000000"); end_node(&c);

    /* chosen — bootargs + initrd addresses */
    begin_node(&c, "chosen");

    prop_str(&c, "bootargs",
        "console=ttyAMA0,115200 earlycon=pl011,mmio32,0x09000000 "
        "init=/init loglevel=8 ignore_loglevel");
    //prop_str(&c, "stdout-path", "/pl011@9000000");
    prop_str(&c, "stdout-path", "serial0:115200n8");
    /* Use the caller-provided IPA values, NOT hard-coded addresses */
    prop_u32(&c, "linux,initrd-start", (u32)initrd_start_ipa);
    prop_u32(&c, "linux,initrd-end",   (u32)initrd_end_ipa);
    end_node(&c);

    /* memory region 1 — below device space */
    begin_node(&c, "memory@0");
    prop_str(&c,    "device_type", "memory");
    prop_cells2(&c, "reg", 0x00000000ULL, 0x08000000ULL);
    end_node(&c);

    /* memory region 2 — above device space */
    begin_node(&c, "memory@10000000");
    prop_str(&c,    "device_type", "memory");
    prop_cells2(&c, "reg", 0x10000000ULL, 0x40000000ULL);
    end_node(&c);

    /* GICv3 */
    begin_node(&c, "intc@8000000");
    prop_str(&c, "compatible", "arm,gic-v3");
    prop_u32(&c, "#interrupt-cells", 3);
    prop_empty(&c, "interrupt-controller");

    {
        emit32(&c, FDT_PROP);
        emit32(&c, 32);
        emit32(&c, strtab_add(&c, "reg"));
        /* GICD: base 0x08000000, size 0x00010000 (64 KB) */
        emit32(&c, 0x00000000); emit32(&c, 0x08000000);
        emit32(&c, 0x00000000); emit32(&c, 0x00010000);
        /* GICR: base 0x080A0000, size 0x00F60000 (QEMU virt window) */
        emit32(&c, 0x00000000); emit32(&c, 0x080A0000);
        emit32(&c, 0x00000000); emit32(&c, 0x00F60000);
    }
    prop_u32(&c, "phandle", 1);
    end_node(&c);

    /* APB clock for PL011 */
    begin_node(&c, "apb-clk");
    prop_str(&c, "compatible",     "fixed-clock");
    prop_u32(&c, "#clock-cells",   0);
    prop_u32(&c, "clock-frequency", 24000000);
    prop_u32(&c, "phandle",        2);
    end_node(&c);

    /* PL011 UART */
    begin_node(&c, "pl011@9000000");
    prop_stringlist(&c, "compatible", "arm,pl011\0arm,primecell", 23);
    prop_cells2(&c, "reg", uart_base, 0x1000);
    prop_u32(&c, "interrupt-parent", 1);
    {
        emit32(&c, FDT_PROP);
        emit32(&c, 12);
        emit32(&c, strtab_add(&c, "interrupts"));
        emit32(&c, 0);
        emit32(&c, uart_irq - 32);
        emit32(&c, 4);
    }
    {
        emit32(&c, FDT_PROP);
        emit32(&c, 8);
        emit32(&c, strtab_add(&c, "clocks"));
        emit32(&c, 2);
        emit32(&c, 2);
    }
    prop_stringlist(&c, "clock-names", "uartclk\0apb_pclk", 17);
    end_node(&c);

    /* ARM arch timer */
    begin_node(&c, "timer");
    prop_str(&c, "compatible", "arm,armv8-timer");
    prop_empty(&c, "always-on");
    prop_u32(&c, "interrupt-parent", 1);
    {
        emit32(&c, FDT_PROP);
        emit32(&c, 48);
        emit32(&c, strtab_add(&c, "interrupts"));

        /* <type=1 PPI, int#, flags=0x8 level-low> */
        emit32(&c, 1); emit32(&c, 13); emit32(&c, 8);  /* phys-secure */
        emit32(&c, 1); emit32(&c, 14); emit32(&c, 8);  /* phys-ns (main) */
        emit32(&c, 1); emit32(&c, 11); emit32(&c, 8);  /* virtual */
        emit32(&c, 1); emit32(&c, 10); emit32(&c, 8);  /* hypervisor */
        /*
        emit32(&c, 1); emit32(&c, 13); emit32(&c, 0xf08);
        emit32(&c, 1); emit32(&c, 14); emit32(&c, 0xf08);
        emit32(&c, 1); emit32(&c, 11); emit32(&c, 0xf08);
        emit32(&c, 1); emit32(&c, 10); emit32(&c, 0xf08);*/
    }
    end_node(&c);

    /* PSCI */
    begin_node(&c, "psci");
    prop_str(&c, "compatible", "arm,psci-0.2");
    prop_str(&c, "method",     "hvc");
    end_node(&c);

    /* CPUs */
    begin_node(&c, "cpus");
    prop_u32(&c, "#address-cells", 1);
    prop_u32(&c, "#size-cells",    0);
    begin_node(&c, "cpu@0");
    prop_str(&c, "device_type",   "cpu");
    prop_str(&c, "compatible",    "arm,cortex-a57");
    prop_str(&c, "enable-method", "psci");
    prop_u32(&c, "reg", 0);
    end_node(&c);

    /*
    begin_node(&c, "cpu@1");
    prop_str(&c, "device_type",   "cpu");
    prop_str(&c, "compatible",    "arm,cortex-a57");
    prop_str(&c, "enable-method", "psci");
    prop_u32(&c, "reg", 1);
    end_node(&c);*/

    end_node(&c);

    end_node(&c);   /* root */
    emit32(&c, FDT_END);

    u64 struct_sz = c.pos;
    u64 str_sz    = c.str_pos;

    memcpy(b + 48 + 16 + struct_sz, strbuf, str_sz);

    u64 total = 48 + 16 + struct_sz + str_sz;
    total = (total + 3) & ~3ULL;

    be32(b +  0, FDT_MAGIC);
    be32(b +  4, (u32)total);
    be32(b +  8, 48 + 16);
    be32(b + 12, (u32)(48 + 16 + struct_sz));
    be32(b + 16, 48);
    be32(b + 20, 17);
    be32(b + 24, 16);
    be32(b + 28, 0);
    be32(b + 32, (u32)str_sz);
    be32(b + 36, (u32)struct_sz);

    /* Memory reservation block terminator */
    be64(b + 48, 0);
    be64(b + 56, 0);

    *out_sz = total;

    uart_puts("DTB generated\n");
    uart_puts("\n");

    return E_OK;
}
