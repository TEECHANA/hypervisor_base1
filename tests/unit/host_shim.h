/*
 * host_shim.h — unit-test host shim (test harness only; hypervisor unchanged).
 *
 * The freestanding hypervisor headers (include/types.h) typedef size_t,
 * uintptr_t and intptr_t to u64 (== unsigned long long). The unit tests are
 * built with the host toolchain WITHOUT -nostdinc, so they also pull in the
 * host <stddef.h>/<stdint.h>, where size_t is unsigned long. Same width on a
 * 64-bit host, but a DISTINCT type — so the two typedefs conflict and the
 * compile fails ("conflicting types for 'size_t'").
 *
 * Fix, entirely in the harness: include the host standard headers FIRST so
 * their size_t/uintptr_t/intptr_t are established, then rename the
 * hypervisor's copies aside while include/types.h is pulled in. types.h is
 * include-guarded, so every later `#include "types.h"` in a test or in the
 * hypervisor source under test is a no-op and inherits the host types. The
 * hypervisor tree itself is not modified.
 *
 * Wired in via `gcc -include tests/unit/host_shim.h ...` in run_tests.sh.
 */
#ifndef HYP_TEST_HOST_SHIM_H
#define HYP_TEST_HOST_SHIM_H

#include <stddef.h>   /* host size_t              */
#include <stdint.h>   /* host uintptr_t/intptr_t  */

#define size_t     hyp_size_t_shimmed_
#define uintptr_t  hyp_uintptr_t_shimmed_
#define intptr_t   hyp_intptr_t_shimmed_
#include "../../include/types.h"
#undef size_t
#undef uintptr_t
#undef intptr_t

#endif /* HYP_TEST_HOST_SHIM_H */
