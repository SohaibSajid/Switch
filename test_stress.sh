#!/bin/bash
# Traffic from many hosts while the admin keeps changing things and signals
# arrive. Meant to be run with SWITCH=./switch_tsan and under valgrind too.

PORT=${1:-9850}
SWITCH=${SWITCH:-./switch}
ROUNDS=${ROUNDS:-40}
LOG=switch_stress.log
ERR=switch_stress.err
OUT=$(mktemp -d)
SOCK=/tmp/vswitch_test_$PORT.sock
SHM=/vswitch_test_$PORT
CTL="./swctl -s $SOCK -S $SHM"
CFG=$OUT/switch.conf
DUMP=$OUT/macs.txt
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

mac() { printf "02:00:00:00:00:%02X" "$1"; }

# a host that keeps sending: broadcasts and unicast to the next host
sender() {
    local n=$1 peer s=""
    peer=$(mac $(( n % 5 + 1 )))
    for _ in $(seq 6); do
        s+="burst bcast 150 100\nburst $peer 150 300\nsleep 200\n"
    done
    s+="sleep 300\nquit\n"
    printf "$s" | ./host 127.0.0.1 "$PORT" "$(mac "$n")" > /dev/null
}

printf 'age = 30\nmax_macs = 500\nmacs_per_port = 8\nwait = 1\nrate = 0\n' > "$CFG"

rm -f "$LOG" "$ERR"
$SWITCH -c "$CFG" -d "$DUMP" -s "$SOCK" -S "$SHM" -l "$LOG" "$PORT" 8 > /dev/null 2> "$ERR" &
SW=$!
sleep "${WARMUP:-0.5}"

for n in 1 2 3 4 5; do
    sender $n &
    sleep 0.1
done

# port 6 connects and never reads, port 7 writes a byte at a time
exec 3<>/dev/tcp/127.0.0.1/"$PORT"
printf 'slow bcast hello from the slow one\nsleep 500\nslow bcast and again\nquit\n' |
    ./host 127.0.0.1 "$PORT" "$(mac 7)" > /dev/null &
SLOW=$!

# hosts that come and go on the last port
churn() {
    for i in $(seq 8); do
        printf 'send bcast hi\nsleep 50\nquit\n' | ./host 127.0.0.1 "$PORT" "$(mac $((20 + i)))" > /dev/null
    done
}
churn &

# admin and signals
ok=0
for i in $(seq "$ROUNDS"); do
    case $((i % 10)) in
        0) $CTL vlan 3 20 > /dev/null; $CTL vlan 1 20 > /dev/null ;;
        1) $CTL vlan 3 1 > /dev/null; $CTL vlan 1 1 > /dev/null ;;
        2) $CTL mirror 2 8 > /dev/null; $CTL rate 1 300 > /dev/null ;;
        3) $CTL mirror 2 off > /dev/null; $CTL rate 1 0 > /dev/null ;;
        4) $CTL shutdown 4 > /dev/null ;;
        5) $CTL up 4 > /dev/null; $CTL clear mac > /dev/null ;;
        6) $CTL trunk 5 > /dev/null; kill -USR1 "$SW" ;;
        7) $CTL trunk off > /dev/null; $CTL show ports > /dev/null ;;
        8) printf 'age = 30\nmax_macs = 500\nmacs_per_port = 8\nwait = 1\nrate = %d\nvlan.3 = %d\n' \
               $((i * 7)) $((i % 3 + 1)) > "$CFG"
           kill -HUP "$SW" ;;
        9) $CTL show mac > /dev/null; $CTL stats > /dev/null ;;
    esac
    kill -0 "$SW" 2>/dev/null || break
    ok=$((ok + 1))
    sleep 0.1
done

[ "$ok" -eq "$ROUNDS" ]
result "switch survived $ROUNDS rounds of admin commands and signals under load" $?

wait $SLOW
$CTL show ports > "$OUT/ports" 2>&1
[ $? -eq 0 ] && [ "$(grep -c '^[0-9]' "$OUT/ports")" -eq 8 ]
result "admin tool still answers" $?

kill -INT "$SW"
for _ in $(seq 300); do
    kill -0 "$SW" 2>/dev/null || break
    sleep 0.1
done
! kill -0 "$SW" 2>/dev/null
result "SIGINT stops the switch" $?
wait "$SW"
code=$?
[ "$code" -eq 0 ]
result "exit status 0 (got $code)" $?
exec 3>&-
wait

grep -q "switch stopped cleanly" "$LOG"
result "shutdown completed" $?
! grep -qE "ThreadSanitizer|ERROR SUMMARY: [1-9]|Invalid (read|write)|definitely lost|Segmentation" "$ERR"
result "no sanitizer or valgrind reports" $?
[ ! -e "/dev/shm$SHM" ] && [ ! -e "$SOCK" ]
result "socket and shared memory removed" $?

rm -rf "$OUT"
echo "$pass passed, $fail failed (switch output in $LOG, errors in $ERR)"
[ "$fail" -eq 0 ]
