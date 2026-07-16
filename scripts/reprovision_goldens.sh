#!/usr/bin/env bash
#
# reprovision_goldens.sh — auto re-provision BOTH Phase 2 component goldens
# (.text slot [0] AND .rodata slot [1]) in vse/component_check.c.
#
# Usage:  scripts/reprovision_goldens.sh          # or: make reprovision-goldens
#
# WHY LEARN MODE
# --------------
# Any hypervisor source change shifts .text; new string/const data shifts
# .rodata. The old enforce-mode approach panics on the FIRST mismatch (.text),
# so .rodata's computed hash never even prints — that is why it could only ever
# fix .text. This tool instead builds ONE image with -DVSE_COMPONENTS_LEARN,
# which blanks the goldens and puts Phase 2 in its (non-fatal) learn mode: it
# LOGS the measured HMAC of every component and continues. Those learned values
# are exactly what the real build needs:
#   - .text is byte-identical between the learn and enforce builds (the golden
#     lives in .rodata; measuring .text never sees it), and
#   - the .rodata measurement deliberately SKIPS the _golden_components[] table,
#     so a blank vs filled table yields the same .rodata hash.
# So we learn both, paste both, rebuild for real, and verify.
#
# The learn image is built in an isolated dir; build/qemu is only rebuilt at the
# end (for real) so a failed run never leaves a blank-golden image behind.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
CC="$ROOT/vse/component_check.c"

R='\033[0;31m'; G='\033[0;32m'; Y='\033[0;33m'; B='\033[0;34m'; N='\033[0m'
info(){ echo -e "${B}==>${N} $*"; }
ok(){   echo -e "${G} ok ${N} $*"; }
warn(){ echo -e "${Y}warn${N} $*"; }
die(){  echo -e "${R}ERROR${N} $*" >&2; exit "${2:-1}"; }

command -v qemu-system-aarch64 >/dev/null || die "qemu-system-aarch64 required"
command -v python3 >/dev/null || die "python3 required"
[[ -f "$CC" ]] || die "$CC not found"

BDIR="$(mktemp -d)"; LOG="$(mktemp)"
trap 'rm -rf "$BDIR" "$LOG"' EXIT

# pw_verifier.h is fail-closed (no default password), so every build here must
# supply VSE_PW_VERIFIER. If the header is already PROVISIONED with a concrete
# password (scripts/provision_password.sh), measure THAT image — do not inject,
# since -D would override the header's #ifndef and silently measure the wrong
# password. Only when the header is the committed fail-closed #error do we inject
# the "changeme" dev verifier (the repo's canonical measured image).
if grep -qE '^#[[:space:]]*define[[:space:]]+VSE_PW_VERIFIER' "$ROOT/vse/pw_verifier.h"; then
    PW_CFLAGS=""
    info "pw_verifier.h is provisioned — measuring that password"
else
    PW_CFLAGS="-DVSE_PW_VERIFIER=$(python3 "$ROOT/scripts/totp_gen.py" --pw-define changeme)"
    info "pw_verifier.h is fail-closed — measuring the changeme dev image"
fi

# ── 1. Build a learn-mode image (blank goldens -> Phase 2 logs, never panics) ──
info "building learn-mode image (-DVSE_COMPONENTS_LEARN)..."
make -C "$ROOT" qemu BUILD_DIR="$BDIR" EXTRA_CFLAGS="-DVSE_COMPONENTS_LEARN $PW_CFLAGS" \
     >"$BDIR/build.log" 2>&1 || { tail -20 "$BDIR/build.log"; die "learn build failed"; }

# ── 2. Boot hypervisor-only and capture both [LEARN] blocks ──
info "booting to capture measured HMACs..."
timeout 30 qemu-system-aarch64 \
    -machine virt,gic-version=3,virtualization=on,iommu=smmuv3 \
    -cpu cortex-a57 -m 2G -smp 4 -nographic -serial mon:stdio -no-reboot \
    -d guest_errors -kernel "$BDIR/hypervisor.elf" >"$LOG" 2>&1 &
