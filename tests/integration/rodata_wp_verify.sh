#!/usr/bin/env bash
#
# rodata_wp_verify.sh — regression test for .rodata Stage-1 write-protection.
#
# Audit #7b (commit c0a0858) maps .rodata read-only at EL2 (mmu.S L2[1],
# AP[2]=1). That protection had never been exercised — no test wrote to
# .rodata, so a future mmu.S / linker change could silently drop it.
#
# This test builds a hypervisor with -DRODATA_WP_SELFTEST (a guarded probe in
# boot/main.c that stores to .rodata early in hyp_main), boots it hypervisor-
# only, and asserts the store faults: the boot must reach the probe, panic, and
# NEVER print the "store returned" line. If .rodata were writable the store
# would succeed and that line would appear -> FAIL.
#
# The special build goes into an isolated BUILD_DIR so the normal build/qemu
# artifacts are untouched.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

BDIR="$(mktemp -d)"
LOG="$(mktemp)"
trap 'rm -rf "$BDIR" "$LOG"' EXIT

echo "=== .rodata write-protection regression test ==="
echo "Building self-test image (-DRODATA_WP_SELFTEST) in isolated dir..."
if ! make -C "$ROOT" qemu BUILD_DIR="$BDIR" EXTRA_CFLAGS=-DRODATA_WP_SELFTEST \
        >"$BDIR/build.log" 2>&1; then
    echo "  FAIL: self-test build failed"; tail -20 "$BDIR/build.log"; exit 1
fi

ELF="$BDIR/hypervisor.elf"
[[ -f "$ELF" ]] || { echo "  FAIL: no ELF produced"; exit 1; }

echo "Booting hypervisor-only and capturing serial..."
# The probe runs right after the boot banner, before any VM is created, so no
# guest images are needed. Bounded run; the probe panics almost immediately.
timeout 25 qemu-system-aarch64 \
    -machine virt,gic-version=3,virtualization=on,iommu=smmuv3 \
    -cpu cortex-a57 -m 2G -smp 4 -nographic -serial mon:stdio -no-reboot \
    -d guest_errors -kernel "$ELF" >"$LOG" 2>&1 || true

fails=0

# 1. The probe must actually have run (guards against a build that silently
#    dropped the define, which would make the test vacuously "pass").
if grep -qF "RODATA-WP-SELFTEST: storing 0xA5 to .rodata" "$LOG"; then
    echo "  PASS: self-test probe reached (store attempted)"
else
    echo "  FAIL: probe never ran — define not compiled in?"; fails=$((fails + 1))
fi

# 2. The "store returned" line must be ABSENT — its presence means the store
#    succeeded, i.e. .rodata is writable (protection broken).
if grep -qF "store returned" "$LOG"; then
    echo "  FAIL: .rodata store RETURNED — NOT write-protected"; fails=$((fails + 1))
else
    echo "  PASS: store never returned (.rodata is read-only)"
fi

# 3. The panic must occur AT the probe: on a protected build the store faults
#    before Phase 1 runs, so the boot must NOT reach "Phase 1 ... verified".
#    (A naive "any panic" check is not enough: if the store succeeded, the probe
#    corrupts .text and the boot panics LATER at the Phase 2 golden check — a
#    downstream panic that must NOT be mistaken for the store faulting.)
if grep -qF "** PANIC:" "$LOG" && ! grep -qF "VSE Phase 1: configuration verified" "$LOG"; then
    echo "  PASS: hypervisor halted at the probe (store faulted, no further boot)"
else
    echo "  FAIL: boot continued past the probe — store did not fault there"
    fails=$((fails + 1))
fi

if [[ $fails -eq 0 ]]; then
    echo ".rodata write-protection verified at runtime."
    exit 0
else
    echo "$fails check(s) FAILED — .rodata write-protection regression."
    echo "---- captured boot log (tail) ----"; tail -25 "$LOG"
    exit 1
fi
