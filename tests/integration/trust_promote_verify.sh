#!/usr/bin/env bash
#
# trust_promote_verify.sh — runtime proof of GAP E auto-promotion.
#
# No organic guest goes "DEGRADED then quiet" (VM2 keeps issuing rogue DMA after
# recovery), so this builds a self-test image that exercises the real path
# deterministically during boot (after IDS init, before login):
#   -DTRUST_PROMOTE_SELFTEST   drive VM3 to DEGRADED, spin out the clean period,
#                              then call the real ids_poll() -> auto_promote_tick
#   -DTRUST_CLEAN_PERIOD_US=.. shorten the clean period so it elapses in-run
#   -DVSE_COMPONENTS_LEARN     the injected code shifts .text/.rodata, so run
#                              Phase 2 in its existing (non-fatal) learn mode
#
# It boots with the three guests (VM3 must pass genuineness to be TRUSTED first)
# and asserts the log shows VM3 going TRUSTED -> DEGRADED and then auto-promoted
# DEGRADED -> TRUSTED. No login/scheduler needed — the promotion runs at boot.
# Built into an isolated BUILD_DIR; the normal build/qemu is untouched.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

CLEAN_US=2000000   # 2 s clean period (spun out against the real counter)

BDIR="$(mktemp -d)"
LOG="$(mktemp)"
trap 'rm -rf "$BDIR" "$LOG"' EXIT

echo "=== trust auto-promote (DEGRADED -> TRUSTED) runtime test ==="
echo "Building self-test image in isolated dir..."
if ! make -C "$ROOT" qemu BUILD_DIR="$BDIR" \
        EXTRA_CFLAGS="-DTRUST_PROMOTE_SELFTEST -DVSE_COMPONENTS_LEARN -DTRUST_CLEAN_PERIOD_US=${CLEAN_US}ull" \
        >"$BDIR/build.log" 2>&1; then
    echo "  FAIL: self-test build failed"; tail -20 "$BDIR/build.log"; exit 1
fi
ELF="$BDIR/hypervisor.elf"

# Guests (rtos/android built from source; linux is a prebuilt blob). VM3 must
# pass genuineness in trust_init to be TRUSTED before the self-test downgrades it.
make -C "$ROOT" guest-rtos guest-android >/dev/null 2>&1
LINUX="$ROOT/guests/linux/Image"; INITRD="$ROOT/guests/linux/initramfs.cpio.gz"
RTOS="$ROOT/guests/rtos/rtos.bin"; ANDROID="$ROOT/guests/android_stub/android.bin"
for g in "$LINUX" "$INITRD" "$RTOS" "$ANDROID"; do
    [[ -f "$g" ]] || { echo "  FAIL: missing guest image $g"; exit 1; }
done

echo "Booting with guests (promotion runs at boot, clean period ${CLEAN_US} us)..."
timeout 40 qemu-system-aarch64 \
    -machine virt,gic-version=3,virtualization=on,iommu=smmuv3 \
    -cpu cortex-a57 -m 2G -smp 4 -nographic -serial mon:stdio -no-reboot -d guest_errors \
    -kernel "$ELF" \
    -device loader,file="$LINUX",addr=0x41200000,force-raw=on \
    -device loader,file="$INITRD",addr=0x47000000,force-raw=on \
    -device loader,file="$RTOS",addr=0x60008000,force-raw=on \
    -device loader,file="$RTOS",addr=0x90000000,force-raw=on \
    -device loader,file="$ANDROID",addr=0x70200000,force-raw=on \
    >"$LOG" 2>&1 &
qemu=$!
for _ in $(seq 1 40); do
    kill -0 "$qemu" 2>/dev/null || break
    grep -q "VM3 trust DEGRADED -> TRUSTED (clean period elapsed)" "$LOG" && break
    sleep 1
done
kill "$qemu" 2>/dev/null; wait "$qemu" 2>/dev/null

fails=0
if grep -qF "TRUST-PROMOTE-SELFTEST: injecting" "$LOG"; then
    echo "  PASS: self-test injected faults into VM3"
else echo "  FAIL: injection never ran (define not compiled?)"; fails=$((fails+1)); fi

if grep -qE "VM3 trust TRUSTED -> DEGRADED" "$LOG"; then
    echo "  PASS: VM3 downgraded to DEGRADED by the injected faults"
else echo "  FAIL: VM3 never reached DEGRADED"; fails=$((fails+1)); fi

if grep -qF "VM3 trust DEGRADED -> TRUSTED (clean period elapsed)" "$LOG"; then
    echo "  PASS: VM3 auto-promoted DEGRADED -> TRUSTED after clean period"
else echo "  FAIL: VM3 was not auto-promoted"; fails=$((fails+1)); fi

if [[ $fails -eq 0 ]]; then
    echo "Auto-promotion verified at runtime."
    exit 0
else
    echo "$fails check(s) FAILED."
    echo "---- log tail ----"; tail -30 "$LOG"
    exit 1
fi
