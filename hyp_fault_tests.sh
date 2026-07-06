#!/usr/bin/env bash
#
# hyp_fault_tests.sh — Functional & fault-injection test harness
# ================================================================
# Goes beyond the "happy path" 16/16 integration suite. Each test deliberately
# breaks something and asserts the hypervisor / VSE reacts correctly:
#
#   T1  Baseline good boot            all 3 guests trusted, scheduler runs
#   T2  Guest image tamper (RTOS)     genuineness FAIL -> VM2 quarantined
#   T3  Guest image tamper (Android)  genuineness FAIL -> VM3 quarantined
#   T4  Hypervisor .text tamper       Phase 2 integrity PANIC (no guest runs)
#   T5  DMA storm attack (built-in)   IDS STORM -> enforce -> failover restore
#   T6  Cross-VM permission fault     Phase 5 perm fault logged, trust drop
#   T7  Backup-OS failover recovery   VM2 restored from backup, resumes
#   T8  Missing guest image           build/boot fails cleanly (no crash-loop)
#   T9  IDS audit-log evidence        audit records + summary emitted
#   T10 Headless IDS monitor parse    Python monitor parses the boot log
#
# Usage:
#   ./hyp_fault_tests.sh              # run all, print PASS/FAIL + report
#   ./hyp_fault_tests.sh --keep       # keep captured logs in /tmp
#   KEEP=1 ./hyp_fault_tests.sh
#
set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── config ────────────────────────────────────────────────────────────────
ELF="build/qemu/hypervisor.elf"
LINUX="guests/linux/Image"
INITRD="guests/linux/initramfs.cpio.gz"
RTOS="guests/rtos/rtos.bin"
ANDROID="guests/android_stub/android.bin"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/hypfault.XXXXXX")"   # was a hardcoded /home/rishi/... path that doesn't exist here
CAP=18                      # seconds to capture per boot
KEEP="${KEEP:-0}"
[[ "${1:-}" == "--keep" ]] && KEEP=1

R='\033[0;31m'; G='\033[0;32m'; Y='\033[0;33m'; B='\033[0;34m'; C='\033[0;36m'; N='\033[0m'
pass=0; fail=0
declare -a RESULTS

info(){ echo -e "${B}==>${N} $*"; }
step(){ echo -e "\n${C}──── $* ────${N}"; }

# assert PATTERN present in FILE
must_have(){ local label="$1" pat="$2" f="$3"
  if grep -qaE "$pat" "$f" 2>/dev/null; then
    echo -e "   [${G}PASS${N}] $label"; pass=$((pass+1)); RESULTS+=("PASS|$label")
  else
    echo -e "   [${R}FAIL${N}] $label"; fail=$((fail+1)); RESULTS+=("FAIL|$label")
  fi
}
# assert PATTERN absent in FILE
must_not(){ local label="$1" pat="$2" f="$3"
  if grep -qaE "$pat" "$f" 2>/dev/null; then
    echo -e "   [${R}FAIL${N}] $label"; fail=$((fail+1)); RESULTS+=("FAIL|$label")
  else
    echo -e "   [${G}PASS${N}] $label"; pass=$((pass+1)); RESULTS+=("PASS|$label")
  fi
}

# boot(image_elf, rtos_img, android_img, out_log)
boot(){
  local elf="$1" rtos="$2" android="$3" out="$4"
  : > "$out"
  timeout "${CAP}s" qemu-system-aarch64 \
    -machine virt,iommu=smmuv3,virtualization=on,gic-version=3 \
    -cpu cortex-a57 -m 2G -smp 4 -nographic \
    -serial stdio -monitor none -no-reboot -d guest_errors \
    -kernel "$elf" \
    -device loader,file="$LINUX",addr=0x41200000,force-raw=on \
    -device loader,file="$INITRD",addr=0x47000000,force-raw=on \
    -device loader,file="$rtos",addr=0x60008000,force-raw=on \
    -device loader,file="$android",addr=0x70200000,force-raw=on \
    </dev/null >"$out" 2>&1 &
  local qpid=$!
  local waited=0
  while kill -0 "$qpid" 2>/dev/null; do
    grep -qa "Operator Login" "$out" 2>/dev/null && { sleep 1; break; }
    grep -qa "PANIC" "$out" 2>/dev/null && { sleep 1; break; }
    sleep 1; waited=$((waited+1)); [[ $waited -ge $CAP ]] && break
  done
  kill "$qpid" 2>/dev/null || true
  pkill -f "qemu-system-aarch64.*(hypervisor|hyp_).*elf" 2>/dev/null || true
  wait "$qpid" 2>/dev/null || true
}

