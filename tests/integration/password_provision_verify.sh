#!/usr/bin/env bash
#
# password_provision_verify.sh — prove per-deployment password provisioning.
#
# Provisions a NON-default operator password with scripts/provision_password.sh,
# builds an image with it, and drives the 2FA login twice to prove:
#   - login SUCCEEDS with the newly provisioned password (+ valid TOTP), and
#   - login FAILS with the old dev default "changeme".
#
# The committed default (changeme) is regenerated only long enough to build the
# throwaway image, then RESTORED (git checkout), so the test harness keeps
# working. The build uses -DVSE_COMPONENTS_LEARN: a non-default password moves
# the Phase 2-measured .rodata (never .text), so learn mode lets the throwaway
# image boot past attestation without a golden reprovision. A real deployment
# would instead run `make reprovision-goldens`.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
HDR="$ROOT/vse/pw_verifier.h"

NEWPW='Tessolve#Fleet2026!'
BDIR="$(mktemp -d)"; NLOG="$(mktemp)"; OLOG="$(mktemp)"
trap 'git -C "$ROOT" checkout -- vse/pw_verifier.h 2>/dev/null; rm -rf "$BDIR" "$NLOG" "$OLOG" "$HDR".bak.* 2>/dev/null' EXIT

echo "=== password provisioning proof ==="

# 1. Provision the NON-default password (regenerates pw_verifier.h).
"$ROOT/scripts/provision_password.sh" "$NEWPW" >/dev/null || { echo "  FAIL: provisioning failed"; exit 1; }
grep -q "VSE_PW_VERIFIER" "$HDR" || { echo "  FAIL: pw_verifier.h not regenerated"; exit 1; }

# 2. Build the throwaway image with the new verifier (learn mode for the moved
#    .rodata), then immediately restore the committed default so the tree stays
#    clean for the rest of the run.
if ! make -C "$ROOT" qemu BUILD_DIR="$BDIR" EXTRA_CFLAGS=-DVSE_COMPONENTS_LEARN \
        >"$BDIR/build.log" 2>&1; then
    echo "  FAIL: build failed"; tail -15 "$BDIR/build.log"; exit 1
fi
ELF="$BDIR/hypervisor.elf"
git -C "$ROOT" checkout -- vse/pw_verifier.h

LINUX="$ROOT/guests/linux/Image"; INITRD="$ROOT/guests/linux/initramfs.cpio.gz"
RTOS="$ROOT/guests/rtos/rtos.bin"; ANDROID="$ROOT/guests/android_stub/android.bin"
for g in "$LINUX" "$INITRD" "$RTOS" "$ANDROID"; do
    [[ -f "$g" ]] || { echo "  FAIL: missing guest image $g"; exit 1; }
done

# TOTP is derived from the master key (unchanged by a password change), so the
# same code works in both cases — this isolates the PASSWORD factor.
EPOCH="$(grep -oE 'RTC_PROVISIONED_EPOCH[[:space:]]+[0-9]+' "$ROOT/lib/rtc/rtc.h" | grep -oE '[0-9]+' | tail -1)"
EPOCH="${EPOCH:-1751328000}"
OTP="$(python3 "$ROOT/scripts/totp_gen.py" --epoch "$EPOCH" --elapsed 5 2>/dev/null | grep -oE 'TOTP code: [0-9]+' | grep -oE '[0-9]+')"
OTP="${OTP:-734795}"

# Boot with guests and drive the login with $1 as the password (fed up to 3x for
# the deny path); capture to $2. Stops on a terminal login outcome.
drive_login() {
    local pw="$1" log="$2" fifo; fifo="$(mktemp -u)"; mkfifo "$fifo"
    ( for _ in $(seq 1 45); do grep -q "Password:" "$log" 2>/dev/null && break; sleep 1; done
      for _ in 1 2 3; do printf '%s\n' "$pw"; sleep 1; printf '%s\n' "$OTP"; sleep 1; done ) > "$fifo" &
    local w=$!
    timeout 70 qemu-system-aarch64 \
        -machine virt,gic-version=3,virtualization=on,iommu=smmuv3 \
        -cpu cortex-a57 -m 2G -smp 4 -nographic -serial mon:stdio -no-reboot -nic none \
        -d guest_errors -kernel "$ELF" \
        -device loader,file="$LINUX",addr=0x41200000,force-raw=on \
        -device loader,file="$INITRD",addr=0x47000000,force-raw=on \
        -device loader,file="$RTOS",addr=0x60008000,force-raw=on \
        -device loader,file="$RTOS",addr=0x90000000,force-raw=on \
        -device loader,file="$ANDROID",addr=0x70200000,force-raw=on \
        < "$fifo" > "$log" 2>&1 &
    local q=$!
    for _ in $(seq 1 70); do
        kill -0 "$q" 2>/dev/null || break
        grep -qE "Access granted|Maximum attempts exceeded" "$log" && break
        sleep 1
    done
    kill "$q" "$w" 2>/dev/null; wait "$q" "$w" 2>/dev/null; rm -f "$fifo"
}

echo "Driving login with the NEW password..."
drive_login "$NEWPW" "$NLOG"
echo "Driving login with the OLD default 'changeme'..."
drive_login "changeme" "$OLOG"

fails=0
if grep -q "Access granted" "$NLOG"; then
    echo "  PASS: login SUCCEEDS with the newly provisioned password"
else echo "  FAIL: new password did not grant access"; fails=$((fails+1)); fi

if grep -q "Access granted" "$OLOG"; then
    echo "  FAIL: old 'changeme' STILL granted access (not re-provisioned)"; fails=$((fails+1))
else echo "  PASS: old 'changeme' does NOT grant access"; fi
if grep -qE "Access denied|Maximum attempts exceeded" "$OLOG"; then
    echo "  PASS: 'changeme' login explicitly denied"
else echo "  FAIL: no denial observed for 'changeme'"; fails=$((fails+1)); fi

# The committed dev default must be intact so the test harness keeps working.
if git -C "$ROOT" diff --quiet -- vse/pw_verifier.h; then
    echo "  PASS: committed pw_verifier.h (changeme default) left untouched"
else echo "  FAIL: pw_verifier.h was modified"; fails=$((fails+1)); fi

if [[ $fails -eq 0 ]]; then
    echo "Password provisioning verified: new password works, changeme rejected."
    exit 0
else
    echo "$fails check(s) FAILED."
    echo "---- new-password login tail ----"; grep -E "Password:|OTP|Access" "$NLOG" | tail -6
    echo "---- changeme login tail ----";     grep -E "Password:|OTP|Access|Maximum" "$OLOG" | tail -6
    exit 1
fi
