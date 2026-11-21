#!/bin/bash
# VLANs, trunk port and port mirroring on a running switch

PORT=${1:-9600}
SWITCH=${SWITCH:-./switch}
LOG=switch_vlan.log
OUT=$(mktemp -d)
SOCK=/tmp/vswitch_test_$PORT.sock
SHM=/vswitch_test_$PORT
CTL="./swctl -s $SOCK -S $SHM"
A=02:00:00:00:00:0A
B=02:00:00:00:00:0B
C=02:00:00:00:00:0C
D=02:00:00:00:00:0D
T=02:00:00:00:00:0E
S=02:00:00:00:00:0F
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

rm -f "$LOG"
$SWITCH -s "$SOCK" -S "$SHM" -l "$LOG" -w 1 "$PORT" 6 > /dev/null 2>&1 &
SW=$!
sleep "${WARMUP:-0.3}"

# ports 1..6 in the order the hosts connect
host $A "sleep 2500\nsend bcast bc10\nsend $C cross\nsend $B same\nsleep 2500\nsend $B afteroff\nsleep 500\nquit\n" "$OUT/a" &
sleep 0.2
host $B "sleep 1800\nsend bcast b_hi\nsleep 1700\nsend $A toA\nsleep 3000\nquit\n" "$OUT/b" &
sleep 0.2
host $C "sleep 2500\nsend bcast bc20\nsleep 4000\nquit\n" "$OUT/c" &
sleep 0.2
host $D "sleep 6500\nquit\n" "$OUT/d" &
sleep 0.2
host $T "sleep 3200\nvlan 20\nsend bcast fromtrunk20\nvlan 10\nsend $A fromtrunk10\nsleep 3000\nquit\n" "$OUT/t" &
sleep 0.2
host $S "sleep 6500\nquit\n" "$OUT/s" &
sleep 0.3

$CTL vlan 1 10 > /dev/null
$CTL vlan 2 10 > /dev/null
$CTL vlan 3 20 > /dev/null
$CTL vlan 4 20 > /dev/null
$CTL vlan 6 10 > /dev/null
$CTL trunk 5 > /dev/null
$CTL mirror 1 6 > "$OUT/mirror_reply"
$CTL show ports > "$OUT/ports"

sleep 2.9
$CTL stats > "$OUT/stats"
$CTL mirror 1 off > /dev/null
$CTL show ports > "$OUT/ports_after"
sleep 3

has "$OUT/ports" "mirrored to port 6" || grep -q "port 1 is mirrored to port 6" "$OUT/mirror_reply"
result "swctl mirror says what it did" $?
grep -qE "^1 +up +10 +->6 " "$OUT/ports"
result "show ports: port 1 in vlan 10, mirrored to 6" $?
grep -qE "^5 +up +trunk " "$OUT/ports"
result "show ports: port 5 is the trunk" $?
grep -qE "^1 +up +10 +- " "$OUT/ports_after"
result "mirroring can be switched off" $?

# VLAN 10: A, B, S.  VLAN 20: C, D.  Trunk: T
has "$OUT/b" "len 4: bc10"
result "broadcast reaches the same VLAN" $?
! has "$OUT/c" "bc10"
result "broadcast does not leave its VLAN (C)" $?
! has "$OUT/d" "bc10"
result "broadcast does not leave its VLAN (D)" $?
! has "$OUT/c" "cross"
result "unicast to a host in another VLAN is not delivered" $?
! has "$OUT/d" "cross"
result "...and is not flooded into the other VLAN" $?
has "$OUT/d" "bc20"
result "VLAN 20 broadcast reaches D" $?
! has "$OUT/a" "bc20"
result "VLAN 20 broadcast does not reach VLAN 10" $?

# the trunk carries every VLAN and keeps the tag
has "$OUT/t" "vlan 10 pri 1 len 4: bc10"
result "trunk gets VLAN 10 with its vlan field" $?
has "$OUT/t" "vlan 20 pri 1 len 4: bc20"
result "trunk gets VLAN 20 with its vlan field" $?
has "$OUT/c" "fromtrunk20"
result "frame from the trunk reaches its VLAN" $?
has "$OUT/d" "fromtrunk20"
result "frame from the trunk reaches all of its VLAN" $?
! has "$OUT/a" "fromtrunk20"
result "frame from the trunk stays out of other VLANs" $?
has "$OUT/a" "fromtrunk10"
result "unicast from the trunk reaches its host" $?

# mirroring port 1 to port 6
[ "$(grep -c "bc10" "$OUT/s")" -eq 1 ]
result "sniffer gets one copy, not two, when it is a recipient anyway" $?
has "$OUT/s" "same"
result "sniffer sees unicast sent by the mirrored port" $?
has "$OUT/s" "toA"
result "sniffer sees unicast sent to the mirrored port" $?
! has "$OUT/s" "afteroff"
result "no copies after mirroring is switched off" $?
grep -qE "^1 +up +[0-9]+ +[0-9]+ +[0-9]+ +[1-9][0-9]*$" "$OUT/stats"
result "mirrored counter is counted in shared memory" $?

kill -0 "$SW" 2>/dev/null
result "switch survived" $?
kill -INT "$SW"
wait "$SW"
result "switch exits cleanly" $?
wait

rm -rf "$OUT"
echo "$pass passed, $fail failed (switch output in $LOG)"
[ "$fail" -eq 0 ]
