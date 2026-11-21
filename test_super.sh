#!/bin/bash
# bonus: the supervisor restarts a crashed switch, the MAC table survives

PORT=${1:-9830}
LOG=switch_super.log
OUT=$(mktemp -d)
SOCK=/tmp/vswitch_test_$PORT.sock
SHM=/vswitch_test_$PORT
MACSHM=/vswitch_mactest_$PORT
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
    printf "$2" | ./host 127.0.0.1 "$PORT" "$1" > "$3"
}

has() { grep -qF -- "$2" "$1"; }

# wait until the admin socket answers again
wait_switch() {
    for _ in $(seq 60); do
        $CTL show mac > /dev/null 2>&1 && return 0
        sleep 0.1
    done
    return 1
}

rm -f "$LOG"
./supervisor -M $MACSHM -n 5 -s "$SOCK" -S "$SHM" -l "$LOG" -w 1 "$PORT" 4 > "$OUT/sup" 2>&1 &
SUP=$!
wait_switch
result "switch is running under the supervisor" $?
[ -e "/dev/shm$MACSHM" ]
result "the MAC table is in shared memory" $?

# A and B stay connected, so that their MACs are in the table when it crashes
host $A "send bcast I am A\nsleep 5000\nquit\n" "$OUT/a" &
sleep 0.2
host $B "send bcast I am B\nsleep 5000\nquit\n" "$OUT/b" &
sleep 0.3
$CTL show mac > "$OUT/mac1"
has "$OUT/mac1" "$A" && has "$OUT/mac1" "$B"
result "MACs are learned" $?

# crash it while traffic is flowing
host 02:00:00:00:00:0C "burst bcast 4000 500\nsleep 300\nquit\n" /dev/null &
sleep 0.2
PID1=$(pgrep -P $SUP)
kill -9 "$PID1"
wait_switch
result "the switch is back after kill -9" $?
PID2=$(pgrep -P $SUP)
[ -n "$PID2" ] && [ "$PID2" != "$PID1" ]
result "it is a new process ($PID1 -> $PID2)" $?
has "$OUT/sup" "died from signal 9, starting it again"
result "the supervisor says what happened" $?

$CTL show mac > "$OUT/mac2"
has "$OUT/mac2" "$A" && has "$OUT/mac2" "$B"
result "the MAC table survived the crash" $?
has "$LOG" "MAC table taken over"
result "the new switch says it took the table over" $?

# the restarted switch forwards again
host 02:00:00:00:00:0E "sleep 1500\nquit\n" "$OUT/e" &
sleep 0.3
host 02:00:00:00:00:0F "send bcast after the crash\nsleep 300\nquit\n" /dev/null
sleep 0.3
has "$OUT/e" "after the crash"
result "traffic flows through the restarted switch" $?
$CTL stats | grep -q "^PORT"
result "stats work again (new shared memory object)" $?

# and a second crash, to see it is not a one time thing
kill -9 "$(pgrep -P $SUP)"
wait_switch
result "it survives a second crash" $?

# clean shutdown through the supervisor
kill -INT $SUP
wait $SUP
code=$?
[ $code -eq 0 ]
result "SIGINT: the switch stops and the supervisor exits with 0 (got $code)" $?
[ ! -e "/dev/shm$MACSHM" ] && [ ! -e "/dev/shm$SHM" ]
result "shared memory objects are removed" $?
! has "$OUT/sup" "starting it again" || [ "$(grep -c 'starting it again' "$OUT/sup")" -eq 2 ]
result "it restarted exactly twice, and not after SIGINT" $?
wait

rm -rf "$OUT"
echo "$pass passed, $fail failed (switch output in $LOG)"
[ "$fail" -eq 0 ]
