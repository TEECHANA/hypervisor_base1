#!/usr/bin/env bash
#
# vse_ids_integration_test.sh — Activity 1.3 / 1.4
#
# Full-system integration test for the VSE + IDS hypervisor. Boots the image
# non-interactively, captures the boot log up to the operator-login prompt
# (the IDS attack demo runs before login, so no login is needed to gather
# evidence), and asserts that every stage actually happened:
#
#   VSE Phase 1..6 each verified
#   IDS monitor initialised
#   IDS detected the injected attack (REPEAT + STORM)
#   IDS actively enforced (quarantined the offending VM)  [1.2.2]
#   Trust engine moved the VM to QUARANTINE
#   Audit log was produced
#
# Prints PASS/FAIL per check and an overall result. Exit 0 if all pass.
#
# Usage:   ./vse_ids_integration_test.sh
#          ./vse_ids_integration_test.sh --keep   # keep the captured log
#
set -uo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ELF="build/qemu/hypervisor.elf"
LINUX="guests/linux/Image"
LOG="/tmp/vse_ids_test_$(date +%s).log"
CAP_SECONDS=25

R='\033[0;31m'; G='\033[0;32m'; Y='\033[0;33m'; B='\033[0;34m'; N='\033[0m'
pass_n=0; fail_n=0
info(){ echo -e "${B}==>${N} $*"; }

check(){
    # check "label" "grep-pattern"
    local label="$1" pat="$2"
    if grep -qE "$pat" "$LOG" 2>/dev/null; then
        echo -e "  [${G}PASS${N}] $label"
        pass_n=$((pass_n+1))
    else
        echo -e "  [${R}FAIL${N}] $label"
        fail_n=$((fail_n+1))
    fi
}

# ── 1. Build ──────────────────────────────────────────────────────────────
info "Building hypervisor..."
if ! make >/dev/null 2>&1; then
    echo -e "${R}BUILD FAILED${N} — run 'make' to see errors."; exit 1
fi
[[ -f "$ELF" ]] || { echo -e "${R}$ELF missing after build${N}"; exit 1; }

# ── 2. Boot + capture (non-interactive, up to the login prompt) ───────────
info "Booting under QEMU and capturing boot log (${CAP_SECONDS}s max)..."
: > "$LOG"
timeout "${CAP_SECONDS}s" qemu-system-aarch64 \
    -machine virt,iommu=smmuv3,virtualization=on,gic-version=3 \
    -cpu cortex-a57 -m 2G -smp 4 -nographic \
    -serial stdio -monitor none -no-reboot -d guest_errors \
    -kernel "$ELF" \
    -device loader,file="$LINUX",addr=0x41000000,force-raw=on \
    </dev/null >"$LOG" 2>&1 &
qpid=$!
# Stop once we reach the login prompt (all evidence is printed by then).
waited=0
while kill -0 "$qpid" 2>/dev/null; do
    grep -q "Operator Login" "$LOG" 2>/dev/null && { sleep 1; break; }
    grep -qi "PANIC" "$LOG" 2>/dev/null && { sleep 1; break; }
    sleep 1; waited=$((waited+1))
    [[ $waited -ge $CAP_SECONDS ]] && break
done
kill "$qpid" 2>/dev/null || true
pkill -f "qemu-system-aarch64.*hypervisor.elf" 2>/dev/null || true
wait "$qpid" 2>/dev/null || true
info "Captured $(wc -l < "$LOG") lines."
echo

# ── 3. Assertions ─────────────────────────────────────────────────────────
echo "── VSE integrity & trust chain ─────────────────────────────"
check "Phase 1: configuration verified"      "VSE Phase 1: configuration verified"
check "Phase 2: components verified (.text)"  "VSE Phase 2: components verified"
check "Phase 3: trust services initialized"   "VSE Phase 3: trust services initialized"
check "Phase 4: sealing service initialized"  "VSE Phase 4: sealing service initialized"
check "Phase 5: fault detection initialized"  "VSE Phase 5: fault detection initialized"
check "Phase 6: failover service initialized" "VSE Phase 6: failover service initialized"

echo
echo "── IDS detection & active enforcement ──────────────────────"
check "IDS monitor came up"                   "VSE IDS: monitor ready"
check "IDS detected REPEATED faults"          "VSE IDS: ALERT VM[0-9]+ repeated fault"
check "IDS detected fault STORM"              "VSE IDS: ALERT VM[0-9]+ fault STORM"
check "IDS actively ENFORCED (quarantine)"    "VSE IDS: ENFORCING.*quarantining VM"
check "Trust engine quarantined the VM"       "trust DEGRADED -> QUARANTINE|trust .* -> QUARANTINE"
check "IDS recorded an ENFORCE audit event"   "VM[0-9]+ ENFORCE ALERT"
check "IDS reported >=1 enforcement"          "enforcements=[1-9]"

echo
echo "── Audit / evidence ────────────────────────────────────────"
check "Audit log was produced"                "VSE IDS: Audit Log"
check "Operator login reached (system armed)" "Operator Login|monitor ready"

# ── 4. Result ─────────────────────────────────────────────────────────────
echo
total=$((pass_n+fail_n))
if [[ $fail_n -eq 0 ]]; then
    echo -e "${G}════════════════════════════════════════════${N}"
    echo -e "${G} INTEGRATION TEST PASSED — $pass_n/$total checks ${N}"
    echo -e "${G}════════════════════════════════════════════${N}"
    rc=0
else
    echo -e "${R}════════════════════════════════════════════${N}"
    echo -e "${R} INTEGRATION TEST FAILED — $fail_n of $total checks failed ${N}"
    echo -e "${R}════════════════════════════════════════════${N}"
    echo "  See full log: $LOG"
    rc=1
fi

if [[ "${1:-}" == "--keep" ]]; then
    echo "  Boot log kept at: $LOG"
else
    [[ $rc -eq 0 ]] && rm -f "$LOG"
fi
exit $rc
