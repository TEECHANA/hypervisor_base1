/*
 * test_phase4b.c — Phase 4B unit tests: VGIC priority and mask handling
 *
 * Build:
 *   gcc -O0 -g -Wall tests/unit/test_phase4b.c -o build/test_phase4b
 *   ./build/test_phase4b
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t  u8;
typedef int32_t  s32;
typedef s32      err_t;

#define E_OK          0
#define E_INVAL      -3
#define E_UNSUPPORTED -9
#define FAIL(e)      ((e) != 0)

#define VGIC_MAX_IRQS  256
#define VGIC_NUM_WORDS (VGIC_MAX_IRQS / 32)

/* Inline vgic types for host testing */
typedef struct {
    u8   priority[VGIC_MAX_IRQS];
    u32  enable[VGIC_NUM_WORDS];
    u32  pending[VGIC_NUM_WORDS];
    bool initialised;
} vgic_dist_t;

typedef struct {
    u8  icc_pmr;
    u8  icc_bpr0;
    u32 icc_grpen1;
} vgic_cpu_t;

/* Inline implementations */
static void vgic_dist_init(vgic_dist_t *d) {
    memset(d, 0, sizeof(*d));
    for (u32 i = 0; i < VGIC_MAX_IRQS; i++) d->priority[i] = 0xA0;
    d->enable[0] = 0x0000FFFFu;
    d->initialised = true;
}

static void vgic_cpu_init(vgic_cpu_t *c) {
    c->icc_pmr = 0xFF; c->icc_bpr0 = 0x03; c->icc_grpen1 = 1;
}

static bool bitmap_get(const u32 *bm, u32 bit) {
    return (bm[bit>>5] >> (bit&31)) & 1u;
}
static void bitmap_set(u32 *bm, u32 bit) { bm[bit>>5] |=  (1u<<(bit&31)); }
static void bitmap_clr(u32 *bm, u32 bit) { bm[bit>>5] &= ~(1u<<(bit&31)); }

#define GICD_OFF_ISENABLER_BASE 0x100u
#define GICD_OFF_ICENABLER_BASE 0x180u
#define GICD_OFF_IPRIORITY_BASE 0x400u

static err_t vgic_dist_write(vgic_dist_t *d, u32 off, u32 val) {
    if (!d || !d->initialised) return E_INVAL;
    if (off >= 0x100u && off <= 0x17Cu) {
        u32 w = (off - 0x100u) / 4;
        if (w < VGIC_NUM_WORDS) d->enable[w] |= val;
    } else if (off >= 0x180u && off <= 0x1FCu) {
        u32 w = (off - 0x180u) / 4;
        if (w < VGIC_NUM_WORDS) { d->enable[w] &= ~val; d->enable[0] |= 0xFFFFu; }
    } else if (off >= 0x400u && off <= 0x4FCu) {
        u32 base = off - 0x400u;
        for (u32 b = 0; b < 4; b++) {
            u32 irq = base + b;
            if (irq < VGIC_MAX_IRQS) d->priority[irq] = (u8)((val >> (b*8)) & 0xFF);
        }
    }
    return E_OK;
}

static err_t vgic_dist_read(vgic_dist_t *d, u32 off, u32 *val) {
    if (!d || !val) return E_INVAL;
    *val = 0;
    if (!d->initialised) return E_OK;
    if (off >= 0x100u && off <= 0x17Cu) {
        u32 w = (off - 0x100u) / 4; if (w < VGIC_NUM_WORDS) *val = d->enable[w];
    } else if (off >= 0x400u && off <= 0x4FCu) {
        u32 base = off - 0x400u;
        for (u32 b = 0; b < 4; b++) {
            u32 irq = base + b;
            if (irq < VGIC_MAX_IRQS) *val |= ((u32)d->priority[irq]) << (b*8);
        }
    }
    return E_OK;
}

static u8 vgic_get_priority(const vgic_dist_t *d, u32 irq) {
    if (!d || !d->initialised || irq >= VGIC_MAX_IRQS) return 0xA0;
    return d->priority[irq];
}

static bool vgic_is_enabled(const vgic_dist_t *d, u32 irq) {
    if (!d || !d->initialised || irq >= VGIC_MAX_IRQS) return true;
    return bitmap_get(d->enable, irq);
}

static bool vgic_is_masked(const vgic_cpu_t *c, u8 prio) {
    if (!c) return false;
    return prio >= c->icc_pmr;
}

/* Test helpers */
static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ✓  %s\n", msg); _pass++; } \
    else       { printf("  ✗  %s  [line %d]\n", msg, __LINE__); _fail++; } \
} while(0)

/* ── Tests ── */

