#!/usr/bin/env bash
#
# reprovision_golden.sh — auto-fix the VSE Phase 2 .text golden hash.
#
# WHY THIS EXISTS
# ---------------
# Any change to the hypervisor's code changes its .text section, so the Phase 2
# component-integrity check panics on next boot until the golden HMAC in
# vse/component_check.c slot [0] is updated to match. Doing that by hand
# (boot, copy the "Computed:" block, paste, rebuild) is error-prone and
# tedious. This script does the whole loop automatically and SAFELY:
#
#   1. build
#   2. run QEMU briefly, capture the boot log
#   3. if Phase 2 says "verified OK"  -> nothing to do, exit 0
#   4. if Phase 2 panics with a .text "Computed:" hash:
#        - extract ONLY the computed block (never the golden block)
#        - patch ONLY slot [0] of _golden_components[] (never slot [1])
#        - rebuild
#        - exit 0 (golden now matches; next real boot passes Phase 2)
#   5. anything else (no panic, no hash) -> patch NOTHING, exit non-zero
#
# It makes a timestamped backup of component_check.c before editing.
#
# SAFETY: this only re-provisions the integrity baseline to match YOUR current
# build. Run it only when the code change is intentional and trusted — it is a
# developer convenience, not something to run on an untrusted image.
#
# Usage:
#   ./reprovision_golden.sh            # run from hypervisor1/
#   ./reprovision_golden.sh --run      # after fixing, also do a normal QEMU run
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

CC="vse/component_check.c"
RUNNER="./scripts/run_qemu.sh"
LOG="/tmp/reprov_boot.log"
QEMU_SECONDS=25      # how long to let QEMU run before capturing/killing

R='\033[0;31m'; G='\033[0;32m'; Y='\033[0;33m'; B='\033[0;34m'; N='\033[0m'
info(){ echo -e "${B}==>${N} $*"; }
ok(){   echo -e "${G} ok ${N} $*"; }
warn(){ echo -e "${Y}warn${N} $*"; }
die(){  echo -e "${R}ERROR${N} $*" >&2; exit "${2:-1}"; }

[[ -f "$CC" ]]     || die "$CC not found — run from hypervisor1/"
[[ -x "$RUNNER" ]] || die "$RUNNER not found/executable"
command -v python3 >/dev/null || die "python3 required"

build(){
    info "building..."
    make >/dev/null 2>&1 || { make 2>&1 | tail -20; die "build failed"; }
    ok "build done"
}

# Run QEMU for a bounded time, capture output, then make sure it's dead.
#
# NOTE: we do NOT call run_qemu.sh here, because it uses "-serial mon:stdio"
# (interactive monitor) which does not flush reliably to a file under timeout.
# Instead we build an equivalent QEMU command that sends the serial console
# straight to the log file via stdio, with the monitor disabled. This captures
# the full boot log non-interactively. (The interactive run_qemu.sh is still
# used for normal --run.)
ELF="build/qemu/hypervisor.elf"
LINUX="guests/linux/Image"
capture_boot(){
    info "running QEMU (up to ${QEMU_SECONDS}s capture)..."
    : > "$LOG"
    [[ -f "$ELF" ]]   || die "$ELF missing — build first"
    [[ -f "$LINUX" ]] || die "$LINUX missing"
    # Stream QEMU serial to the log. We background QEMU and tail the log until
    # we see a decisive Phase-2 result (verified OK, or the panic), then give
    # it a brief grace period so the full computed-hash block flushes, then
    # stop. This avoids truncating the panic mid-print (the 12s-timeout bug).
    timeout "${QEMU_SECONDS}s" qemu-system-aarch64 \
        -machine virt,iommu=smmuv3,virtualization=on,gic-version=3 \
        -cpu cortex-a57 -m 2G -smp 4 -nographic \
        -serial stdio -monitor none -no-reboot -d guest_errors \
        -kernel "$ELF" \
        -device loader,file="$LINUX",addr=0x41000000,force-raw=on \
        </dev/null >"$LOG" 2>&1 &
    local qpid=$!
    # Poll the log for a decisive line.
    local waited=0
    while kill -0 "$qpid" 2>/dev/null; do
        if grep -q "component '.text' verified OK" "$LOG" 2>/dev/null; then
            sleep 1; break
        fi
        if grep -qi "PANIC: VSE: component integrity" "$LOG" 2>/dev/null; then
            sleep 2; break   # grace: let the computed block finish printing
        fi
        sleep 1
        waited=$((waited+1))
        [[ $waited -ge $QEMU_SECONDS ]] && break
    done
    kill "$qpid" 2>/dev/null || true
    pkill -f "qemu-system-aarch64.*hypervisor.elf" 2>/dev/null || true
    wait "$qpid" 2>/dev/null || true
    ok "captured $(wc -l < "$LOG") lines -> $LOG"
}

