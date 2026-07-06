#!/usr/bin/env bash
#
# Integration test (run with: make test-integration).
#
# Boots the hypervisor in QEMU WITH the three guest images loaded and verifies
# the end-to-end security path up to the operator-login gate:
#   - configuration integrity check (Phase 1)
#   - component self-measurement / root of trust (Phase 2, .text + .rodata)
#   - creation + attestation + trust of all three guests (linux, rtos, android)
#   - scheduler reaching the login prompt
#
# All of these happen BEFORE the interactive login, so no credentials/TOTP are
# needed here. (The previous version booted the bare ELF with no guests loaded
# and only checked four generic strings — it could not exercise attestation.)

set -u
ELF="build/qemu/hypervisor.elf"
LINUX="guests/linux/Image"
INITRD="guests/linux/initramfs.cpio.gz"
RTOS="guests/rtos/rtos.bin"
ANDROID="guests/android_stub/android.bin"

[[ -f "$ELF" ]] || { echo "Build first: make qemu"; exit 1; }
for g in "$LINUX" "$INITRD" "$RTOS" "$ANDROID"; do
    [[ -f "$g" ]] || { echo "Missing guest image: $g — build the guests first"; exit 1; }
done

echo "Booting hypervisor + 3 guests in QEMU (25s capture)..."
OUT=$(timeout 25 qemu-system-aarch64 \
    -machine virt,iommu=smmuv3,virtualization=on,gic-version=3 \
    -cpu cortex-a57 -m 2G -smp 4 -nographic -serial mon:stdio -no-reboot \
    -kernel "$ELF" \
    -device loader,file="$LINUX",addr=0x41200000,force-raw=on \
    -device loader,file="$INITRD",addr=0x47000000,force-raw=on \
    -device loader,file="$RTOS",addr=0x60008000,force-raw=on \
    -device loader,file="$ANDROID",addr=0x70200000,force-raw=on \
    -device loader,file="$RTOS",addr=0x90000000,force-raw=on \
    </dev/null 2>/dev/null || true)

fails=0
chk(){ if grep -qF -- "$1" <<<"$OUT"; then echo "  PASS: $1"
       else echo "  FAIL: $1"; fails=$((fails + 1)); fi; }

chk "Tessolve Hypervisor v1.0"
chk "VSE Phase 1: configuration verified"
chk "VSE Phase 2: all components verified"
chk "VM 1 'linux' created"
chk "VM 2 'rtos' created"
chk "VM 3 'android' created"
chk "guest 'linux' genuineness VERIFIED"
chk "guest 'rtos' genuineness VERIFIED"
chk "guest 'android' genuineness VERIFIED"
chk "VM1 'linux' -> TRUSTED"
chk "VM2 'rtos' -> TRUSTED"
chk "VM3 'android' -> TRUSTED"
chk "Operator Login"

if [[ $fails -eq 0 ]]; then
    echo "All integration checks passed."
else
    echo "$fails integration check(s) FAILED."
    exit 1
fi
