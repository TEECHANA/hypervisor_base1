#!/usr/bin/env bash
#
# lib_boot.sh — shared QEMU boot+capture helper for the integration tests.
#
# Provides boot_capture(), which boots the hypervisor WITH the three guest
# images loaded, drives the operator 2FA login non-interactively, and captures
# the full serial log to a file — far enough for the guests to run and for the
# organic rogue-DMA path (VM2) to reach detect -> enforce -> quarantine.
#
# Credentials:
#   Password : the dev/demo operator password "changeme" (matches the verifier
#              _pw_verifier[] provisioned in vse/login.c).
#   OTP      : derived at run time from scripts/totp_gen.py. The TOTP time base
#              is the hypervisor's RTC_PROVISIONED_EPOCH (lib/rtc/rtc.h). Because
#              that epoch is a multiple of the 30s step and login is reached in
#              well under one step of guest time, the code is deterministic
#              across boots (58377600 -> 734795 for the default epoch).
#
# This file compiles into NO hypervisor .text/.rodata — it is a host-side test
# harness only, so it does not affect the Phase 2 component goldens.

# boot_capture <logfile> [max_wait_s]
# Returns 0 once the boot reaches a terminal marker (organic quarantine or a
# login failure), or after max_wait_s. The capture is left in <logfile>.
boot_capture() {
    local log="$1"
    local max_wait="${2:-200}"

    # Resolve repo root from this script's location (tests/integration/..).
    local here root
    here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    root="$(cd "$here/../.." && pwd)"

    local ELF="$root/build/qemu/hypervisor.elf"
    local LINUX="$root/guests/linux/Image"
    local INITRD="$root/guests/linux/initramfs.cpio.gz"
    local RTOS="$root/guests/rtos/rtos.bin"
    local ANDROID="$root/guests/android_stub/android.bin"

    [[ -f "$ELF" ]] || { echo "Build first: make qemu"; return 2; }
    local g
    for g in "$LINUX" "$INITRD" "$RTOS" "$ANDROID"; do
        [[ -f "$g" ]] || { echo "Missing guest image: $g — build the guests first"; return 2; }
    done

    # Derive the operator credentials.
    local PW="changeme"
    local EPOCH OTP
    EPOCH="$(grep -oE 'RTC_PROVISIONED_EPOCH[[:space:]]+[0-9]+' "$root/lib/rtc/rtc.h" \
             | grep -oE '[0-9]+' | tail -1)"
    EPOCH="${EPOCH:-1751328000}"
    OTP="$(python3 "$root/scripts/totp_gen.py" --epoch "$EPOCH" --elapsed 5 2>/dev/null \
           | grep -oE 'TOTP code: [0-9]+' | grep -oE '[0-9]+')"
    OTP="${OTP:-734795}"

    : > "$log"
    local fifo; fifo="$(mktemp -u)"; mkfifo "$fifo"

    # Delayed credential writer: wait for the "Password:" prompt in the log,
    # then feed password + OTP. Exits afterwards (QEMU keeps running on EOF).
    (
        local i
        for i in $(seq 1 "$max_wait"); do
            grep -q "Password:" "$log" 2>/dev/null && break
            sleep 1
        done
        printf '%s\n' "$PW"
        sleep 1
        printf '%s\n' "$OTP"
        sleep 2
    ) > "$fifo" &
    local writer=$!

    qemu-system-aarch64 \
        -machine virt,gic-version=3,virtualization=on,iommu=smmuv3 \
        -cpu cortex-a57 -m 2G -smp 4 -nographic -serial mon:stdio -no-reboot \
        -d guest_errors \
        -kernel "$ELF" \
        -device loader,file="$LINUX",addr=0x41200000,force-raw=on \
        -device loader,file="$INITRD",addr=0x47000000,force-raw=on \
        -device loader,file="$RTOS",addr=0x60008000,force-raw=on \
        -device loader,file="$RTOS",addr=0x90000000,force-raw=on \
        -device loader,file="$ANDROID",addr=0x70200000,force-raw=on \
        < "$fifo" > "$log" 2>&1 &
    local qemu=$!

    # Wait for a terminal marker or the deadline, then stop QEMU.
    local i
    for i in $(seq 1 "$max_wait"); do
        kill -0 "$qemu" 2>/dev/null || break
        if grep -Eq "quarantining VM2|Maximum attempts|System locked" "$log" 2>/dev/null; then
            sleep 2   # let the quarantine/failover lines flush
            break
        fi
        sleep 1
    done

    kill "$qemu"  2>/dev/null; wait "$qemu"  2>/dev/null
    kill "$writer" 2>/dev/null; wait "$writer" 2>/dev/null
    rm -f "$fifo"
    return 0
}
