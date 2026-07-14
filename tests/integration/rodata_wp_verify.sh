#!/usr/bin/env bash
#
# rodata_wp_verify.sh — regression test for .rodata Stage-1 write-protection.
#
# Audit #7b (commit c0a0858) maps .rodata read-only at EL2 (mmu.S L2[1],
# AP[2]=1). This test builds a hypervisor with -DRODATA_WP_SELFTEST (a guarded
# probe in boot/main.c that stores 0xA5 to .rodata early in hyp_main) and boots
# it: on a protected build the store raises an EL2 permission fault and the
# hypervisor PANICS at the probe, before Phase 1; the "store returned" line is
# never printed.
#
# SOUND VERDICT (this is a rewrite — the old logic could vacuously "PASS" the
# read-only check from the ABSENCE of a string, e.g. when the boot produced no
# output at all). We only conclude "protected" from POSITIVE evidence, and gate
# on the self-test actually having run:
#   Gate A: the probe is compiled into the ELF        (else: define didn't reach build)
#   Gate B: the boot produced output                  (else: QEMU didn't run — inconclusive)
#   Gate C: the probe executed (store attempted)       (else: inconclusive)
#   Verdict: store RETURNED -> NOT protected (fail); store FAULTED at probe -> PASS.
# Any gate failing is a LOUD, unambiguous failure — never a partial/contradictory pass.
#
# The special build goes into an isolated BUILD_DIR so build/qemu is untouched.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

BDIR="$(mktemp -d)"; LOG="$(mktemp)"
trap 'rm -rf "$BDIR" "$LOG"' EXIT

dump_and_die() { echo "$1"; echo "---- captured boot log ----"; cat "$LOG"; exit 1; }

echo "=== .rodata write-protection regression test ==="
echo "Building self-test image (-DRODATA_WP_SELFTEST) in isolated dir..."
# Also build with -DVSE_COMPONENTS_LEARN (like trust_promote_verify.sh): the probe
# shifts .text/.rodata, so the Phase 2 goldens no longer match. In LEARN mode
# Phase 2 is non-fatal, so on an UNPROTECTED build the boot continues cleanly past
# the probe and the "store returned" line is reliably captured -> an honest
# "WRITABLE, protection NOT enforced" FAIL, instead of a Phase 2 panic that some
# hosts drop as empty output. On a PROTECTED build the probe faults and panics
# before Phase 2 is ever reached, so learn mode is moot there.
if ! make -C "$ROOT" qemu BUILD_DIR="$BDIR" \
        EXTRA_CFLAGS="-DRODATA_WP_SELFTEST -DVSE_COMPONENTS_LEARN" \
        >"$BDIR/build.log" 2>&1; then
    echo "  FAIL: self-test build failed"; tail -20 "$BDIR/build.log"; exit 1
fi
ELF="$BDIR/hypervisor.elf"
[[ -f "$ELF" ]] || { echo "  FAIL: no ELF produced"; exit 1; }

# Gate A — the define actually reached the build (probe present in the image).
# Distinguishes "define didn't compile in" from any runtime conclusion.
if grep -aqF "RODATA-WP-SELFTEST" "$ELF"; then
    echo "  ok  : probe is compiled into the self-test image (-D reached the build)"
else
    echo "  FAIL: -DRODATA_WP_SELFTEST did NOT reach the build (probe absent from ELF)."
    echo "        Cannot test .rodata write-protection — INCONCLUSIVE."
    exit 1
fi

echo "Booting hypervisor-only and capturing serial..."
# The probe runs right after the boot banner, before any VM is created, so no
# guest images are needed. The image emits only ~9 lines then HALTS at the panic;
# capture the serial with '-serial file:' so QEMU writes it straight to the file
# as it is produced. (The old '-serial mon:stdio > file' relied on QEMU flushing
# stdio before `timeout` kills it — on some hosts that tiny pre-halt output was
# lost, yielding an empty log and a spurious "no boot output" INCONCLUSIVE.)
timeout 25 qemu-system-aarch64 \
    -machine virt,gic-version=3,virtualization=on,iommu=smmuv3 \
    -cpu cortex-a57 -m 2G -smp 4 -nographic -serial "file:$LOG" -no-reboot -nic none \
    -d guest_errors -kernel "$ELF" >/dev/null 2>&1 || true

# Gate B — QEMU actually ran and the hypervisor produced output. An empty/no-boot
# log is the failure mode that used to vacuously "pass" the old read-only check.
if ! grep -qF "Tessolve Hypervisor v1.0" "$LOG"; then
    dump_and_die "  FAIL: self-test image produced no boot output (QEMU did not run) — INCONCLUSIVE."
fi

# Gate C — the probe executed (the store was attempted). Without this, nothing
# can be said about .rodata.
if grep -qF "RODATA-WP-SELFTEST: storing 0xA5 to .rodata" "$LOG"; then
    echo "  ok  : probe executed (store to .rodata attempted)"
else
    dump_and_die "  FAIL: probe did not execute despite being compiled in — INCONCLUSIVE."
fi

# Verdict — decide from POSITIVE evidence only.
if grep -qF "store returned" "$LOG"; then
    echo "  FAIL: the store to .rodata RETURNED — .rodata is WRITABLE, protection NOT enforced."
    echo "---- relevant lines ----"; grep -E "RODATA-WP-SELFTEST|PANIC" "$LOG"
    exit 1
fi
# The panic must be AT the probe: on a protected build the store faults before
# Phase 1 runs, so the boot must NOT reach "Phase 1 ... verified". (On an
# unprotected build the store instead RETURNS and the boot sails on through the
# now non-fatal learn-mode Phase 2 — that case is already caught above by the
# "store returned" check, so it never reaches here.)
if grep -qF "** PANIC:" "$LOG" && ! grep -qF "VSE Phase 1: configuration verified" "$LOG"; then
    echo "  PASS: store to .rodata FAULTED at the probe (halted before Phase 1)."
    echo ".rodata write-protection verified at runtime."
    exit 0
fi

dump_and_die "  FAIL: the store neither returned nor faulted at the probe — INCONCLUSIVE."
