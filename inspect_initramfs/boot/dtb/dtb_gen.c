/*
 * dtb_gen.c — Minimal Flattened Device Tree builder
 */
#include "dtb.h"
#include "../../lib/str/string.h"

#define FDT_MAGIC       0xD00DFEEDU
#define FDT_BEGIN_NODE  0x00000001U
#define FDT_END_NODE    0x00000002U
#define FDT_PROP        0x00000003U
#define FDT_END         0x00000009U

static void be32(void *p, u32 v)
{
    u8 *b = (u8*)p;
    b[0]=(u8)(v>>24); b[1]=(u8)(v>>16); b[2]=(u8)(v>>8); b[3]=(u8)v;
}
static void be64(void *p, u64 v)
{
    be32(p,        (u32)(v>>32));
    be32((u8*)p+4, (u32)(v));
}

typedef struct {
    u8  *buf;
    u64  cap;
    u64  pos;
    u64  str_pos;
    u8  *strtab;
    u64  strtab_cap;
} fdt_ctx_t;

/* ── Primitive emitters ── */

static void emit32(fdt_ctx_t *c, u32 v)
{
    if (c->pos+4 <= c->cap) be32(c->buf+c->pos, v);
    c->pos += 4;
}

static void emit_str(fdt_ctx_t *c, const char *s)
{
    u64 len = strlen(s)+1;
    if (c->pos+len <= c->cap) memcpy(c->buf+c->pos, s, len);
    c->pos += len;
    while (c->pos & 3) { if (c->pos < c->cap) c->buf[c->pos]=0; c->pos++; }
}

static u32 strtab_add(fdt_ctx_t *c, const char *s)
{
    u32 off = (u32)c->str_pos;
    u64 len = strlen(s)+1;
    if (c->str_pos+len <= c->strtab_cap)
        memcpy(c->strtab+c->str_pos, s, len);
    c->str_pos += len;
    return off;
}

/* ── Node helpers ── */

static void begin_node(fdt_ctx_t *c, const char *name)
{
    emit32(c, FDT_BEGIN_NODE);
    emit_str(c, name);
}

static void end_node(fdt_ctx_t *c)
{
    emit32(c, FDT_END_NODE);
}

/* ── Property helpers ── */

/* Single string property */
static void prop_str(fdt_ctx_t *c, const char *name, const char *val)
{
    u64 len = strlen(val)+1;
    emit32(c, FDT_PROP);
    emit32(c, (u32)len);
    emit32(c, strtab_add(c, name));
    if (c->pos+len <= c->cap) memcpy(c->buf+c->pos, val, len);
    c->pos += len;
    while (c->pos & 3) { if (c->pos < c->cap) c->buf[c->pos]=0; c->pos++; }
}

/* Multi-string (stringlist) property — caller provides exact total byte length */
static void prop_stringlist(fdt_ctx_t *c, const char *name,
                            const char *val, u64 total_len)
{
    emit32(c, FDT_PROP);
    emit32(c, (u32)total_len);
    emit32(c, strtab_add(c, name));
    if (c->pos+total_len <= c->cap) memcpy(c->buf+c->pos, val, total_len);
    c->pos += total_len;
    while (c->pos & 3) { if (c->pos < c->cap) c->buf[c->pos]=0; c->pos++; }
}

static void prop_u32(fdt_ctx_t *c, const char *name, u32 val)
{
    emit32(c, FDT_PROP);
    emit32(c, 4);
    emit32(c, strtab_add(c, name));
    emit32(c, val);
}

/* Two 64-bit big-endian values (address + size) */
static void prop_cells2(fdt_ctx_t *c, const char *name, u64 hi, u64 lo)
{
    emit32(c, FDT_PROP);
    emit32(c, 16);
    emit32(c, strtab_add(c, name));
    be64(c->buf+c->pos, hi); c->pos += 8;
    be64(c->buf+c->pos, lo); c->pos += 8;
}

static void prop_empty(fdt_ctx_t *c, const char *name)
{
    emit32(c, FDT_PROP);
    emit32(c, 0);
    emit32(c, strtab_add(c, name));
}

/* ── Main DTB builder ── */

