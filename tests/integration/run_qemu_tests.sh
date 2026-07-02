#!/usr/bin/env bash
#
# Integration smoke test: boot the hypervisor WITH its three guests (matching
# `make run-with-guests`) and assert the system reaches a healthy running state
# — VSE Phase 2 verified, operator login, non-zero RTOS fuel rpm, and the
# scheduler time-slicing between guests. The heavy lifting (driving the TOTP
# login non-interactively) lives in qemu_integration.py.
#
# The previous version booted a NO-GUEST config without virtualization=on and
# grepped for post-login markers that the login gate now makes unreachable, so
# it could never pass. Replaced with a guest-loaded, login-driving check.
set -e

ELF="build/qemu/hypervisor.elf"
[[ -f "$ELF" ]] || { echo "Build first: make all"; exit 1; }

for img in guests/linux/Image guests/linux/initramfs.cpio.gz \
           guests/rtos/rtos.bin guests/android_stub/android.bin; do
    [[ -f "$img" ]] || { echo "Missing guest image: $img — run 'make guests'"; exit 1; }
done

command -v qemu-system-aarch64 >/dev/null || { echo "qemu-system-aarch64 not found"; exit 1; }
python3 -c 'import pexpect' 2>/dev/null || { echo "python3 'pexpect' module required (pip install pexpect)"; exit 1; }

exec python3 tests/integration/qemu_integration.py
