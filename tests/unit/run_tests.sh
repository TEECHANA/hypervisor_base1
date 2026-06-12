#!/usr/bin/env bash
set -e
PASS=0; FAIL=0
run(){ local n="$1" s="$2"
    gcc -O0 -g -Wall -I. -DUNIT_TEST -DLOG_LEVEL=0 "$s" -o /tmp/hyp_test 2>/dev/null && /tmp/hyp_test
    if [[ $? -eq 0 ]]; then echo "PASS $n"; ((PASS++))
    else echo "FAIL $n"; ((FAIL++)); fi; }
run string_ops    tests/unit/test_string.c
run stage2_basic  tests/unit/test_stage2.c
echo "Passed:$PASS  Failed:$FAIL"
[[ $FAIL -eq 0 ]]
