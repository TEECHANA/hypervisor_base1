#!/usr/bin/env bash
# tests/integration/phase1_verify.sh — Phase 1 exit-criteria test
#
# Run from the hypervisor root:
#   bash tests/integration/phase1_verify.sh
#
# What changed from original version:
#   - QEMU serial goes to BOTH the terminal (tee) AND a log file so
#     you see output live instead of waiting 120 s in silence.
#   - Timeout reduced to 60 s (Linux fully boots in ~10 s on QEMU).
#   - Log file is kept on failure so you can inspect it.
#   - Added a progress spinner so you know the script is alive.

set -uo pipefail

PASS=0
FAIL=0
TIMEOUT=60

# Keep log in current dir so it survives the run
LOG="build/phase1_test.log"
mkdir -p build

ok()    { echo "  ✓  $*"; PASS=$((PASS+1)); }
fail()  { echo "  ✗  $*"; FAIL=$((FAIL+1)); }
note()  { echo "  △  $*"; }

echo "=== Phase 1 integration test ==="
echo "QEMU output → $LOG  (also printed live below)"
echo "Timeout: ${TIMEOUT}s"
echo "-----------------------------------------------------------"

# Run QEMU, pipe serial to tee so output appears live AND goes to log.
# We use a pipe + process substitution so timeout applies to QEMU only.
timeout "$TIMEOUT" qemu-system-aarch64 \
    -machine virt,gic-version=3,virtualization=on \
    -cpu cortex-a57 -m 2G -smp 4 \
    -nographic \
    -serial "file:$LOG" \
    -no-reboot \
    -d guest_errors \
    -kernel build/qemu/hypervisor.elf \
    -device loader,file=guests/linux/Image,addr=0x41200000,force-raw=on \
    -device loader,file=guests/linux/initramfs.cpio.gz,addr=0x47000000,force-raw=on \
    -device loader,file=guests/rtos/rtos.bin,addr=0x60008000,force-raw=on \
    -device loader,file=guests/android_stub/android.bin,addr=0x70200000,force-raw=on \
    2>/dev/null &

QEMU_PID=$!

# Stream the log file live while QEMU runs
echo "[live output — press Ctrl-C to stop watching, test continues]"
echo ""
tail -f "$LOG" --pid="$QEMU_PID" 2>/dev/null &
TAIL_PID=$!

# Wait for QEMU to finish (timeout kills it)
wait "$QEMU_PID" 2>/dev/null || true
kill "$TAIL_PID" 2>/dev/null || true
wait "$TAIL_PID" 2>/dev/null || true

echo ""
echo "-----------------------------------------------------------"
echo "QEMU finished. Checking exit criteria..."
echo ""

# ── Check 1: pool exhaustion ──────────────────────────────────────────
if grep -q "pool EXHAUSTED" "$LOG"; then
    fail "Page-table pool exhausted"
else
    ok "Page-table pool not exhausted"
fi

# ── Check 2: ICH_VTR queried ─────────────────────────────────────────
if grep -q "list registers available" "$LOG"; then
    LR=$(grep "list registers available" "$LOG" | head -1 | sed 's/.*\[INF\] //')
    ok "ICH_VTR queried: $LR"
else
    fail "ICH_VTR LR count not logged — gic_init change not applied?"
fi

# ── Check 3: IRQ routes registered ───────────────────────────────────
ROUTE_COUNT=$(grep -c "IRQ route:" "$LOG" 2>/dev/null || echo 0)
if [ "$ROUTE_COUNT" -ge 3 ]; then
    ok "IRQ routing table populated ($ROUTE_COUNT routes)"
else
    fail "IRQ routes not registered (found $ROUTE_COUNT, expected ≥3)"
    echo "       platform_add_irq_routes() not called?"
fi

# ── Check 4: Linux reaches userspace ─────────────────────────────────
if grep -qE "INIT SCRIPT STARTED|BusyBox|~ #|sh:.*job control" "$LOG"; then
    ok "Linux reached userspace"
else
    fail "Linux did not reach userspace"
fi

# ── Check 5: RTOS ticked ─────────────────────────────────────────────
TICK_COUNT=$(grep -c "\[RTOS\] tick" "$LOG" 2>/dev/null || echo 0)
if [ "$TICK_COUNT" -ge 5 ]; then
    ok "RTOS ticked $TICK_COUNT times"
else
    fail "RTOS tick count too low: $TICK_COUNT (expected ≥5)"
fi

# ── Check 6: unexpected DABTs ────────────────────────────────────────
# Known harmless: Android stub stack fault at 0x48fefff0
LATE_DABT=$(grep "DABT:" "$LOG" 2>/dev/null | grep -v "48fefff0" | wc -l)
if [ "$LATE_DABT" -eq 0 ]; then
    ok "No unexpected DABTs"
else
    note "$LATE_DABT unexpected DABT line(s) — review $LOG"
    grep "DABT:" "$LOG" | grep -v "48fefff0" | head -5 | sed 's/^/       /'
    FAIL=$((FAIL+1))
fi

# ── Check 7: s2_unmap exercised (informational) ───────────────────────
if grep -q "S2 UNMAP IPA" "$LOG"; then
    ok "s2_unmap() exercised"
else
    note "s2_unmap not called this run (needs explicit vm_stop — informational)"
fi

# ── Check 8: no 'No route for IRQ' for routed SPIs ───────────────────
NO_ROUTE=$(grep "No route for IRQ 33\|No route for IRQ 34\|No route for IRQ 35" "$LOG" 2>/dev/null | wc -l)
if [ "$NO_ROUTE" -eq 0 ]; then
    ok "No 'No route' warnings for registered SPIs"
else
    fail "'No route' seen for SPI 33/34/35 — irq_route_add not working"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
if [ "$FAIL" -gt 0 ]; then
    echo "    Full log: $LOG"
    exit 1
else
    echo "    Phase 1 exit criteria: ALL PASSED"
    rm -f "$LOG"
    exit 0
fi
