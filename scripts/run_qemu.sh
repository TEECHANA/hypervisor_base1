#!/usr/bin/env bash
set -e

ELF="build/qemu/hypervisor.elf"
LINUX="guests/linux/Image"

[[ -f "$ELF" ]] || { echo "Run 'make qemu' first"; exit 1; }
[[ -f "$LINUX" ]] || { echo "Linux Image missing"; exit 1; }

ARGS=(
    -machine virt,iommu=smmuv3,virtualization=on,gic-version=3
    -cpu cortex-a57
    -m 2G
    -smp 4
    -nographic
    -serial mon:stdio
    -no-reboot
    -d guest_errors

    -kernel "$ELF"

    # Load Linux kernel into guest RAM
    -device loader,file="$LINUX",addr=0x41200000,force-raw=on
    -device loader,file=guests/linux/initramfs.cpio.gz,addr=0x47000000,force-raw=on
    -device loader,file=guests/rtos/rtos.bin,addr=0x60008000,force-raw=on
    -device loader,file=guests/android_stub/android.bin,addr=0x70200000,force-raw=on
    -device loader,file=guests/rtos/rtos.bin,addr=0x90000000,force-raw=on
)

[[ "$1" == "--debug" ]] && {
    echo "GDB on :1234"
    ARGS+=(-s -S)
}

exec qemu-system-aarch64 "${ARGS[@]}"
