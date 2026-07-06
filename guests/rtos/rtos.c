/*
 * rtos.c — RTOS guest with Fuel Injection virtual driver (Phase 5A §5.3.1)
 *
 * Architecture:
 *   The RTOS simulates a 4-cylinder gasoline engine ECU.
 *   Each 500ms scheduler slot = one engine management cycle:
 *
 *   1. fuel_inject_task() updates the fuel state:
 *      - RPM follows a simple drive cycle: idle → accelerate → cruise → decel
 *      - Injection pulse width = (14.7 AFR × fuel_per_cycle) / injector_flow
 *      - Each cylinder fires in order: 1→3→4→2 (standard firing order)
 *      - Lambda (O2) sensor simulates closed-loop correction
 *
 *   2. fuel_hvc_update() sends the state to the hypervisor via HVC 0x0060
 *      The hypervisor stores it so Linux OBD app can read it via HVC 0x0051
 *
 *   3. fuel_print_status() prints the current state to UART so it appears
 *      in the context switch output between CTX[N] lines
 *
 * Output during context switch:
 *   [FUEL] RPM=3200 inj=2400us λ=0.98 temp=92°C
 *   [FUEL] Cyl1:OK Cyl2:OK Cyl3:OK Cyl4:OK  inj#=1847  fault=none
 */

#define UART_BASE 0x09000000UL

/* ── UART helpers ── */
static void uart_putc(char c)
{
    volatile unsigned int *uartdr = (volatile unsigned int *)UART_BASE;
    *uartdr = (unsigned int)c;
}
static void uart_puts(const char *s)
{
    while (*s) { if (*s == '\n') uart_putc('\r'); uart_putc(*s++); }
}
static void uart_putu(unsigned long v)
{
    char buf[20]; int i = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v) { buf[i++] = '0' + (int)(v % 10); v /= 10; }
    while (i--) uart_putc(buf[i]);
}
static void uart_puti(int v)
{
    if (v < 0) { uart_putc('-'); uart_putu((unsigned long)-v); }
    else uart_putu((unsigned long)v);
}
/* Print fixed-point x/100 as "x.xx" */
static void uart_putfixed(unsigned long v)
{
    uart_putu(v / 100);
    uart_putc('.');
    unsigned long frac = v % 100;
    if (frac < 10) uart_putc('0');
    uart_putu(frac);
}

/* ── Timer helpers ── */
static unsigned long read_cntpct(void)
{
    unsigned long v; asm volatile("mrs %0, cntpct_el0" : "=r"(v)); return v;
}
static unsigned long read_cntfrq(void)
{
    unsigned long v; asm volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v;
}
static void busy_ms(unsigned int ms)
{
    unsigned long freq  = read_cntfrq();
    unsigned long ticks = (freq / 1000UL) * ms;
    unsigned long start = read_cntpct();
    while ((read_cntpct() - start) < ticks) asm volatile("nop");
}

/* ── HVC call (fuel injection update) ── */
/* HVC_CALL macro — avoids compiler variable shadowing with -O2 */
#define hvc_call(_id, _a1, _a2, _a3, _a4) do { \
    register unsigned long _x0 asm("x0") = (unsigned long)(_id); \
    register unsigned long _x1 asm("x1") = (unsigned long)(_a1); \
    register unsigned long _x2 asm("x2") = (unsigned long)(_a2); \
    register unsigned long _x3 asm("x3") = (unsigned long)(_a3); \
    register unsigned long _x4 asm("x4") = (unsigned long)(_a4); \
    asm volatile("hvc #0" \
        : "+r"(_x0),"+r"(_x1),"+r"(_x2),"+r"(_x3),"+r"(_x4) \
        :: "x5","x6","x7","memory"); \
} while(0)

/* ── Fuel injection simulation ── */

#define FUEL_NUM_CYLINDERS  4
#define HVC_FUEL_UPDATE     0x0060u
#define HVC_DMA_XFER        0x0068u   /* virtual DMA controller (see virtual_drivers.h) */

/* Drive cycle states */
typedef enum { IDLE=0, ACCEL, CRUISE, DECEL } drive_state_t;

/* Engine parameters */
static unsigned int  g_rpm         = 800;
static unsigned int  g_target_rpm  = 800;
static unsigned int  g_coolant_c   = 20;    /* cold start */
static unsigned int  g_throttle    = 5;     /* percent */
static unsigned int  g_inj_count   = 0;
static unsigned int  g_seq         = 0;
static unsigned char g_cyl_ok[FUEL_NUM_CYLINDERS] = {1,1,1,1};
static drive_state_t g_drive       = IDLE;
static unsigned int  g_drive_tick  = 0;

