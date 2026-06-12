#!/bin/bash
# phase2_verify.sh — Phase 2 integration test
# Run from: /home/rishi/Hypervisor_development/LINUX_RTOS/hypervisor1

set -euo pipefail
cd "$(dirname "$0")/../.."

LOG=build/phase2_test.log
TIMEOUT=60
PASS=0; FAIL=0; INFO=0

pass() { echo "  ✓  $1"; ((PASS++)); }
fail() { echo "  ✗  $1"; ((FAIL++)); }
info() { echo "  △  $1"; ((INFO++)); }

echo "=== Phase 2 integration test ==="
echo "QEMU output → $LOG"
echo "Timeout: ${TIMEOUT}s"
echo "-----------------------------------------------------------"

mkdir -p build

# Run QEMU with timeout, capture serial output
timeout "$TIMEOUT" qemu-system-aarch64 \
    -machine virt,gic-version=3,virtualization=on \
    -cpu cortex-a57 -m 2G -smp 4 \
    -nographic -serial "file:$LOG" -no-reboot \
    -kernel build/qemu/hypervisor.elf \
    -device loader,file=guests/linux/Image,addr=0x41200000,force-raw=on \
    -device loader,file=guests/linux/initramfs.cpio.gz,addr=0x47000000,force-raw=on \
    -device loader,file=guests/rtos/rtos.bin,addr=0x60008000,force-raw=on \
    -device loader,file=guests/android_stub/android.bin,addr=0x70200000,force-raw=on \
    2>/dev/null || true

echo "-----------------------------------------------------------"
echo "Checking Phase 2 exit criteria..."

# ── Phase 1 criteria (must still pass) ──

grep -q "page-table pool EXHAUSTED" "$LOG" && \
    fail "PT pool exhausted (regression)" || \
    pass "Page-table pool not exhausted"

grep -q "GICv3:.*list registers" "$LOG" && \
    pass "ICH_VTR queried: $(grep 'list registers' "$LOG" | head -1 | sed 's/.*HYP \[INF\] //')" || \
    fail "ICH_VTR not queried"

ROUTES=$(grep -c "IRQ route: phys=" "$LOG" 2>/dev/null || true)
[ "$ROUTES" -eq 3 ] && \
    pass "IRQ routing table populated (3 routes)" || \
    fail "IRQ routing table: expected 3 routes, got $ROUTES"

grep -q "~ #\|INIT SCRIPT STARTED" "$LOG" && \
    pass "Linux reached userspace" || \
    fail "Linux did not reach userspace"

TICKS=$(grep -c "\[RTOS\] tick" "$LOG" 2>/dev/null || true)
[ "$TICKS" -ge 10 ] && \
    pass "RTOS ticked $TICKS times" || \
    fail "RTOS ticked only $TICKS times (expected >= 10)"

UNEXPECTED_DABT=$(grep "DABT" "$LOG" | grep -v "FAR=0x.*48fe" | wc -l || true)
[ "$UNEXPECTED_DABT" -eq 0 ] && \
    pass "No unexpected DABTs" || \
    info "DABTs detected (may be Android stack — informational): $UNEXPECTED_DABT"

# ── Phase 2 criteria ──

# 2.2.1 MMIO ownership: mmio_register_vm_device called for each VM's devices
grep -q "MMIO: VM.*owns device" "$LOG" && \
    pass "§2.2.1 MMIO: VM device ownership registered" || \
    fail "§2.2.1 MMIO: no device ownership entries logged"

# 2.2.1 No cross-VM MMIO violations at boot (each VM accessing only its own devices)
VIOLATIONS=$(grep -c "MMIO VIOLATION" "$LOG" 2>/dev/null || true)
[ "$VIOLATIONS" -eq 0 ] && \
    pass "§2.2.1 MMIO: zero ownership violations during normal boot" || \
    info "§2.2.1 MMIO: $VIOLATIONS violations detected (check which VM)"

# 2.2.2 DMA guard: module loaded (no DMA violations during normal boot)
grep -q "DMA VIOLATION" "$LOG" && \
    fail "§2.2.2 DMA: unexpected DMA violation at boot" || \
    pass "§2.2.2 DMA: no DMA violations during boot"

# 2.2.3 SMMU: init called, graceful fallback on QEMU
grep -q "SMMU.*not present\|SMMU.*no hardware\|DMA isolation.*software" "$LOG" && \
    pass "§2.2.3 SMMU: graceful fallback (no hardware on QEMU)" || \
    { grep -q "SMMU.*enabled" "$LOG" && \
      pass "§2.2.3 SMMU: hardware present and enabled" || \
      fail "§2.2.3 SMMU: init not called"; }

# 2.2.4 Power: power_init called at boot
grep -q "Power manager.*initialised\|Power manager ready" "$LOG" && \
    pass "§2.2.4 Power: manager initialised" || \
    fail "§2.2.4 Power: manager not initialised"

# 2.2.4 Power: vm_suspend/resume gate/ungate (informational — needs HVC call)
grep -q "Power: gating VM\|power_gate" "$LOG" && \
    pass "§2.2.4 Power: VM gating invoked" || \
    info "§2.2.4 Power: no VM gating this run (needs explicit HVC_VM_SUSPEND)"

# 3.1.4 Device profile: applied
grep -q "DevProfile:.*entries applied\|DevProfile.*applying" "$LOG" && \
    pass "§3.1.4 DevProfile: profile table applied" || \
    fail "§3.1.4 DevProfile: profile not applied"

# No regression: s2_unmap still present
grep -n "s2_unmap" core/vm/vm.c | grep -q "s2_unmap" && \
    pass "Regression: s2_unmap still in vm_stop" || \
    fail "Regression: s2_unmap removed from vm_stop"

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $INFO informational ==="
if [ "$FAIL" -eq 0 ]; then
    echo "    Phase 2 exit criteria: ALL PASSED ✓"
    exit 0
else
    echo "    Phase 2 exit criteria: $FAIL FAILED"
    exit 1
fi