err_t dtb_build_linux(void *buf, u64 buf_sz,
                      u64 ram_base, u64 ram_size,
                      u64 uart_base, u32 uart_irq,
                      u64 *out_sz)
{
    if (!buf || !buf_sz || !out_sz) return E_INVAL;
    memset(buf, 0, buf_sz);

    u8 *b = (u8*)buf;
    u8 strbuf[1024];
    memset(strbuf, 0, sizeof(strbuf));

    fdt_ctx_t c = {
        .buf        = b+48+16,
        .cap        = buf_sz-48-16-1024,
        .pos        = 0,
        .strtab     = strbuf,
        .strtab_cap = sizeof(strbuf),
        .str_pos    = 0
    };

    /* ── Root node ── */
    begin_node(&c, "");
      prop_u32(&c, "#address-cells", 2);
      prop_u32(&c, "#size-cells",    2);
      prop_str(&c,  "compatible",    "linux,dummy-virt");
      prop_str(&c,  "model",         "Tessolve-Hypervisor-Guest");

      /* chosen */
      begin_node(&c, "chosen");
        prop_str(&c, "bootargs",    "console=ttyAMA0 keep_bootcon earlycon rdinit=/init");
        prop_str(&c, "stdout-path", "/pl011@9000000");
        prop_u32(&c, "linux,initrd-start", 0x06000000U); 
        prop_u32(&c, "linux,initrd-end", 0x06800000U);
      end_node(&c);

      /* memory */
      /* memory region 1: below device space (128 MB) */
      begin_node(&c, "memory@0");
        prop_str(&c,    "device_type", "memory");
        prop_cells2(&c, "reg", 0x00000000ULL, 0x08000000ULL);
      end_node(&c);

      /* memory region 2: above device space (1 GB) */
      begin_node(&c, "memory@10000000");
        prop_str(&c,    "device_type", "memory");
        prop_cells2(&c, "reg", 0x10000000ULL, 0x40000000ULL);
      end_node(&c);

      /* GICv3 — must come before UART so phandle=1 is defined first */
      begin_node(&c, "intc@8000000");
        /* "arm,gic-v3" — single string */
        prop_str(&c, "compatible", "arm,gic-v3");
        prop_u32(&c, "#interrupt-cells", 3);
        prop_empty(&c, "interrupt-controller");
        /* reg: 4 x u64 = 32 bytes (GICD base/size, GICR base/size) */
        /* reg: GICD and GICR, each as <addr_hi addr_lo size_hi size_lo>
         * With #address-cells=2 and #size-cells=2, each range = 4 x u32
         * Two ranges = 8 x u32 = 32 bytes total */
        {
            emit32(&c, FDT_PROP);
            emit32(&c, 32);
            emit32(&c, strtab_add(&c, "reg"));
            /* GICD: base=0x08000000, size=0x10000 */
            emit32(&c, 0x00000000);  /* addr hi */
            emit32(&c, 0x08000000);  /* addr lo */
            emit32(&c, 0x00000000);  /* size hi */
            emit32(&c, 0x00010000);  /* size lo */
            /* GICR: base=0x080A0000, size=0xF60000 */
            emit32(&c, 0x00000000);  /* addr hi */
            emit32(&c, 0x080A0000);  /* addr lo */
            emit32(&c, 0x00000000);  /* size hi */
            emit32(&c, 0x00F60000);  /* size lo */
        }
        prop_u32(&c, "phandle", 1);
      end_node(&c);

      /* PL011 UART */
      begin_node(&c, "apb-clk");
        prop_str(&c, "compatible", "fixed-clock");
        prop_u32(&c, "#clock-cells", 0);
        prop_u32(&c, "clock-frequency", 24000000);
        prop_u32(&c, "phandle", 2);
      end_node(&c);

      /* PL011 UART */
      begin_node(&c, "pl011@9000000");
        prop_stringlist(&c, "compatible",
                        "arm,pl011\0arm,primecell", 23);
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
        /* clocks: two refs to phandle 2 (uartclk + apb_pclk same source) */
        {
            emit32(&c, FDT_PROP);
            emit32(&c, 8);
            emit32(&c, strtab_add(&c, "clocks"));
            emit32(&c, 2);   /* uartclk → apb-clk phandle */
            emit32(&c, 2);   /* apb_pclk → apb-clk phandle */
        }
        prop_stringlist(&c, "clock-names",
                        "uartclk\0apb_pclk", 17);
      end_node(&c);

      /* ARM arch timer */
      begin_node(&c, "timer");
        prop_str(&c, "compatible", "arm,armv8-timer");
        prop_empty(&c, "always-on");
        prop_u32(&c, "interrupt-parent", 1);
        /* 4 timer interrupts: secure, non-secure, virtual, hypervisor
         * Each: <type=PPI(1) intnum flags>
         * PPI 13=secure phys, 14=non-secure phys, 11=virtual, 10=hypervisor */
        {
            emit32(&c, FDT_PROP);
            emit32(&c, 48);   /* 4 interrupts x 3 cells x 4 bytes = 48 */
            emit32(&c, strtab_add(&c, "interrupts"));
            emit32(&c, 1); emit32(&c, 13); emit32(&c, 0xf08); /* secure phys */
            emit32(&c, 1); emit32(&c, 14); emit32(&c, 0xf08); /* non-secure phys */
            emit32(&c, 1); emit32(&c, 11); emit32(&c, 0xf08); /* virtual */
            emit32(&c, 1); emit32(&c, 10); emit32(&c, 0xf08); /* hypervisor */
        }
      end_node(&c);

      /* PSCI — advertise v0.2 only; psci_handler reports v1.1 via VERSION call */
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
        begin_node(&c, "cpu@1");
          prop_str(&c, "device_type",   "cpu");
          prop_str(&c, "compatible",    "arm,cortex-a57");
          prop_str(&c, "enable-method", "psci");
          prop_u32(&c, "reg", 1);
        end_node(&c);
      end_node(&c);

    end_node(&c); /* root */
    emit32(&c, FDT_END);

    u64 struct_sz = c.pos;
    u64 str_sz    = c.str_pos;

    /* String table immediately after struct block */
    memcpy(b+48+16+struct_sz, strbuf, str_sz);

    u64 total = 48+16+struct_sz+str_sz;
    total = (total+3) & ~3ULL;

    /* FDT header — all big-endian */
    be32(b+0,  FDT_MAGIC);
    be32(b+4,  (u32)total);
    be32(b+8,  48+16);                         /* off_dt_struct  */
    be32(b+12, (u32)(48+16+struct_sz));        /* off_dt_strings */
    be32(b+16, 48);                            /* off_mem_rsvmap */
    be32(b+20, 17);                            /* version        */
    be32(b+24, 16);                            /* last_comp_version */
    be32(b+28, 0);                             /* boot_cpuid_phys */
    be32(b+32, (u32)str_sz);                   /* size_dt_strings */
    be32(b+36, (u32)struct_sz);                /* size_dt_struct  */
    /* bytes 40-47: padding (already zeroed) */

    /* Memory reservation block: single terminating entry (0,0) */
    be64(b+48, 0);
    be64(b+56, 0);

    *out_sz = total;
    return E_OK;
}
