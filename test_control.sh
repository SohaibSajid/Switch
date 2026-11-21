#!/bin/bash

PORT=${1:-9900}
SWITCH=${SWITCH:-./switch}
LOG=switch_ctl.log
OUT=$(mktemp -d)
SOCK=/tmp/vswitch_test_$PORT.sock
SHM=/vswitch_test_$PORT
CTL="./swctl -s $SOCK -S $SHM"
A=02:00:00:00:00:0A
B=02:00:00:00:00:0B
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
    printf "$2" | ./host 127.0.0.1 "$PORT" "$1" > /dev/null
}

rm -f "$LOG"
$SWITCH -s "$SOCK" -S "$SHM" -l "$LOG" "$PORT" 4 > /dev/null 2>&1 &
SW=$!
sleep "${WARMUP:-0.3}"

[ -S "$SOCK" ]
result "admin UNIX socket created" $?
[ -e "/dev/shm${SHM}" ]
result "shared memory object created" $?

host $B "send bcast hello\nsleep 6000\nquit\n" &
HB=$!
sleep 0.2
host $A "send $B one\nsend $B two\nsend $B three\nsleep 5800\nquit\n" &
HA=$!
sleep 0.2
host 02:00:00:00:00:0C "send $A hi A\nsleep 200\nquit\n"
sleep 0.3

$CTL show mac > "$OUT/mac"
grep -q "$A" "$OUT/mac" && grep -q "$B" "$OUT/mac"
result "show mac lists learned MACs" $?

$CTL show ports > "$OUT/ports"
grep -qE "^1 +up +1 +- +- +1 +0 +1 +3 +0 +0" "$OUT/ports"
result "show ports: port 1 (B) got 3 frames from A" $?
grep -qE "^2 +up +1 +- +- +1 +0 +3 +1 +0 +0" "$OUT/ports"
result "show ports: port 2 (A) sent 3, received 1" $?

$CTL stats > "$OUT/stats"
grep -qE "^2 +up +3 +1 +0 +0" "$OUT/stats"
result "stats reads counters from shared memory" $?

timeout 2.5 $CTL stats --watch > "$OUT/watch"
[ "$(grep -c '^PORT' "$OUT/watch")" -ge 2 ]
result "stats --watch refreshes every second" $?

$CTL clear mac > "$OUT/clear"
grep -q "cleared 2 MAC entries" "$OUT/clear"
result "clear mac empties the table" $?
$CTL show mac | grep -q "^0 entries"
result "table is empty after clear" $?

$CTL bogus > "$OUT/bad"
[ $? -ne 0 ] && grep -q "error: unknown command" "$OUT/bad"
result "unknown command is an error" $?

kill -INT "$SW"
wait "$SW"
result "switch exits cleanly" $?
wait $HA $HB

[ ! -e "$SOCK" ]
result "admin socket unlinked on shutdown" $?
[ ! -e "/dev/shm${SHM}" ]
result "shared memory unlinked on shutdown" $?
! $CTL show mac 2> /dev/null
result "swctl reports switch not running" $?

rm -rf "$OUT"
echo "$pass passed, $fail failed (switch output in $LOG)"
[ "$fail" -eq 0 ]
