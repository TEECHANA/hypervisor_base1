#!/usr/bin/env bash
set -e
ELF="build/qemu/hypervisor.elf"
[[ -f "$ELF" ]] || { echo "Build first: make qemu"; exit 1; }
echo "Booting in QEMU (15s timeout)..."
OUT=$(timeout 15 qemu-system-aarch64 -machine virt,gic-version=3 -cpu cortex-a57 \
    -m 2G -smp 4 -nographic -kernel "$ELF" -serial file:/dev/stdout -no-reboot 2>/dev/null || true)
echo "$OUT"
chk(){ echo "$OUT"|grep -q "$1"&&echo "PASS: $1"||{ echo "FAIL: $1"; exit 1;}; }
chk "Tessolve Hypervisor"
chk "GICv3 init"
chk "VM subsystem ready"
chk "Scheduler"
echo "All integration checks passed."