static void test_dist_init(void) {
    printf("\n--- test_dist_init ---\n");
    vgic_dist_t d;
    vgic_dist_init(&d);
    CHECK(d.initialised,                        "initialised flag set");
    CHECK(d.priority[0]  == 0xA0,              "IRQ 0 default priority 0xA0");
    CHECK(d.priority[33] == 0xA0,              "IRQ 33 default priority 0xA0");
    CHECK(d.priority[255]== 0xA0,              "IRQ 255 default priority 0xA0");
    CHECK(d.enable[0] == 0x0000FFFFu,          "SGIs (0-15) enabled at reset");
    CHECK(!(d.enable[1] & 0x3),                "SPI 32-33 disabled at reset");
}

static void test_cpu_init(void) {
    printf("\n--- test_cpu_init ---\n");
    vgic_cpu_t c;
    vgic_cpu_init(&c);
    CHECK(c.icc_pmr == 0xFF,    "PMR=0xFF (all priorities allowed)");
    CHECK(c.icc_grpen1 == 1,    "Group 1 enabled");
    CHECK(c.icc_bpr0 == 0x03,   "BPR0 reset value");
}

static void test_priority_write_read(void) {
    printf("\n--- test_priority_write_read ---\n");
    vgic_dist_t d;
    vgic_dist_init(&d);

    /* Write priority for IRQs 32-35 via GICD_IPRIORITYR: offset = 0x400+32 = 0x420 */
    /* Pack 4 bytes: IRQ32=0x20, IRQ33=0x40, IRQ34=0x60, IRQ35=0x80 */
    u32 prio_word = 0x80604020u;
    vgic_dist_write(&d, 0x420u, prio_word);
    CHECK(d.priority[32] == 0x20, "IRQ 32 priority = 0x20");
    CHECK(d.priority[33] == 0x40, "IRQ 33 priority = 0x40");
    CHECK(d.priority[34] == 0x60, "IRQ 34 priority = 0x60");
    CHECK(d.priority[35] == 0x80, "IRQ 35 priority = 0x80");

    /* Read back via vgic_dist_read */
    u32 rval = 0;
    vgic_dist_read(&d, 0x420u, &rval);
    CHECK(rval == prio_word, "IPRIORITYR read-back matches write");

    /* vgic_get_priority API */
    CHECK(vgic_get_priority(&d, 33) == 0x40, "vgic_get_priority(33) = 0x40");
    CHECK(vgic_get_priority(&d, 255) == 0xA0,"vgic_get_priority(255) = default");
    CHECK(vgic_get_priority(NULL, 33) == 0xA0,"NULL dist → default priority");
}

static void test_enable_disable(void) {
    printf("\n--- test_enable_disable ---\n");
    vgic_dist_t d;
    vgic_dist_init(&d);

    /* Enable SPI 33 via GICD_ISENABLER1 (offset 0x104) bit 1 */
    vgic_dist_write(&d, 0x104u, (1u << 1));
    CHECK(vgic_is_enabled(&d, 33), "IRQ 33 enabled after ISENABLER write");

    /* Enable SPI 32 */
    vgic_dist_write(&d, 0x104u, (1u << 0));
    CHECK(vgic_is_enabled(&d, 32), "IRQ 32 enabled");
    CHECK(vgic_is_enabled(&d, 33), "IRQ 33 still enabled after second write");

    /* Disable SPI 33 via GICD_ICENABLER1 (offset 0x184) bit 1 */
    vgic_dist_write(&d, 0x184u, (1u << 1));
    CHECK(!vgic_is_enabled(&d, 33), "IRQ 33 disabled after ICENABLER write");
    CHECK(vgic_is_enabled(&d, 32),  "IRQ 32 still enabled");

    /* SGIs cannot be disabled */
    vgic_dist_write(&d, 0x180u, 0xFFFFu);  /* try to disable SGIs 0-15 */
    CHECK(vgic_is_enabled(&d, 0),  "SGI 0 cannot be disabled");
    CHECK(vgic_is_enabled(&d, 15), "SGI 15 cannot be disabled");
}

static void test_pmr_masking(void) {
    printf("\n--- test_pmr_masking ---\n");
    vgic_cpu_t c;
    vgic_cpu_init(&c);

    /* Default PMR=0xFF: nothing masked */
    CHECK(!vgic_is_masked(&c, 0x00), "PMR=FF: priority 0x00 not masked");
    CHECK(!vgic_is_masked(&c, 0xA0), "PMR=FF: priority 0xA0 not masked");
    CHECK(!vgic_is_masked(&c, 0xFE), "PMR=FF: priority 0xFE not masked");

    /* Set PMR=0x80: only priorities < 0x80 are delivered */
    c.icc_pmr = 0x80;
    CHECK(!vgic_is_masked(&c, 0x40), "PMR=80: priority 0x40 passes (high prio)");
    CHECK(!vgic_is_masked(&c, 0x7F), "PMR=80: priority 0x7F passes");
    CHECK(vgic_is_masked(&c, 0x80),  "PMR=80: priority 0x80 masked (==PMR)");
    CHECK(vgic_is_masked(&c, 0xA0),  "PMR=80: priority 0xA0 masked (low prio)");
    CHECK(vgic_is_masked(&c, 0xFF),  "PMR=80: priority 0xFF masked");

    /* PMR=0x00: block everything */
    c.icc_pmr = 0x00;
    CHECK(vgic_is_masked(&c, 0x00), "PMR=00: even priority 0x00 masked");
}