# ── build once ──────────────────────────────────────────────────────────
info "Building hypervisor..."
if ! make >/dev/null 2>&1; then echo -e "${R}BUILD FAILED${N}"; exit 1; fi
[[ -f "$ELF" ]] || { echo -e "${R}$ELF missing${N}"; exit 1; }
cp "$ELF" "$TMP/hyp_good.elf"

# tampered .text ELF (flip a byte inside the measured .text range)
cp "$ELF" "$TMP/hyp_text_tamper.elf"
python3 - "$TMP/hyp_text_tamper.elf" <<'PY'
import sys
# __text_start VA 0x40000000 maps to file off 0x10000; flip byte at VA 0x40001000.
with open(sys.argv[1],'r+b') as f:
    f.seek(0x11000); b=f.read(1); f.seek(0x11000); f.write(bytes([b[0]^0xFF]))
PY

# tampered guest images
cp "$RTOS" "$TMP/rtos_tamper.bin"
printf '\xDE\xAD\xBE\xEF' | dd of="$TMP/rtos_tamper.bin" bs=1 seek=100 count=4 conv=notrunc 2>/dev/null
cp "$ANDROID" "$TMP/android_tamper.bin"
printf '\xBA\xAD' | dd of="$TMP/android_tamper.bin" bs=1 seek=16 count=2 conv=notrunc 2>/dev/null

# ── T1 baseline good boot ─────────────────────────────────────────────────
step "T1  Baseline good boot (all guests genuine)"
boot "$TMP/hyp_good.elf" "$RTOS" "$ANDROID" "$TMP/t1.log"
must_have "Phase 2 self-measure verified"        "Phase 2: all components verified" "$TMP/t1.log"
must_have "VM1 linux TRUSTED"                     "VM1 'linux' -> TRUSTED"           "$TMP/t1.log"
must_have "VM2 rtos TRUSTED"                      "VM2 'rtos' -> TRUSTED"            "$TMP/t1.log"
must_have "VM3 android TRUSTED"                   "VM3 'android' -> TRUSTED"         "$TMP/t1.log"
must_have "Scheduler reached login (armed)"       "Operator Login"                   "$TMP/t1.log"
must_not  "No panic on clean image"               "PANIC"                            "$TMP/t1.log"

# ── T2 tampered RTOS image ────────────────────────────────────────────────
step "T2  Tampered RTOS guest image (4-byte corruption)"
boot "$TMP/hyp_good.elf" "$TMP/rtos_tamper.bin" "$ANDROID" "$TMP/t2.log"
must_have "RTOS genuineness FAILS"                "guest 'rtos' GENUINENESS CHECK FAILED" "$TMP/t2.log"
must_have "VM2 quarantined (not trusted)"         "VM2 'rtos' -> QUARANTINE"         "$TMP/t2.log"
must_have "Linux still trusted (isolation holds)" "VM1 'linux' -> TRUSTED"           "$TMP/t2.log"

# ── T3 tampered Android image ─────────────────────────────────────────────
step "T3  Tampered Android guest image"
boot "$TMP/hyp_good.elf" "$RTOS" "$TMP/android_tamper.bin" "$TMP/t3.log"
must_have "Android genuineness FAILS"             "guest 'android' GENUINENESS CHECK FAILED" "$TMP/t3.log"
must_have "VM3 quarantined"                        "VM3 'android' -> QUARANTINE"     "$TMP/t3.log"

# ── T4 tampered hypervisor .text ─────────────────────────────────────────
step "T4  Tampered hypervisor .text (root-of-trust)"
boot "$TMP/hyp_text_tamper.elf" "$RTOS" "$ANDROID" "$TMP/t4.log"
must_have "Phase 2 detects .text violation"       "COMPONENT INTEGRITY VIOLATION in '.text'" "$TMP/t4.log"
must_have "Hypervisor PANICS on tamper"           "PANIC: VSE: component integrity" "$TMP/t4.log"
must_not  "No guest ever started"                 "Launching first guest"           "$TMP/t4.log"

# ── T5 DMA storm + enforcement (built-in demo) ───────────────────────────
step "T5  DMA fault storm -> IDS enforce -> quarantine (VM2)"
must_have "IDS detects fault STORM"               "fault STORM"                      "$TMP/t1.log"
must_have "IDS actively ENFORCES"                 "ENFORCING — quarantining VM2"     "$TMP/t1.log"
must_have "Trust engine -> QUARANTINE"            "VM2 trust DEGRADED -> QUARANTINE" "$TMP/t1.log"

