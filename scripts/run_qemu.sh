#!/usr/bin/env bash
set -e

ELF="build/qemu/hypervisor.elf"
LINUX="guests/linux/Image"

[[ -f "$ELF" ]] || { echo "Run 'make qemu' first"; exit 1; }
[[ -f "$LINUX" ]] || { echo "Linux Image missing"; exit 1; }

ARGS=(
    -machine virt,virtualization=on,gic-version=3
    -cpu cortex-a57
    -m 2G
    -smp 4
    -nographic
    -serial mon:stdio
    -no-reboot
    -d guest_errors

    -kernel "$ELF"

    # Load Linux kernel into guest RAM
    -device loader,file="$LINUX",addr=0x41000000,force-raw=on
)

[[ "$1" == "--debug" ]] && {
    echo "GDB on :1234"
    ARGS+=(-s -S)
}

exec qemu-system-aarch64 "${ARGS[@]}"