/* Firing order: Cyl1→Cyl3→Cyl4→Cyl2 (standard 4-cyl) */
static const unsigned char FIRING_ORDER[FUEL_NUM_CYLINDERS] = {0, 2, 3, 1};

/*
 * update_drive_cycle — advances the simulated drive scenario.
 * Every 5 slots (~2.5s) we move to the next phase.
 */
static void update_drive_cycle(void)
{
    g_drive_tick++;
    if (g_drive_tick < 5) return;
    g_drive_tick = 0;

    switch (g_drive) {
    case IDLE:
        g_drive = ACCEL;
        g_target_rpm  = 3200;
        g_throttle    = 45;
        break;
    case ACCEL:
        g_drive = CRUISE;
        g_target_rpm  = 2800;
        g_throttle    = 30;
        break;
    case CRUISE:
        g_drive = DECEL;
        g_target_rpm  = 1200;
        g_throttle    = 10;
        break;
    case DECEL:
        g_drive = IDLE;
        g_target_rpm  = 800;
        g_throttle    = 5;
        break;
    }
}

/*
 * update_engine — physics step called once per 100ms slice.
 * RPM ramps toward target at 200 RPM/step.
 * Coolant warms up to operating temperature.
 */
static void update_engine(void)
{
    /* RPM ramping */
    if (g_rpm < g_target_rpm) {
        g_rpm += 200;
        if (g_rpm > g_target_rpm) g_rpm = g_target_rpm;
    } else if (g_rpm > g_target_rpm) {
        g_rpm -= 200;
        if (g_rpm < g_target_rpm) g_rpm = g_target_rpm;
    }

    /* Coolant warm-up: 2°C per cycle toward 95°C */
    if (g_coolant_c < 95) g_coolant_c += 2;

    /* Injection count: 2 injections per rev per cylinder = rpm/60*2*4 per sec
     * Per 100ms = rpm/60*2*4/10 */
    unsigned int inj_per_100ms = (g_rpm * 8) / 600;
    g_inj_count += inj_per_100ms;

    /* Cylinder health: introduce a misfire at high RPM if cold */
    if (g_coolant_c < 40 && g_rpm > 2000)
        g_cyl_ok[2] = 0;   /* Cyl3 misfires when cold + high RPM */
    else
        g_cyl_ok[2] = 1;
}

/*
 * calc_injection_us — returns injection pulse width in microseconds.
 * Simplified Bosch Motronic formula:
 *   pulse_width = (engine_displacement / cylinders) * AFR_target / injector_flow
 *   At idle (800RPM): ~1200µs
 *   At cruise (2800RPM): ~2400µs
 *   At full throttle (5000RPM): ~4200µs
 */
static unsigned int calc_injection_us(void)
{
    /* Base pulse = 800µs + throttle contribution */
    unsigned int base = 800 + (g_throttle * 68);
    /* Lambda correction: if coolant cold, enrich by 20% */
    if (g_coolant_c < 60) base = (base * 120) / 100;
    return base;
}

/*
 * fuel_hvc_update — send fuel state to hypervisor via HVC 0x0060.
 * Packs state into x1-x4 registers (4×u64 = 32 bytes of data).
 *
 * Encoding:
 *   x1 = rpm | (injection_us << 16) | (coolant_c << 32) | (throttle << 48)
 *   x2 = cyl_status (bit 0-3) | (inj_count << 8) | (fault_code << 32)
 *   x3 = seq | (lambda_mv << 32)
 *   x4 = manifold_pressure_kpa (100 = 100kPa = atmospheric)
 */
static void fuel_hvc_update(unsigned int inj_us)
{
    unsigned long x1 = (unsigned long)g_rpm
                     | ((unsigned long)inj_us         << 16)
                     | ((unsigned long)g_coolant_c    << 32)
                     | ((unsigned long)g_throttle     << 48);

    unsigned long cyl_bits = (unsigned long)g_cyl_ok[0]
                           | ((unsigned long)g_cyl_ok[1] << 1)
                           | ((unsigned long)g_cyl_ok[2] << 2)
                           | ((unsigned long)g_cyl_ok[3] << 3);
    unsigned long fault = (!g_cyl_ok[2]) ? 0x0300 : 0; /* P0300 = misfire */
    unsigned long x2 = cyl_bits | ((unsigned long)g_inj_count << 8) | (fault << 32);

    /* Lambda: simulate 0.97–1.03 range (×100 for integer) */
    unsigned long lambda = 98 + (g_rpm % 6);  /* 98-103 ×100 */
    g_seq++;
    unsigned long x3 = (unsigned long)g_seq | (lambda << 32);

    /* MAP: idle=35kPa, cruise=70kPa, full throttle=100kPa */
    unsigned long map_kpa = 35 + (g_throttle * 65) / 100;
    unsigned long x4 = map_kpa;

    hvc_call(HVC_FUEL_UPDATE, x1, x2, x3, x4);
}