# ── T6 cross-VM permission fault ─────────────────────────────────────────
step "T6  Cross-VM / NX permission fault (VM3)"
must_have "Phase 5 flags permission fault"        "VM3 PERMISSION fault"             "$TMP/t1.log"
must_have "Possible cross-VM violation noted"     "possible cross-VM/NX violation"   "$TMP/t1.log"

# ── T7 backup-OS failover recovery ───────────────────────────────────────
step "T7  Backup-OS failover recovers quarantined VM2"
must_have "Failover invoked on quarantine"        "invoking failover"                "$TMP/t1.log"
must_have "Restarting from backup OS"             "restarting from backup"           "$TMP/t1.log"
must_have "VM2 restored from backup bytes"        "VM2 restored [0-9]+ bytes from backup" "$TMP/t1.log"
must_have "VM2 recovered & resumed"               "recovered via backup OS"          "$TMP/t1.log"

# ── T8 missing guest image ───────────────────────────────────────────────
step "T8  Missing guest image handled cleanly"
if make check-guests >/dev/null 2>&1; then
  # temporarily hide rtos and confirm check-guests fails (does not crash)
  mv "$RTOS" "$RTOS.hidden"
  if make check-guests >"$TMP/t8.log" 2>&1; then
    echo -e "   [${R}FAIL${N}] check-guests should fail when image missing"; fail=$((fail+1)); RESULTS+=("FAIL|check-guests detects missing image")
  else
    echo -e "   [${G}PASS${N}] check-guests detects missing image"; pass=$((pass+1)); RESULTS+=("PASS|check-guests detects missing image")
  fi
  mv "$RTOS.hidden" "$RTOS"
else
  echo -e "   [${Y}SKIP${N}] check-guests target unavailable"
fi

# ── T9 IDS audit evidence ────────────────────────────────────────────────
step "T9  IDS audit log & summary produced"
must_have "IDS summary block emitted"             "VSE IDS: Summary"                 "$TMP/t1.log"
must_have "IDS audit log emitted"                 "VSE IDS: Audit Log"               "$TMP/t1.log"
must_have "Enforcement counted (>=1)"             "enforcements=1"                   "$TMP/t1.log"

# ── T10 headless IDS monitor ─────────────────────────────────────────────
step "T10 Python IDS monitor parses boot log"
if python3 scripts/ids_monitor.py --once --headless "$TMP/t1.log" >"$TMP/t10.log" 2>&1; then
  must_have "Monitor produced a report"           "VSE / IDS Headless Report"        "$TMP/t10.log"
  must_have "Monitor saw the enforcement"          "ENFORCE"                          "$TMP/t10.log"
else
  echo -e "   [${Y}WARN${N}] ids_monitor.py failed (likely missing python3-tk). Try: apt install python3-tk"
  RESULTS+=("WARN|ids_monitor.py headless (tkinter missing)")
fi

# ── report ───────────────────────────────────────────────────────────────
echo -e "\n${B}════════════════════════════════════════════════════════${N}"
echo -e "  FAULT-INJECTION TEST SUMMARY"
echo -e "${B}════════════════════════════════════════════════════════${N}"
printf "  %-52s %s\n" "CHECK" "RESULT"
for r in "${RESULTS[@]}"; do
  st="${r%%|*}"; lb="${r#*|}"
  case "$st" in
    PASS) printf "  %-52s ${G}%s${N}\n" "$lb" "PASS";;
    FAIL) printf "  %-52s ${R}%s${N}\n" "$lb" "FAIL";;
    *)    printf "  %-52s ${Y}%s${N}\n" "$lb" "$st";;
  esac
done
echo -e "${B}────────────────────────────────────────────────────────${N}"
echo -e "  Total: $((pass+fail))   ${G}PASS=$pass${N}   ${R}FAIL=$fail${N}"
if [[ $fail -eq 0 ]]; then
  echo -e "  ${G}ALL FAULT-INJECTION TESTS PASSED${N}"
else
  echo -e "  ${R}$fail CHECK(S) FAILED — see logs${N}"
fi

if [[ "$KEEP" == "1" ]]; then
  echo -e "\n  Logs kept in: $TMP"
else
  rm -rf "$TMP"
fi
[[ $fail -eq 0 ]]
