#!/usr/bin/env bash
#
# build_and_run.sh — Build everything and launch in QEMU
#
set -e

echo "=== Tessolve Hypervisor Full Build ==="

# Detect cross compiler
if command -v aarch64-linux-gnu-gcc &>/dev/null; then
    CROSS=aarch64-linux-gnu-
elif command -v aarch64-none-elf-gcc &>/dev/null; then
    CROSS=aarch64-none-elf-
else
    echo "ERROR: No AArch64 cross compiler found."
    echo "       Install with: apt install gcc-aarch64-linux-gnu"
    exit 1
fi

echo "Cross compiler : ${CROSS}gcc ($(${CROSS}gcc --version | head -1))"
echo "Platform       : qemu"
echo ""

# Step 1 — Hypervisor
# pw_verifier.h is fail-closed (no default password); inject the known "changeme"
# dev verifier for this demo build (derived by the same script a deployment uses).
echo "=== [1/4] Building hypervisor ==="
make CROSS=$CROSS PLATFORM=qemu build/qemu/hypervisor.elf \
     EXTRA_CFLAGS=-DVSE_PW_VERIFIER=$(python3 scripts/totp_gen.py --pw-define changeme)

# Step 2 — Linux demo guest
echo ""
echo "=== [2/4] Building Linux guest ==="
make -C guests/linux CROSS=$CROSS demo

# Step 3 — RTOS guest
echo ""
echo "=== [3/4] Building RTOS guest ==="
make -C guests/rtos CROSS=$CROSS

# Step 4 — Android stub guest
echo ""
echo "=== [4/4] Building Android stub guest ==="
make -C guests/android_stub CROSS=$CROSS

echo ""
echo "=== Build complete. All artifacts: ==="
ls -lh build/qemu/hypervisor.elf \
       guests/linux/Image \
       guests/rtos/rtos.bin \
       guests/android_stub/android.bin

echo ""
echo "=== Launching QEMU ==="
echo "    Press Ctrl-A X to quit QEMU"
echo ""
sleep 1

exec qemu-system-aarch64 \
    -machine virt,gic-version=3 \
    -cpu cortex-a57 \
    -m 2G -smp 4 \
    -nographic \
    -kernel build/qemu/hypervisor.elf \
    -device loader,file=guests/linux/Image,addr=0x41200000,force-raw=on \
    -device loader,file=guests/rtos/rtos.bin,addr=0x60008000,force-raw=on \
    -device loader,file=guests/android_stub/android.bin,addr=0x70200000,force-raw=on \
    -serial mon:stdio \
    -no-reboot \
    -d guest_errors,unimp
