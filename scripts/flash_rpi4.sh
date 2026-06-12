#!/usr/bin/env bash
DEV="$1"; [[ -n "$DEV" ]] || { echo "Usage: $0 /dev/sdX"; exit 1; }
echo "Erasing $DEV in 5s... Ctrl-C to abort"; sleep 5
parted -s "$DEV" mklabel msdos
parted -s "$DEV" mkpart primary fat32 1MiB 257MiB
parted -s "$DEV" mkpart primary ext4  257MiB 100%
mkfs.vfat -F32 "${DEV}1"
mkfs.ext4 -F   "${DEV}2"
MNT=$(mktemp -d); mount "${DEV}1" "$MNT"
cp build/rpi4/hypervisor.elf "$MNT/kernel8.img"
printf 'arm_64bit=1\nkernel=kernel8.img\nenable_uart=1\ncore_freq=500\ngpu_mem=16\n' > "$MNT/config.txt"
umount "$MNT"; rmdir "$MNT"; sync
echo "Done."
