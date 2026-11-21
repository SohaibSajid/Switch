#!/bin/bash
# Runs the unit tests and every test script, one after the other.
#
#   ./run_tests.sh              the normal build
#   SWITCH=... SWITCH_BIN=... ./run_tests.sh
#                               another build, for example the sanitizer one
#
# SWITCH is the command the test scripts start the switch with (it may have
# a prefix like "valgrind -q"), SWITCH_BIN is a single executable, which
# switch_pair and supervisor start. See README.md for the TSAN and valgrind
# commands.

export SWITCH=${SWITCH:-./switch}
export SWITCH_BIN=${SWITCH_BIN:-./switch}

failed=0

echo "=== unit tests"
if ${UNIT:-./test_main} > /dev/null 2>&1; then
    echo "all unit tests passed"
else
    echo "UNIT TESTS FAILED (run ./test_main to see which)"
    failed=$((failed + 1))
fi

for t in test_proto test_forward test_tier1 test_signals test_control \
         test_vlan test_admin test_qos test_link test_super test_stress; do
    echo "=== $t"
    out=$(./$t.sh 2>&1)
    status=$?
    echo "$out" | grep -v "^PASS"
    if [ $status -ne 0 ]; then
        failed=$((failed + 1))
    fi
done

if [ "$failed" -eq 0 ]; then
    echo "ALL TESTS PASSED"
else
    echo "$failed test group(s) failed"
fi
[ "$failed" -eq 0 ]
