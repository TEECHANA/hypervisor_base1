#!/usr/bin/env bash
#
# seal_wrapper.sh — host-side wrapper for the VSE sealing workflow (Audit #6).
#
# The hypervisor's seal subsystem (vse/seal.c) binds a sealed blob to the
# current platform configuration: seal_key = HMAC(master_key, label||config_tag).
# A blob unseals ONLY if the configuration HMAC is unchanged. This wrapper
# documents and drives the host side of that workflow: it (re)provisions the
# inputs the seal binds to and verifies a clean boot exercises seal_selftest.
#
# SCOPE: sealing provides INTEGRITY + CONFIG-BINDING, not confidentiality
# (payload is stored in cleartext — see vse/seal.c header). The seal key
# derives from the consolidated keystore (vse/keystore.c, audit #3); on QEMU
# that is the dev key, not a per-device fuse key.
#
# Usage:
#   scripts/seal_wrapper.sh build     # rebuild hypervisor
#   scripts/seal_wrapper.sh verify    # boot and confirm seal self-test PASSES
#   scripts/seal_wrapper.sh all       # build then verify (default)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

LOG="/tmp/seal_verify.log"

do_build() {
    echo "[seal_wrapper] building hypervisor..."
    make clean && make
}

do_verify() {
    echo "[seal_wrapper] booting to exercise seal self-test (Phase 4)..."
    rm -f "$LOG"
    # Run the boot in the background, capture to the log, hard-kill after window.
    ./scripts/run_qemu.sh > "$LOG" 2>&1 &
    local qpid=$!
    # Phase 4 runs early; 30s is ample. Then stop QEMU regardless.
    sleep 30
    pkill -9 -f qemu-system-aarch64 2>/dev/null || true
    kill -9 "$qpid" 2>/dev/null || true
    wait "$qpid" 2>/dev/null || true

    echo "[seal_wrapper] --- seal-related results ---"
    if ! grep -E "seal self-test|unseal (DENIED|rejected)|sealing service" "$LOG"; then
        echo "[seal_wrapper] ERROR: no seal output found in $LOG"
        echo "[seal_wrapper] (log had $(wc -l < "$LOG") lines; tail below)"
        tail -5 "$LOG"
        exit 1
    fi

    if grep -q "seal self-test PASSED" "$LOG"; then
        echo "[seal_wrapper] RESULT: PASS - seal round-trip ok, tamper correctly denied"
        exit 0
    else
        echo "[seal_wrapper] RESULT: FAIL - seal self-test did not pass"
        exit 1
    fi
}

case "${1:-all}" in
    build)  do_build ;;
    verify) do_verify ;;
    all)    do_build; do_verify ;;
    *)      echo "usage: $0 {build|verify|all}"; exit 2 ;;
esac
