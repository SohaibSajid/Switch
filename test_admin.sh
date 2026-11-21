#!/bin/bash
# shutdown / up of ports, port semaphore accounting, hosts waiting for a port

PORT=${1:-9700}
SWITCH=${SWITCH:-./switch}
LOG=switch_admin.log
OUT=$(mktemp -d)
SOCK=/tmp/vswitch_test_$PORT.sock
SHM=/vswitch_test_$PORT
CTL="./swctl -s $SOCK -S $SHM"
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

host() {
    printf "$2" | ./host 127.0.0.1 "$PORT" "$1" > "$3"
}

has() { grep -qF -- "$2" "$1"; }

wait_log() {
    for _ in $(seq 50); do
        grep -qF -- "$1" "$LOG" && return 0
        sleep 0.1
    done
    return 1
}

rm -f "$LOG"
$SWITCH -s "$SOCK" -S "$SHM" -l "$LOG" -w 1 "$PORT" 3 > /dev/null 2>&1 &
SW=$!
sleep "${WARMUP:-0.3}"

host $A "send bcast a1\nsleep 9000\nquit\n" "$OUT/a" &
sleep 0.2
host $B "send bcast b1\nsleep 9000\nquit\n" "$OUT/b" &
sleep 0.2
host $C "sleep 9000\nquit\n" "$OUT/c" &
sleep 0.3

# shut down a port with a host on it
$CTL shutdown 2 > "$OUT/r1"
rc=$?
[ $rc -eq 0 ] && has "$OUT/r1" "port 2 is shut down"
result "shutdown <port> works while a host is connected" $?
sleep 0.2
has "$OUT/b" "switch closed the connection"
result "the host on a shut down port is disconnected" $?
$CTL show ports > "$OUT/p1"
grep -qE "^2 +admin-down " "$OUT/p1"
result "show ports says admin-down" $?
$CTL show mac | grep -q "$B"
[ $? -ne 0 ]
result "its MACs are flushed" $?
$CTL shutdown 2 > "$OUT/r2" 2>&1
[ $? -ne 0 ] && has "$OUT/r2" "already shut down"
result "shutting it down twice is an error" $?
$CTL shutdown 9 > /dev/null 2>&1
[ $? -ne 0 ]
result "shutdown of a port that does not exist is an error" $?

# the port is out of the pool: a 4th host finds the switch full
host 02:00:00:00:00:0D "sleep 1800\nquit\n" "$OUT/d"
has "$OUT/d" "switch full"
result "a shut down port is not handed out (switch full)" $?

$CTL up 2 > "$OUT/r3"
[ $? -eq 0 ] && has "$OUT/r3" "port 2 is up"
result "up <port> works" $?
$CTL up 2 > /dev/null 2>&1
[ $? -ne 0 ]
result "bringing up a port that is up is an error" $?
host 02:00:00:00:00:0E "send bcast e1\nsleep 300\nquit\n" "$OUT/e"
has "$OUT/e" "from $A vlan 1 pri 1 len 2: a1" || has "$OUT/a" "len 2: e1"
result "after up a new host gets the port and traffic flows" $?
wait_log "port 2: host connected"
result "the new host was put on port 2" $?

# shut down a port with no host on it
sleep 0.2
$CTL shutdown 2 > /dev/null
result "shutdown of a free port works" $?
host 02:00:00:00:00:0F "sleep 1800\nquit\n" "$OUT/f"
has "$OUT/f" "switch full"
result "a free port that is shut down is not handed out" $?
$CTL up 2 > /dev/null
host 02:00:00:00:00:10 "sleep 100\nquit\n" "$OUT/g"
! has "$OUT/g" "switch full"
result "and after up it is again" $?

# clear mac
$CTL clear mac | grep -q "cleared"
result "clear mac" $?

# many hosts waiting at the same time must not wait one after the other
host 02:00:00:00:00:11 "sleep 7000\nquit\n" "$OUT/h" &
sleep 0.3
start=$(date +%s.%N)
for i in $(seq 1 12); do
    host 02:00:00:00:01:$(printf %02x $i) "sleep 1800\nquit\n" "$OUT/w$i" &
done
for i in $(seq 1 12); do
    for _ in $(seq 100); do
        has "$OUT/w$i" "switch full" && break
        sleep 0.1
    done
done
end=$(date +%s.%N)
took=$(echo "$end - $start" | bc)
[ "$(echo "$took < 4" | bc)" -eq 1 ]
result "12 hosts waiting for a port are all answered within 4 s ($took s)" $?
all=0
for i in $(seq 1 12); do has "$OUT/w$i" "switch full" && all=$((all + 1)); done
[ $all -eq 12 ]
result "all of them got switch full" $?

kill -0 "$SW" 2>/dev/null
result "switch survived" $?
kill -INT "$SW"
wait "$SW"
result "switch exits cleanly" $?
for _ in $(seq 20); do [ ! -e "/dev/shm$SHM" ] && break; sleep 0.1; done
[ ! -e "/dev/shm$SHM" ]
result "shared memory is gone" $?
kill $(jobs -p) 2>/dev/null
wait 2>/dev/null

rm -rf "$OUT"
echo "$pass passed, $fail failed (switch output in $LOG)"
[ "$fail" -eq 0 ]
