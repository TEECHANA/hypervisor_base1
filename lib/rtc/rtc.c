/*
 * rtc.c — ARM Generic Timer-based soft RTC (see rtc.h)
 */

#include "rtc.h"

static inline u64 _cntpct(void)
{
    u64 v;
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(v) :: "memory");
    return v;
}

static inline u64 _cntfrq(void)
{
    u64 v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static u64 _epoch_s;    /* provisioned Unix time at boot */
static u64 _boot_cnt;   /* CNTPCT_EL0 captured at rtc_init() */
static u64 _freq;       /* CNTFRQ_EL0 — ticks per second */

void rtc_init(void)
{
    _epoch_s  = RTC_PROVISIONED_EPOCH;
    _boot_cnt = _cntpct();
    _freq     = _cntfrq();
    if (_freq == 0)
        _freq = 62500000ULL;  /* QEMU virt cortex-a57 default */
}

u64 rtc_unix_now(void)
{
    u64 elapsed_ticks = _cntpct() - _boot_cnt;
    return _epoch_s + elapsed_ticks / _freq;
}
