#!/usr/bin/env bash
#
# Host-side unit tests (run with: make test-unit).
#
# Compiled with -DUNIT_TEST so include/types.h defers size_t/uintptr_t/intptr_t/
# bool to the standard libc headers instead of its freestanding typedefs — this
# avoids the size_t redefinition clash against <stdio.h> et al.
#
# NOTE: no `set -e` here — a failing test must be counted and reported, not abort
# the whole run. (The old version also tripped over `((FAIL++))` returning a
# non-zero status when the counter was 0.)

CC=${CC:-gcc}
CFLAGS="-O0 -g -Wall -I. -DUNIT_TEST -DLOG_LEVEL=0"
BIN="$(mktemp -u /tmp/hyp_test.XXXXXX)"
CCLOG="$(mktemp)"; RUNLOG="$(mktemp)"
PASS=0; FAIL=0

run(){ local n="$1" s="$2"
    if [[ ! -f "$s" ]]; then echo "SKIP $n (missing $s)"; return; fi
    if $CC $CFLAGS "$s" -o "$BIN" 2>"$CCLOG" && "$BIN" >"$RUNLOG" 2>&1; then
        echo "PASS $n"; PASS=$((PASS + 1))
    else
        echo "FAIL $n"; FAIL=$((FAIL + 1))
        sed 's/^/    /' "$CCLOG" "$RUNLOG" 2>/dev/null | head -6
    fi
}

run string_ops       tests/unit/test_string.c
run stage2_basic     tests/unit/test_stage2_phase1.c
run phase2_modules   tests/unit/test_phase2.c
run phase3_sched_ipc tests/unit/test_phase3.c
run phase4a_pmu      tests/unit/test_phase4a.c
run phase4b          tests/unit/test_phase4b.c
run phase4c          tests/unit/test_phase4c.c
run phase4d          tests/unit/test_phase4d.c

echo "Passed:$PASS  Failed:$FAIL"
rm -f "$BIN" "$CCLOG" "$RUNLOG"
[[ $FAIL -eq 0 ]]
