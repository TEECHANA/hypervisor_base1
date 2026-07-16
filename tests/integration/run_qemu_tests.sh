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
# The synthetic -DVSE_IDS_STORM_DEMO injection (VM3, boot-time) has been retired;
# the storm/enforcement here is entirely organic, driven by VM2's own DMA burst.

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

echo "=== Guest liveness / x1-trapframe regression ==="
# Regression guard for the entry.S guest-x1 save bug: if the guest's x1 were
# clobbered by ESR before SAVE_GUEST_REGS, HVC arg1 (fuel rpm) would arrive as
# garbage/zero. A sane non-zero rpm proves x1 is preserved across the trap.
# (Salvaged from the retired fix/fuel-x1-trapframe-and-warnings branch.)
if grep -qE "FUEL HVC: rpm=[1-9][0-9]*" "$LOG"; then echo "  PASS: RTOS fuel rpm non-zero (x1 preserved)"
  else echo "  FAIL: RTOS fuel rpm non-zero (possible x1-trapframe regression)"; fails=$((fails + 1)); fi
# Scheduler is actually time-slicing between guests.
if grep -qE "CTX\[[0-9]+\]:" "$LOG"; then echo "  PASS: scheduler time-slicing (CTX switches)"
  else echo "  FAIL: scheduler time-slicing (no CTX switches)"; fails=$((fails + 1)); fi

echo "=== Organic rogue-DMA path (VM2, guest runtime) ==="
# Reuse this capture — do not boot a second time.
if ROGUE_LOG="$LOG" bash "$HERE/rogue_dma_verify.sh"; then
    echo "  PASS: organic rogue-DMA regression"
else
    echo "  FAIL: organic rogue-DMA regression"
    fails=$((fails + 1))
fi

echo "=== Phase 6 failover: organic quarantine -> real backup restore ==="
# The organic VM2 quarantine (above) must drive trust_quarantine ->
# failover_on_quarantine -> _do_failover: stop, restore from the DISTINCT backup
# region via real memcpy, re-finalize, restart. Each step is checked on the same
# capture so a regression in the quarantine->failover wiring is caught.
if grep -qE "quarantine of critical VM2 — invoking failover" "$LOG"; then echo "  PASS: failover invoked on VM2 quarantine"
  else echo "  FAIL: failover not invoked on VM2 quarantine"; fails=$((fails + 1)); fi
if grep -qE "FAILOVER VM2 'rtos' attempt [1-9]" "$LOG"; then echo "  PASS: failover_trigger fired (restart from backup OS)"
  else echo "  FAIL: failover_trigger did not fire"; fails=$((fails + 1)); fi
# Real restore: a distinct-region memcpy must run, NOT the same-image stub.
if grep -qE "VM2 restored [1-9][0-9]* bytes from backup 0x[0-9a-fx]+ -> live 0x[0-9a-fx]+" "$LOG"; then echo "  PASS: real backup restore (distinct-region memcpy)"
  else echo "  FAIL: no real backup restore observed"; fails=$((fails + 1)); fi
if grep -q "no distinct backup region" "$LOG"; then echo "  FAIL: restore fell back to same-image stub (not a real failover)"; fails=$((fails + 1))
  else echo "  PASS: not a same-image stub"; fi
if grep -qE "VSE Phase 6: VM2 'rtos' recovered via backup OS" "$LOG"; then echo "  PASS: VM2 recovered + restarted (system continues)"
  else echo "  FAIL: VM2 did not recover via backup OS"; fails=$((fails + 1)); fi

echo "=== Console restored after recovery (VM2 rtos-uart passthrough) ==="
# VM2's rtos-uart is a 1:1 passthrough of the real console UART (0x09000000).
# After failover restores VM2's Stage-2 mappings, the restarted RTOS must write
# to the console again — i.e. raw guest lines ([RTOS]/[FUEL], no HYP prefix) must
# appear AFTER the recovery marker in this same capture. This guards against a
# failover/S2-remap change silently dropping the recovered guest's console.
if awk '/recovered via backup OS/{r=1; next}
        r && /^\[(RTOS|FUEL)\]/{found=1}
        END{exit !found}' "$LOG"; then
    echo "  PASS: RTOS console output resumes after recovery"
else
    echo "  FAIL: no RTOS console output after recovery (console not restored)"
    fails=$((fails + 1))
fi

echo "=== .rodata Stage-1 write-protection (Audit #7b regression) ==="
# Separate build+boot: needs a -DRODATA_WP_SELFTEST image in an isolated dir, so
# it does NOT reuse the capture above. Proves a store to .rodata faults at EL2.
if bash "$HERE/rodata_wp_verify.sh"; then
    echo "  PASS: .rodata write-protection regression"
else
    echo "  FAIL: .rodata write-protection regression"
    fails=$((fails + 1))
fi

echo "=== Trust auto-promotion (DEGRADED -> TRUSTED after clean period) ==="
# Separate build+boot: a self-test image drives VM3 to DEGRADED then observes the
# real ids_poll -> trust_auto_promote_tick promote it once the clean period lapses.
if bash "$HERE/trust_promote_verify.sh"; then
    echo "  PASS: trust auto-promotion regression"
else
    echo "  FAIL: trust auto-promotion regression"
    fails=$((fails + 1))
fi

echo "=== Operator password provisioning (new password works, changeme fails) ==="
# Separate build+boot: provisions a non-default password, then proves login
# succeeds with it and is denied with the "changeme" dev password. Restores the
# committed fail-closed pw_verifier.h afterward.
if bash "$HERE/password_provision_verify.sh"; then
    echo "  PASS: password provisioning regression"
else
    echo "  FAIL: password provisioning regression"
    fails=$((fails + 1))
fi

if [[ $fails -eq 0 ]]; then
    echo "All integration checks passed."
else
    echo "$fails integration check(s) FAILED."
    exit 1
fi
