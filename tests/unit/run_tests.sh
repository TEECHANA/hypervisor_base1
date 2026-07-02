#!/usr/bin/env bash
#
# Unit-test harness. Compiles each self-contained test with the HOST toolchain
# and runs it. Tests that pull in the freestanding hypervisor headers include
# tests/unit/host_shim.h FIRST to reconcile the hypervisor's size_t/uintptr_t
# typedefs with the host libc headers (see host_shim.h and test_string.c). The
# hypervisor source itself is never modified for tests.
#
# NOTE: no `set -e` here — we want to run ALL tests and tally pass/fail rather
# than abort on the first failure. (bash `((PASS++))` returns non-zero when the
# pre-increment value is 0, which under `set -e` would abort even on success.)

PASS=0; FAIL=0

run(){
    local n="$1" s="$2"
    if gcc -O0 -g -Wall -I. -DUNIT_TEST -DLOG_LEVEL=0 "$s" \
           -o /tmp/hyp_test 2>/tmp/hyp_test_cc.log && /tmp/hyp_test; then
        echo "PASS $n"; PASS=$((PASS+1))
    else
        echo "FAIL $n"; FAIL=$((FAIL+1))
        sed 's/^/    /' /tmp/hyp_test_cc.log
    fi
}

run string_ops    tests/unit/test_string.c
run stage2_basic  tests/unit/test_stage2_phase1.c
echo "Passed:$PASS  Failed:$FAIL"
[[ $FAIL -eq 0 ]]
