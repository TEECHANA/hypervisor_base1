/*
 * rtc.h — ARM Generic Timer-based soft RTC for TOTP (RFC 6238)
 *
 * There is no hardware RTC on QEMU virt or RPi4 in this configuration.
 * We synthesize wall-clock time as:
 *
 *   unix_now = RTC_PROVISIONED_EPOCH + (CNTPCT_EL0 - boot_cnt) / CNTFRQ_EL0
 *
 * RTC_PROVISIONED_EPOCH is the Unix timestamp representing "time at power-on."
 * On a real device this would be read from fuses or written by a provisioning
 * tool; here it is a build-time constant. Override with -DRTC_PROVISIONED_EPOCH=N.
 *
 * Accuracy: ±1 second (limited by integer division of the tick counter).
 * Drift: CNTFRQ_EL0 is read once at rtc_init(); any hardware frequency error
 * accumulates but is negligible over the seconds-to-minutes lifetime of a boot.
 */

#ifndef LIB_RTC_H
#define LIB_RTC_H

#include "../../include/types.h"

/*
 * Provisioned Unix epoch — seconds since 1970-01-01 00:00:00 UTC.
 * Default: 2025-07-01 00:00:00 UTC = 1751328000.
 * For deployment: inject the actual manufacture / provisioning timestamp.
 */
#ifndef RTC_PROVISIONED_EPOCH
#define RTC_PROVISIONED_EPOCH  1751328000ULL
#endif

/*
 * rtc_init — capture the boot-time tick counter and clock frequency.
 * Call once, before any rtc_unix_now() call. No prerequisites.
 */
void rtc_init(void);

/*
 * rtc_unix_now — return the current synthesized Unix time in whole seconds.
 * Monotonically non-decreasing after rtc_init().
 */
u64  rtc_unix_now(void);

#endif /* LIB_RTC_H */
