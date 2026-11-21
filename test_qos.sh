#!/bin/bash
# rate limiting (token bucket) and priority scheduling on a live switch

PORT=${1:-9750}
SWITCH=${SWITCH:-./switch}
LOG=switch_qos.log
OUT=$(mktemp -d)
SOCK=/tmp/vswitch_test_$PORT.sock
SHM=/vswitch_test_$PORT
CTL="./swctl -s $SOCK -S $SHM"
R=02:00:00:00:00:0A
S=02:00:00:00:00:0B
X=02:00:00:00:00:0C
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

frames() { grep -c "^recv" "$1"; }

# no aging here, so the order is the plain 5:3:1 weighted round robin
printf 'qos_age = 0\nweights = 5:3:1\n' > "$OUT/conf"

rm -f "$LOG"
$SWITCH -c "$OUT/conf" -s "$SOCK" -S "$SHM" -l "$LOG" "$PORT" 4 > /dev/null 2>&1 &
SW=$!
sleep "${WARMUP:-0.3}"

host $R "sleep 12500\nquit\n" "$OUT/r" &
sleep 0.2
host $S "sleep 1500\nburst bcast 60 20\nsleep 5000\npri 2\nburst bcast 10\npri 0\nburst bcast 10\nsleep 3000\nquit\n" "$OUT/s" &
sleep 0.2
host $X "sleep 12500\nquit\n" "$OUT/x" &
sleep 0.3

$CTL rate 1 20 > "$OUT/rate_reply"
$CTL show ports > "$OUT/ports"
grep -q "limited to 20 frames/s" "$OUT/rate_reply"
result "swctl rate answers" $?
grep -qE "^1 +up +1 +- +20/s " "$OUT/ports"
result "show ports shows the limit" $?

# S sends 60 frames about 2 s after start; look one second later
sleep 2.0
mid_x=$(frames "$OUT/x")
mid_r=$(frames "$OUT/r")
[ "$mid_x" -eq 60 ]
result "the port without a limit got all 60 frames at once ($mid_x)" $?
[ "$mid_r" -ge 10 ] && [ "$mid_r" -le 30 ]
result "limited port: about 20 frames/s after the first second ($mid_r of 60)" $?

sleep 3.0
[ "$(frames "$OUT/r")" -eq 60 ]
result "...and in the end all frames arrive, nothing is lost" $?

# phase two: 10 low priority frames, then 10 high priority ones
$CTL rate 1 10 > /dev/null
sleep 4.3
total=$(frames "$OUT/r")
[ "$total" -eq 80 ]
result "all 20 frames of the second burst arrive ($total in total)" $?

grep -oE "pri [02]" "$OUT/r" > "$OUT/order"
first12_high=$(head -12 "$OUT/order" | grep -c "pri 0")
[ "$first12_high" -ge 8 ]
result "high priority goes first: $first12_high of the first 12 frames are high" $?
[ "$(grep -c "pri 0" "$OUT/order")" -eq 10 ] && [ "$(grep -c "pri 2" "$OUT/order")" -eq 10 ]
result "low priority frames are not starved" $?
last=$(tail -1 "$OUT/order")
[ "$last" = "pri 2" ]
result "the low priority frames are the last ones" $?

$CTL rate 1 0 | grep -q "no rate limit"
result "rate 0 takes the limit away" $?

kill -0 "$SW" 2>/dev/null
result "switch survived" $?
kill -INT "$SW"
wait "$SW"
result "switch exits cleanly" $?
wait

rm -rf "$OUT"
echo "$pass passed, $fail failed (switch output in $LOG)"
[ "$fail" -eq 0 ]
