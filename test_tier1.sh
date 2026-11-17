#!/bin/bash

PORT=${1:-9500}
SWITCH=${SWITCH:-./switch}
LOG=switch_tier1.log
OUT=$(mktemp -d)
pass=0
fail=0

result() {
    if [ "$2" -eq 0 ]; then
        echo "PASS  $1"
        pass=$((pass + 1))
    else
        echo "FAIL  $1"
        fail=$((fail + 1))
    fi
}

host() {
    printf "$2" | ./host 127.0.0.1 "$PORT" "$1" > "$3"
}

$SWITCH -a 2 -m 3 -w 1 "$PORT" 2 > "$LOG" 2>&1 &
SW=$!
sleep "${WARMUP:-0.3}"

host 02:00:00:00:00:0A "send bcast hi\nsleep 3500\nquit\n" "$OUT/a" &
HA=$!
sleep 0.2
host 02:00:00:00:00:0B "sleep 2500\nquit\n" "$OUT/b" &
HB=$!
sleep 0.2
host 02:00:00:00:00:0C "sleep 1500\nquit\n" "$OUT/c"
wait $HB
host 02:00:00:00:00:0D "send bcast got in\nsleep 200\nquit\n" "$OUT/d"
sleep 0.2

host 02:00:00:00:01:01 "send bcast m1\nsrc 02:00:00:00:01:02\nsend bcast m2\nsrc 02:00:00:00:01:03\nsend bcast m3\nsrc 02:00:00:00:01:04\nsend bcast m4\nsleep 300\nquit\n" "$OUT/e"
wait $HA
sleep 0.3

grep -q "recv from 00:00:00:00:00:00 .*: switch full" "$OUT/c"
result "third host gets switch full reply" $?
grep -q "switch full, rejecting connection" "$LOG"
result "switch logs the rejection" $?
grep -q '"got in" =>' "$LOG"
result "host gets a port after one frees up" $?
grep -q "port security, more than 3 MACs" "$LOG"
result "port security shuts the port down" $?
! grep -q '"m4" =>' "$LOG"
result "frame from the 4th MAC is not forwarded" $?
grep -q "MAC aged out: 02:00:00:00:00:0A" "$LOG"
result "idle MAC ages out" $?
kill -0 "$SW" 2>/dev/null
result "switch survived" $?

kill "$SW" 2>/dev/null
wait "$SW" 2>/dev/null
rm -rf "$OUT"
echo "$pass passed, $fail failed (switch output in $LOG)"
[ "$fail" -eq 0 ]