qemu=$!
for _ in $(seq 1 30); do
    kill -0 "$qemu" 2>/dev/null || break
    # Both learn blocks present? (.rodata is logged after .text)
    if grep -q "\[LEARN\] component '.rodata'" "$LOG" 2>/dev/null; then
        sleep 2; break   # grace: let the last hash rows flush
    fi
    sleep 1
done
kill "$qemu" 2>/dev/null; wait "$qemu" 2>/dev/null || true

# ── 3. Parse the learned HMACs and patch BOTH golden slots ──
cp "$CC" "${CC}.bak.$(date +%Y%m%d-%H%M%S)"
info "patching .text (slot [0]) and .rodata (slot [1]) goldens..."
python3 - "$LOG" "$CC" <<'PY'
import re, sys
log = open(sys.argv[1], errors="replace").read().splitlines()

def learn_bytes(section):
    """32 hex bytes following the [LEARN] block for `section`. The UART
    interleaves blank lines between hex rows, so collect across blanks and stop
    only at the next [LEARN] block (never break on a blank/non-hex line)."""
    for i, ln in enumerate(log):
        if re.search(r"\[LEARN\] component '%s'" % re.escape(section), ln):
            found = []
            for l2 in log[i+1:]:
                if "[LEARN]" in l2:
                    break   # reached the next component's block
                found.extend(re.findall(r'0x[0-9a-fA-F]{2}', l2))
                if len(found) >= 32:
                    return [b.lower() for b in found[:32]]
    sys.exit(f"could not find 32 learned bytes for {section} in the boot log")

text_b   = learn_bytes('.text')
rodata_b = learn_bytes('.rodata')

src = open(sys.argv[2]).read()

def patch(src, idx, byts):
    rows = "\n".join("        " + ", ".join(byts[r*8:(r+1)*8]) + "," for r in range(4))
    # Anchor on the "[idx]" marker comment + opening brace; replace the 4 hex rows.
    pat = re.compile(
        r"(/\*\s*\[%d\][^\n]*\*/\s*\n\s*\{\s*\n)"
        r"(?:\s*0x[0-9a-fA-F]{2}(?:\s*,\s*0x[0-9a-fA-F]{2})*\s*,?\s*\n){4}" % idx,
        re.M)
    out, n = pat.subn(lambda m: m.group(1) + rows + "\n", src, count=1)
    if n != 1:
        sys.exit(f"expected to patch exactly 1 block for slot [{idx}], patched {n}")
    return out

src = patch(src, 0, text_b)
src = patch(src, 1, rodata_b)
open(sys.argv[2], "w").write(src)
print("  .text  slot[0]:", " ".join(text_b[:4]), "...")
print("  .rodata slot[1]:", " ".join(rodata_b[:4]), "...")
PY

# ── 4. Rebuild for real and verify Phase 2 now passes ──
info "rebuilding (enforce) and verifying..."
rm -rf "$ROOT/build/qemu"
make -C "$ROOT" qemu EXTRA_CFLAGS="$PW_CFLAGS" >"$BDIR/rebuild.log" 2>&1 || { tail -20 "$BDIR/rebuild.log"; die "rebuild failed"; }
: > "$LOG"
timeout 25 qemu-system-aarch64 \
    -machine virt,gic-version=3,virtualization=on,iommu=smmuv3 \
    -cpu cortex-a57 -m 2G -smp 4 -nographic -serial mon:stdio -no-reboot \
    -d guest_errors -kernel "$ROOT/build/qemu/hypervisor.elf" >"$LOG" 2>&1 &
qemu=$!
for _ in $(seq 1 25); do
    kill -0 "$qemu" 2>/dev/null || break
    grep -q "VSE Phase 2: all components verified" "$LOG" && break
    grep -q "COMPONENT INTEGRITY VIOLATION" "$LOG" && break
    sleep 1
done
kill "$qemu" 2>/dev/null; wait "$qemu" 2>/dev/null || true

if grep -q "VSE Phase 2: all components verified" "$LOG"; then
    ok "both goldens re-provisioned — Phase 2 verifies (.text + .rodata)."
    exit 0
else
    warn "Phase 2 did not verify after re-provision; see log:"
    grep -E "component|VIOLATION|verified" "$LOG" | tail -10
    die "re-provision did not converge" 3
fi