static void test_injection_gating(void) {
    printf("\n--- test_injection_gating ---\n");
    vgic_dist_t d;
    vgic_cpu_t  c;
    vgic_dist_init(&d);
    vgic_cpu_init(&c);

    /* IRQ 33 disabled by default — injection should be blocked */
    CHECK(!vgic_is_enabled(&d, 33), "IRQ 33 disabled (cannot inject)");

    /* Enable IRQ 33 */
    vgic_dist_write(&d, 0x104u, (1u << 1));
    CHECK(vgic_is_enabled(&d, 33), "IRQ 33 enabled (can inject)");

    /* Set priority for IRQ 33 to 0xA0 */
    vgic_dist_write(&d, 0x420u, 0x0000A000u); /* IRQ32=0, IRQ33=0xA0 */
    u8 prio = vgic_get_priority(&d, 33);
    CHECK(prio == 0xA0, "IRQ 33 priority = 0xA0");

    /* PMR=0xFF: not masked */
    CHECK(!vgic_is_masked(&c, prio), "priority 0xA0 passes PMR=0xFF");

    /* Now mask low priority: PMR=0x80 */
    c.icc_pmr = 0x80;
    CHECK(vgic_is_masked(&c, prio),  "priority 0xA0 masked by PMR=0x80");

    /* High priority IRQ (0x40) still passes */
    CHECK(!vgic_is_masked(&c, 0x40), "priority 0x40 passes PMR=0x80");
}

static void test_multi_vm_isolation(void) {
    printf("\n--- test_multi_vm_isolation ---\n");
    vgic_dist_t vm1, vm2;
    vgic_dist_init(&vm1);
    vgic_dist_init(&vm2);

    /* VM1 programs IRQ33 priority 0x40 (offset 0x420, byte 1) */
    vgic_dist_write(&vm1, 0x420u, 0x00004000u);
    /* VM2 programs IRQ33 priority 0xC0 */
    vgic_dist_write(&vm2, 0x420u, 0x0000C000u);

    CHECK(vgic_get_priority(&vm1, 33) == 0x40, "VM1 IRQ33 priority = 0x40");
    CHECK(vgic_get_priority(&vm2, 33) == 0xC0, "VM2 IRQ33 priority = 0xC0");
    CHECK(vgic_get_priority(&vm1, 33) != vgic_get_priority(&vm2, 33),
          "VM1 and VM2 priorities are independent");

    /* VM2 enables IRQ33, VM1 doesn't */
    vgic_dist_write(&vm2, 0x104u, (1u << 1));
    CHECK(!vgic_is_enabled(&vm1, 33), "VM1 IRQ33 still disabled");
    CHECK( vgic_is_enabled(&vm2, 33), "VM2 IRQ33 enabled");
}

static void test_icc_pmr_trap_sim(void) {
    printf("\n--- test_icc_pmr_trap_sim ---\n");
    vgic_cpu_t c;
    vgic_cpu_init(&c);

    /* Simulate guest writing ICC_PMR_EL1 = 0xF0 */
    c.icc_pmr = 0xF0;
    CHECK(c.icc_pmr == 0xF0,              "icc_pmr stored correctly");
    CHECK(!vgic_is_masked(&c, 0x80),      "0x80 < 0xF0: passes");
    CHECK( vgic_is_masked(&c, 0xF0),      "0xF0 >= 0xF0: masked");
    CHECK( vgic_is_masked(&c, 0xFF),      "0xFF >= 0xF0: masked");

    /* Simulate guest reading ICC_PMR_EL1 */
    u8 read_back = c.icc_pmr;
    CHECK(read_back == 0xF0,              "icc_pmr read-back correct");

    /* Simulate ICC_GRPEN1 write */
    c.icc_grpen1 = 0;
    CHECK(c.icc_grpen1 == 0,              "icc_grpen1 can be disabled");
    c.icc_grpen1 = 1;
    CHECK(c.icc_grpen1 == 1,              "icc_grpen1 re-enabled");
}

int main(void) {
    printf("=== Phase 4B unit tests: VGIC priority and mask handling ===\n");
    test_dist_init();
    test_cpu_init();
    test_priority_write_read();
    test_enable_disable();
    test_pmr_masking();
    test_injection_gating();
    test_multi_vm_isolation();
    test_icc_pmr_trap_sim();
    printf("\n=== Results: %d passed, %d failed ===\n", _pass, _fail);
    if (_fail == 0) printf("    Phase 4B unit tests: ALL PASSED ✓\n");
    return _fail ? 1 : 0;
}