build
capture_boot

# Decide outcome from the log.
if grep -q "component '.text' verified OK" "$LOG"; then
    ok ".text already verified — golden is correct, nothing to do."
    exit 0
fi

if ! grep -qi "COMPONENT INTEGRITY VIOLATION in '.text'" "$LOG"; then
    warn "No .text verification AND no .text violation found in the log."
    warn "The boot may have failed for another reason. Patching nothing."
    echo "---- tail of boot log ----"
    tail -25 "$LOG"
    die "unexpected boot state — not touching $CC" 2
fi

info ".text violation detected — extracting computed hash and patching slot [0]"

# Backup before edit.
cp "$CC" "${CC}.bak.$(date +%Y%m%d_%H%M%S)"

python3 - "$LOG" "$CC" <<'PY'
import re, sys
logpath, ccpath = sys.argv[1], sys.argv[2]
log = open(logpath, errors="replace").read()

# Extract ONLY the 'computed:' block (the new correct hash).
lines = log.splitlines()
start = None
for i, ln in enumerate(lines):
    if re.search(r'\bcomputed:\s*$', ln, re.I):
        start = i + 1
        break
if start is None:
    sys.exit("could not find 'computed:' block in log")

found = []
for ln in lines[start:]:
    hb = re.findall(r'0x[0-9a-fA-F]{2}', ln)
    if hb:
        found.extend(hb)
    elif found:
        break
    if len(found) >= 32:
        break
if len(found) != 32:
    sys.exit(f"expected 32 computed bytes, got {len(found)} — the boot capture "
             f"was likely truncated before the full hash printed. Increase "
             f"QEMU_SECONDS in this script, or paste the hash manually.")
new_bytes = [b.lower() for b in found[:32]]

# Patch ONLY slot [0] .text, anchored on its comment.
src = open(ccpath).read()
rows = ["        " + ", ".join(new_bytes[r*8:(r+1)*8]) + "," for r in range(4)]
new_block = "\n".join(rows)
pat = re.compile(
    r"(/\* \[0\] \.text golden HMAC[^\n]*\*/\s*\n\s*\{\s*\n)"
    r"(?:\s*0x[0-9a-fA-F]{2}(?:\s*,\s*0x[0-9a-fA-F]{2})*\s*,?\s*\n){4}",
    re.M)
out, n = pat.subn(lambda m: m.group(1) + new_block + "\n", src, count=1)
if n != 1:
    sys.exit(f"expected to patch exactly 1 golden block, patched {n} — aborting")
open(ccpath, "w").write(out)
print("patched .text golden slot [0] with:", " ".join(new_bytes[:4]), "...")
PY

ok "golden updated — rebuilding"
build

# Optional verify pass: rebuild already done; confirm it now verifies.
info "verifying the new golden boots clean..."
capture_boot
if grep -q "component '.text' verified OK" "$LOG"; then
    ok ".text now verifies — re-provision complete."
else
    warn "still not verifying — the golden may be self-referential this build."
    warn "Check the log tail; you may need manual learn mode this once."
    tail -15 "$LOG"
    die "re-provision did not converge" 3
fi

if [[ "${1:-}" == "--run" ]]; then
    info "launching a normal interactive run..."
    exec "$RUNNER"
fi

echo
ok "Done. Next real boot will pass Phase 2. Backup saved as ${CC}.bak.*"
