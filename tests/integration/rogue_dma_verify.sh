#!/usr/bin/env bash
#
# rogue_dma_verify.sh — regression test for the ORGANIC rogue-DMA attack path.
#
# Proves that VM2 (the rtos guest, built with VSE_ROGUE_DMA) issues its OWN
# out-of-bounds HVC_DMA_XFER burst and that it flows through the genuine chain:
#
#     guest HVC_DMA_XFER  ->  dma_guard (S2 reject)  ->  dma_guard_log_violation
#         ->  fdetect_dma_violation  ->  trust_report_fault + ids_notify_fault
#         ->  IDS STORM detector  ->  IDS ENFORCE  ->  trust_quarantine(VM2)
#
# This is the REAL guest-driven path. The old synthetic -DVSE_IDS_STORM_DEMO
# injection (which fabricated fdetect_mem_fault() on VM3 at boot) has been
# retired — there is no longer any injected fault in the build. The test
# explicitly asserts that NO injected memory/permission fault is attributed to
# VM2 — every VM2 fault is a DMA-isolation violation (fault type 0x2)
# originating from the guest's own request.
#
# Usage:
#   bash tests/integration/rogue_dma_verify.sh
#   ROGUE_LOG=/path/to/existing_capture.log bash tests/integration/rogue_dma_verify.sh
#     (reuse a capture already produced by run_qemu_tests.sh — avoids re-booting)

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib_boot.sh
source "$HERE/lib_boot.sh"

LOG="${ROGUE_LOG:-}"
if [[ -z "$LOG" || ! -s "$LOG" ]]; then
    LOG="$(mktemp)"
    echo "Booting hypervisor + guests and driving login (organic rogue-DMA capture)..."
    boot_capture "$LOG" 200 || { echo "boot_capture failed (missing build/guests?)"; exit 2; }
fi

fails=0
chk()  { if grep -Eq -- "$1" "$LOG"; then echo "  PASS: $2"
         else echo "  FAIL: $2"; fails=$((fails + 1)); fi; }
chk0() { local n; n="$(grep -Ec -- "$1" "$LOG")"
         if [[ "$n" -eq 0 ]]; then echo "  PASS: $2"
         else echo "  FAIL: $2 (found $n)"; fails=$((fails + 1)); fi; }

echo "=== Organic rogue-DMA chain (VM2, no injection) ==="
# 0. Login was actually driven — otherwise the guests never run.
chk "Access granted"                                   "operator login succeeded (guests run)"
# 1. VM2 issues its OWN out-of-bounds DMA burst.
chk "\[RTOS\] !! ROGUE: issuing out-of-bounds DMA"     "VM2 rtos issues rogue DMA burst"
# 2. dma_guard rejects the OOB target (IPA 0x50000000 not in VM2's Stage-2 map).
chk "DMA: VM2 IPA=.*50000000.* (DENIED|not in S2)"     "dma_guard denies VM2 OOB target"
# 3. dma_guard_log_violation fires for VM2.
chk "DMA VIOLATION: stream=.* VM2 PA=.*50000000"       "dma_guard_log_violation(VM2)"
# 4. fdetect receives the violation.
chk "VSE Phase 5: VM2 DMA violation"                   "fdetect_dma_violation(VM2)"
# 5. trust engine records it as a DMA fault (type 0x2), not an injected mem fault.
chk "VSE Phase 3: VM2 fault type=.*0000000000000002"   "trust_report_fault VM2 DMA(0x2)"
# 6. IDS storm detector trips on VM2.
chk "VSE IDS: ALERT VM2 fault STORM"                   "IDS STORM detected on VM2"
# 7. IDS active enforcement quarantines VM2.
chk "VSE IDS: ENFORCING.*quarantining VM2"             "IDS ENFORCE -> quarantine VM2"
# 8. Trust engine transitions VM2 to QUARANTINE.
chk "VM2 trust DEGRADED -> QUARANTINE"                 "VM2 trust -> QUARANTINE"

echo "=== Organic, not synthetic (negative assertions on VM2) ==="
# The synthetic demo injects fdetect_mem_fault() -> "memory/PERMISSION fault".
# The organic path must NOT produce any such injected fault attributed to VM2.
chk0 "VM2 memory fault"                                "no injected memory fault on VM2"
chk0 "VM2 PERMISSION fault"                            "no injected permission fault on VM2"
chk0 "VM2 fault type=.*0000000000000001"              "no MEM-type(0x1) fault on VM2"

if [[ $fails -eq 0 ]]; then
    echo "PASS: organic rogue-DMA path verified end-to-end."
    exit 0
else
    echo "FAIL: $fails organic rogue-DMA check(s) failed. Log: $LOG"
    exit 1
fi