/*
 * fuel_print_status — prints the current engine state to UART.
 * Called once per 500ms slot (after 5×100ms work iterations).
 */
static void fuel_print_status(unsigned int inj_us)
{
    uart_puts("[FUEL] RPM=");
    uart_putu(g_rpm);
    uart_puts(" inj=");
    uart_putu(inj_us);
    uart_puts("us thr=");
    uart_putu(g_throttle);
    uart_puts("% temp=");
    uart_puti((int)g_coolant_c);
    uart_puts("C\r\n");

    uart_puts("[FUEL] Cyl1:");
    uart_puts(g_cyl_ok[0] ? "OK" : "MISS");
    uart_puts(" Cyl2:");
    uart_puts(g_cyl_ok[1] ? "OK" : "MISS");
    uart_puts(" Cyl3:");
    uart_puts(g_cyl_ok[2] ? "OK" : "MISS");
    uart_puts(" Cyl4:");
    uart_puts(g_cyl_ok[3] ? "OK" : "MISS");
    uart_puts("  inj#=");
    uart_putu(g_inj_count);

    /* Lambda: stored as ×100 in seq packing; decode for display */
    unsigned long lambda = 98 + (g_rpm % 6);
    uart_puts("  λ=0.");
    uart_putu(lambda);

    unsigned long fault = (!g_cyl_ok[2]) ? 0x0300 : 0;
    if (fault) {
        uart_puts("  DTC=P0");
        uart_putu(fault);
    } else {
        uart_puts("  DTC=none");
    }
    uart_puts("\r\n");

    const char *phase_name[] = {"idle", "accel", "cruise", "decel"};
    uart_puts("[FUEL] phase=");
    uart_puts(phase_name[g_drive]);
    uart_puts("\r\n");
}

/* ── RTOS main ── */

void rtos_main(void)
{
    uart_puts("\r\n");
    uart_puts("[RTOS] *** Tessolve RTOS — Fuel Injection ECU v1.0 ***\r\n");
    uart_puts("[RTOS] Engine: 4-cyl 2.0L gasoline, Bosch-style injection\r\n");

    /* Startup sequence: 10 ticks at 50ms for self-test */
    for (unsigned int tick = 1; tick <= 10; tick++) {
        uart_puts("[RTOS] self-test tick ");
        uart_putu(tick);
        uart_puts("\r\n");
        busy_ms(50);
    }
    uart_puts("[RTOS] Self-test complete — starting fuel ECU loop\r\n");

#ifdef VSE_ROGUE_DMA
    /* ── Rogue behaviour (attack scenario, compile-time gated) ─────────────
     * A genuinely misbehaving guest: program the virtual DMA controller to
     * target memory OUTSIDE this VM's Stage-2 map (IPA 0x50000000 is not in the
     * RTOS's [0, 0x0F000000) window). Each request is rejected by the
     * hypervisor's DMA guard and reported to the trust engine; the burst forms
     * the fault storm that drives detect -> enforce -> quarantine. This is the
     * REAL guest-driven replacement for the old fabricated fdetect loop that
     * used to live in boot/main.c.
     */
    uart_puts("[RTOS] !! ROGUE: issuing out-of-bounds DMA burst @IPA 0x50000000\r\n");
    for (unsigned int i = 0; i < 8u; i++) {
        hvc_call(HVC_DMA_XFER, 0x50000000UL + (unsigned long)i * 0x1000UL,
                 0x1000UL, 0xBAD0u + i, 0);
        busy_ms(20);
    }
    uart_puts("[RTOS] !! ROGUE: DMA burst complete\r\n");
#endif

    unsigned long slot = 1;
    unsigned long iter = 0;

    while (1) {
        /* 100ms work quantum — one engine management step */
        busy_ms(100);
        iter++;

        /* Update engine physics and drive cycle */
        update_engine();
        if (iter % 5 == 0) update_drive_cycle();

        unsigned int inj_us = calc_injection_us();

        /* Fire injectors for next cylinder in firing order */
        unsigned int cyl = FIRING_ORDER[iter % FUEL_NUM_CYLINDERS];
        (void)cyl;  /* in real hw: set GPIO to trigger injector solenoid */

        /* Every 500ms slot: report status + update hypervisor + yield */
        if (iter % 5 == 0) {
            uart_puts("[RTOS] ---- slot ");
            uart_putu(slot);
            uart_puts(" (500ms) ----\r\n");

            fuel_print_status(inj_us);
            fuel_hvc_update(inj_us);

            slot++;

            /* Yield to hypervisor — triggers context switch to Linux/Android */
            __asm__ volatile("wfi");
        }
    }
}
