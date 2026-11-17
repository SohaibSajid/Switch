#!/bin/bash

PORT=${1:-9200}
SWITCH=${SWITCH:-./switch}
LOG=switch_fwd.log
OUT=$(mktemp -d)
A=02:00:00:00:00:0A
B=02:00:00:00:00:0B
C=02:00:00:00:00:0C
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

has() { grep -qF -- "$2" "$1"; }

host() {
    local mac=$1 script=$2 out=$3
    printf "$script" | ./host 127.0.0.1 "$PORT" "$mac" > "$out"
}

$SWITCH "$PORT" 8 > "$LOG" 2>&1 &
SW=$!
sleep "${WARMUP:-0.3}"

host $B "sleep 300\nsend $A hello A\nsleep 4000\nquit\n" "$OUT/b" &
HB=$!
host $C "sleep 4300\nquit\n" "$OUT/c" &
HC=$!
sleep 0.1
host $A "sleep 100\nsend $B hello B\nsleep 4000\nquit\n" "$OUT/a" &
HA=$!
sleep 0.6

exec 3<>/dev/tcp/127.0.0.1/"$PORT"
sleep 0.2
host 02:00:00:00:00:0D "burst bcast 6000 1400\nsleep 300\nquit\n" /dev/null
host 02:00:00:00:00:0E "send $A after flood\nsleep 300\nquit\n" /dev/null
sleep 0.3
exec 3>&-

wait $HA $HB $HC
host 02:00:00:00:00:0F "send $A to gone A\nsleep 300\nquit\n" /dev/null
sleep 0.3

has "$OUT/b" "recv from $A vlan 1 pri 1 len 7: hello B"
result "unknown destination is flooded to B" $?
has "$OUT/c" "hello B"
result "unknown destination is flooded to C" $?
has "$OUT/a" "recv from $B vlan 1 pri 1 len 7: hello A"
result "learned destination reaches A" $?
! has "$OUT/c" "hello A"
result "unicast is not seen by C" $?
has "$OUT/a" "after flood"
result "silent host does not block others" $?
grep -qE "closed, [1-9][0-9]* frames dropped" "$LOG"
result "full queue drops and counts frames" $?
has "$LOG" "1 MACs flushed"
result "MACs flushed when a host leaves" $?
has "$LOG" '"to gone A" => flood (unknown'
result "frame to departed host is flooded" $?
kill -0 "$SW" 2>/dev/null
result "switch survived" $?

kill "$SW" 2>/dev/null
wait "$SW" 2>/dev/null
rm -rf "$OUT"
echo "$pass passed, $fail failed (switch output in $LOG)"
[ "$fail" -eq 0 ]
