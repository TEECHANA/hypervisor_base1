/*
 * android_main.c — Android guest audio workload (Phase 5C §5.4)
 *
 * Replaces the former boot-only stub with a real (simulated) Android audio HAL
 * workload. It models a PCM playback pipeline — 44.1 kHz / stereo / 16-bit — and
 * drives the hypervisor's audio HVC ABI so section 5.4 is actually exercised:
 *
 *   HVC_AUDIO_PLAY   (0x0071) — start/resume playback of a track
 *   HVC_AUDIO_STATUS (0x0070) — periodic buffer report: frames played/dropped,
 *                               output latency (ms)
 *   HVC_AUDIO_STOP   (0x0072) — stop playback between tracks
 *
 * The hypervisor stores the latest report in g_audio and surfaces it on context
 * switches ("[AUDIO->HYP] frames=... dropped=... lat=...ms playing"), analogous
 * to how the RTOS drives the fuel/OBD path. A buffer-underrun model drops frames
 * when a playback deadline is missed, so the reported stream is not constant.
 *
 * Entry: _android_start (start.S) -> android_main(dtb_pa).
 */

typedef unsigned long long u64;
typedef unsigned int       u32;

#define UART0_BASE        0x09000000UL

#define HVC_AUDIO_STATUS  0x0070u
#define HVC_AUDIO_PLAY    0x0071u
#define HVC_AUDIO_STOP    0x0072u

/* ── UART (PL011 data register) ── */
static inline void uart_putc(char c)
{
    volatile unsigned int *dr = (volatile unsigned int *)UART0_BASE;
    *dr = (unsigned int)c;
}
static void uart_puts(const char *s)
{
    while (*s) { if (*s == '\n') uart_putc('\r'); uart_putc(*s++); }
}
static void uart_putu(unsigned long v)
{
    char buf[20]; int i = 0;
    if (!v) { uart_putc('0'); return; }
    while (v) { buf[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (i--) uart_putc(buf[i]);
}

/* ── HVC helpers ── */
static void hvc0(u64 id)
{
    register u64 x0 asm("x0") = id;
    asm volatile("hvc #0" : "+r"(x0) :: "x1", "x2", "x3", "memory");
}
static void hvc3(u64 id, u64 a1, u64 a2, u64 a3)
{
    register u64 x0 asm("x0") = id;
    register u64 x1 asm("x1") = a1;
    register u64 x2 asm("x2") = a2;
    register u64 x3 asm("x3") = a3;
    asm volatile("hvc #0"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
}

/* ── Timer-based busy wait ── */
static inline u64 read_cntpct(void)
{
    u64 v; asm volatile("mrs %0, cntpct_el0" : "=r"(v)); return v;
}
static inline u64 read_cntfrq(void)
{
    u64 v; asm volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v;
}
static void busy_ms(unsigned int ms)
{
    u64 f = read_cntfrq();
    u64 t = (f / 1000UL) * (u64)ms;
    u64 s = read_cntpct();
    while ((read_cntpct() - s) < t) asm volatile("nop");
}

/* ── Audio pipeline model ──
 * 44.1 kHz stereo 16-bit. One 100 ms slice = 4410 frames. A "track" is a fixed
 * number of 100 ms slices; between tracks we STOP (pause), then PLAY the next.
 */
#define FRAMES_PER_100MS   4410u
#define SLICES_PER_TRACK   30u        /* ~3 s per track                      */
#define REPORT_EVERY       2u         /* HVC_AUDIO_STATUS cadence (200 ms)   */

/* A tiny fixed playlist just to give the log some variety. */
static const char *const TRACKS[] = {
    "01 - startup_chime.pcm",
    "02 - navigation_voice.pcm",
    "03 - media_playback.pcm",
};
#define NUM_TRACKS (sizeof(TRACKS) / sizeof(TRACKS[0]))

void android_main(u64 dtb_pa)
{
    (void)dtb_pa;

    uart_puts("\r\n[ANDROID] Guest boot: Android audio VM\r\n");
    uart_puts("[ANDROID] Audio HAL: PCM 44100Hz stereo 16-bit\r\n");
    uart_puts("[ANDROID] Android VM booted successfully\r\n");

    unsigned long frames_played  = 0;
    unsigned long frames_dropped = 0;
    unsigned long track          = 0;

    while (1) {
        const char *name = TRACKS[track % NUM_TRACKS];

        uart_puts("[AUDIO] >> PLAY track ");
        uart_puts(name);
        uart_puts("\r\n");
        hvc0(HVC_AUDIO_PLAY);                 /* mark playback active */

        for (unsigned int slice = 1; slice <= SLICES_PER_TRACK; slice++) {
            busy_ms(100);                     /* render one 100 ms buffer */
            frames_played += FRAMES_PER_100MS;

            /*
             * Buffer-underrun model: if a render deadline is missed (here, a
             * periodic "load spike" every 7th slice), the output ring starves
             * and we drop a partial buffer. Output latency tracks buffer depth
             * and jitters a little around a 12 ms target.
             */
            unsigned int latency_ms = 12u;
            if ((slice % 7u) == 0u) {
                frames_dropped += 128u;       /* underrun: dropped frames */
                latency_ms = 18u;             /* buffer refill raises latency */
            } else if ((slice % 3u) == 0u) {
                latency_ms = 10u;
            }

            if ((slice % REPORT_EVERY) == 0u)
                hvc3(HVC_AUDIO_STATUS, frames_played, frames_dropped, latency_ms);

            if ((slice % 10u) == 0u) {
                uart_puts("[AUDIO] frames=");
                uart_putu(frames_played);
                uart_puts(" dropped=");
                uart_putu(frames_dropped);
                uart_puts(" lat=");
                uart_putu(latency_ms);
                uart_puts("ms\r\n");
            }

            asm volatile("wfi");              /* yield remaining slice */
        }

        uart_puts("[AUDIO] << STOP (track complete)\r\n");
        hvc0(HVC_AUDIO_STOP);                 /* pause between tracks */
        busy_ms(200);                         /* inter-track gap */
        track++;
    }
}
