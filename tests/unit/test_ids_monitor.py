#!/usr/bin/env python3
"""
test_ids_monitor.py — unit coverage for scripts/ids_monitor.py (headless path).

Drives the monitor's headless parser (`--headless --once`) against a REAL
captured runtime log, run_current.log, and asserts it correctly parses
the organic VM2 attack chain: the IDS storm/enforcement quarantine AND the
Phase 6 failover (restart from backup OS -> recovery).

This is host-side only — it boots nothing and compiles nothing; it exercises the
exact code path a user gets from `python3 scripts/ids_monitor.py --headless`.
The log is a fixture: the same capture the integration suite asserts against.

Run standalone (exit 0 = pass) or via `make test-unit` (wired into
tests/unit/run_tests.sh).
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))          # tests/unit -> repo root
MONITOR = os.path.join(ROOT, "scripts", "ids_monitor.py")
LOG = os.path.join(ROOT, "run_current.log")

_fails = []


def check(cond, msg):
    if cond:
        print(f"  ok  : {msg}")
    else:
        print(f"  FAIL: {msg}")
        _fails.append(msg)


def main():
    # Sanity: fixture + script must exist before we can prove anything.
    if not os.path.isfile(MONITOR):
        print(f"FAIL: monitor not found: {MONITOR}", file=sys.stderr)
        return 1
    if not os.path.isfile(LOG):
        print(f"FAIL: captured log fixture not found: {LOG}", file=sys.stderr)
        return 1

    # Run the monitor exactly as an operator would in headless mode.
    proc = subprocess.run(
        [sys.executable, MONITOR, "--headless", "--once", LOG],
        capture_output=True, text=True,
    )
    out = proc.stdout
    print("=== ids_monitor --headless --once run_current.log ===")
    check(proc.returncode == 0, f"exit code 0 (got {proc.returncode})")
    if proc.stderr.strip():
        print("  (stderr)\n" + proc.stderr)

    lines = out.splitlines()

    def has(sub):
        return any(sub in ln for ln in lines)

    # ── VM2 IDS quarantine chain ──
    check(has("[ALERT] VM2 fault STORM"),
          "IDS storm alert on VM2 parsed")
    check(has("[ENFORCE] quarantining VM2"),
          "IDS enforcement (quarantine) on VM2 parsed")
    check(has("[QUARANTINE] VM2"),
          "VM2 quarantine event parsed")
    check(has("[TRUST] VM2 DEGRADED -> QUARANTINE"),
          "VM2 trust transition into QUARANTINE parsed")

    # ── Phase 6 failover chain (the point of this coverage) ──
    check(has("[FAILOVER] VM2 attempt 1 restart from backup OS"),
          "VM2 failover attempt 1 (restart from backup OS) parsed")
    check(has("[FAILOVER] VM2 recovered via backup OS (attempt 1)"),
          "VM2 recovery via backup OS parsed")
    check(has("[TRUST] VM2 QUARANTINE -> DEGRADED"),
          "VM2 trust transition back out of QUARANTINE (post-restore) parsed")
    check(has("failovers=1"),
          "summary reports exactly one failover")

    # ── Ordering: quarantine must precede failover recovery ──
    def idx(sub):
        for i, ln in enumerate(lines):
            if sub in ln:
                return i
        return -1
    q_i = idx("[QUARANTINE] VM2")
    fo_i = idx("[FAILOVER] VM2 attempt 1")
    rec_i = idx("recovered via backup OS")
    check(-1 < q_i < fo_i < rec_i,
          f"sequence quarantine({q_i}) -> failover({fo_i}) -> recover({rec_i})")

    # ── Negative: only VM2 is attacked; VM1/VM3 stay clean ──
    check(not any("QUARANTINE] VM1" in ln or "QUARANTINE] VM3" in ln
                  for ln in lines),
          "no spurious quarantine of VM1 or VM3")
    check(not any("FAILOVER] VM1" in ln or "FAILOVER] VM3" in ln
                  for ln in lines),
          "no spurious failover of VM1 or VM3")

    if _fails:
        print(f"\n{len(_fails)} assertion(s) FAILED")
        print("---- full output ----")
        print(out)
        return 1
    print("\nAll ids_monitor headless assertions passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
