#!/bin/bash

PORT=${1:-9100}
LOG=switch_test.log
SWITCH=${SWITCH:-./switch}
pass=0
fail=0

check() {
    if grep -qF -- "$2" "$LOG"; then
        echo "PASS  $1"
        pass=$((pass + 1))
    else
        echo "FAIL  $1   (missing: $2)"
        fail=$((fail + 1))
    fi
}

rm -f "$LOG"
$SWITCH -f -l "$LOG" "$PORT" 4 > /dev/null 2>&1 &
SW=$!
sleep "${WARMUP:-0.3}"

printf 'send 02:00:00:00:00:0B hello B\nsleep 1500\nquit\n' | ./host 127.0.0.1 "$PORT" 02:00:00:00:00:0A > /dev/null &
A=$!
sleep 0.2
printf 'send 02:00:00:00:00:0A hello A\nsleep 200\nquit\n' | ./host 127.0.0.1 "$PORT" 02:00:00:00:00:0B > /dev/null
printf 'slow 02:00:00:00:00:0A split up\nsleep 200\nquit\n' | ./host 127.0.0.1 "$PORT" 02:00:00:00:00:0C > /dev/null
printf 'burst bcast 50\nsleep 300\nquit\n' | ./host 127.0.0.1 "$PORT" 02:00:00:00:00:0D > /dev/null
printf 'half 02:00:00:00:00:0A\n' | ./host 127.0.0.1 "$PORT" 02:00:00:00:00:0E > /dev/null
wait "$A"
printf 'vlan 20\npri 0\nsend bcast moved\nsleep 200\nquit\n' | ./host 127.0.0.1 "$PORT" 02:00:00:00:00:0A > /dev/null
printf '\xff\xff\x02\x00\x00\x00\x00\x0F\xff\xff\xff\xff\xff\xff\x00\x01\x01' > /dev/tcp/127.0.0.1/"$PORT"
sleep 0.5

check "frame to unknown MAC floods"    '"hello B" => flood (unknown'
check "learned MAC is forwarded"       '"hello A" => port'
check "byte-by-byte frame reassembled" '"split up" => port'
check "50 back-to-back frames parsed"  '"burst 49" => flood (broadcast'
check "half frame closes the port"     'closing (Protocol error)'
check "port vlan wins, priority decoded"  'vlan 1 pri 0'
check "bad length rejected"            'closing (Protocol error)'
check "switch still running"           'host connected'

if kill -0 "$SW" 2>/dev/null; then
    echo "PASS  switch survived everything"
    pass=$((pass + 1))
else
    echo "FAIL  switch crashed"
    fail=$((fail + 1))
fi

kill "$SW" 2>/dev/null
wait "$SW" 2>/dev/null
echo "$pass passed, $fail failed (switch output in $LOG)"
[ "$fail" -eq 0 ]
