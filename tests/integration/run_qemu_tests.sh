#!/usr/bin/env bash
#
# Integration test (run with: make test-integration).
#
# Boots the hypervisor in QEMU WITH the three guest images loaded, drives the
# operator 2FA login, and verifies the end-to-end security path — now all the
# way THROUGH login into guest runtime:
#   - configuration integrity check (Phase 1)
#   - component self-measurement / root of trust (Phase 2, .text + .rodata)
#   - creation + attestation + trust of all three guests (linux, rtos, android)
#   - operator login (password + TOTP) reached and passed
#   - the ORGANIC rogue-DMA attack: VM2 (rtos, VSE_ROGUE_DMA) issues its own
#     out-of-bounds HVC_DMA_XFER burst that drives the genuine
#     dma_guard -> fdetect -> IDS -> quarantine chain (delegated to
#     rogue_dma_verify.sh, run against the SAME capture to avoid re-booting).
#
# The synthetic -DVSE_IDS_STORM_DEMO injection (VM3, boot-time) is left intact
# and is independent of the organic VM2 path checked here.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib_boot.sh
source "$HERE/lib_boot.sh"

LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

echo "Booting hypervisor + 3 guests in QEMU and driving login..."
boot_capture "$LOG" 200 || { echo "boot_capture failed (build the hypervisor + guests first)"; exit 1; }

fails=0
chk(){ if grep -qF -- "$1" "$LOG"; then echo "  PASS: $1"
       else echo "  FAIL: $1"; fails=$((fails + 1)); fi; }

echo "=== Boot / attestation / trust (pre-login) ==="
chk "Tessolve Hypervisor v1.0"
chk "VSE Phase 1: configuration verified"
chk "VSE Phase 2: all components verified"
chk "VM 1 'linux' created"
chk "VM 2 'rtos' created"
chk "VM 3 'android' created"
chk "guest 'linux' genuineness VERIFIED"
chk "guest 'rtos' genuineness VERIFIED"
chk "guest 'android' genuineness VERIFIED"
chk "VM1 'linux' -> TRUSTED"
chk "VM2 'rtos' -> TRUSTED"
chk "VM3 'android' -> TRUSTED"
chk "Operator Login"
chk "Access granted."

echo "=== Organic rogue-DMA path (VM2, guest runtime) ==="
# Reuse this capture — do not boot a second time.
if ROGUE_LOG="$LOG" bash "$HERE/rogue_dma_verify.sh"; then
    echo "  PASS: organic rogue-DMA regression"
else
    echo "  FAIL: organic rogue-DMA regression"
    fails=$((fails + 1))
fi

if [[ $fails -eq 0 ]]; then
    echo "All integration checks passed."
else
    echo "$fails integration check(s) FAILED."
    exit 1
fi
