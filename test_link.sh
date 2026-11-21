#!/bin/bash
# bonus: two switches joined by two pipes (switch_pair)

PA=${1:-9810}
PB=$((PA + 1))
SWITCH_BIN=${SWITCH_BIN:-./switch}
export SWITCH_BIN
OUT=$(mktemp -d)
CTLA="./swctl -s /tmp/vswitch_$PA.sock -S /vswitch_stats_$PA"
CTLB="./swctl -s /tmp/vswitch_$PB.sock -S /vswitch_stats_$PB"
A1=02:00:00:00:00:0A
A2=02:00:00:00:00:0B
B1=02:00:00:00:00:0C
B2=02:00:00:00:00:0D
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
    local port=$1 mac=$2 script=$3 out=$4
    printf "$script" | ./host 127.0.0.1 "$port" "$mac" > "$out"
}

has() { grep -qF -- "$2" "$1"; }

rm -f switch_$PA.log switch_$PB.log
./switch_pair $PA $PB 4 -f -w 1 > "$OUT/pair" 2>&1 &
PAIR=$!
sleep "${WARMUP:-0.6}"

host $PA $A1 "sleep 1500\nsend bcast hello from A1\nsleep 1500\nsend $B1 unicast to B1\nsleep 1400\nsend bcast vlan10 msg\nsleep 1600\nsend bcast after B died\nsleep 700\nquit\n" "$OUT/a1" &
sleep 0.1
host $PA $A2 "sleep 7500\nquit\n" "$OUT/a2" &
sleep 0.1
host $PB $B1 "sleep 2200\nsend $A1 reply to A1\nsleep 4500\nquit\n" "$OUT/b1" &
sleep 0.1
host $PB $B2 "sleep 6500\nquit\n" "$OUT/b2" &
sleep 0.1

# t = 3.5: put A1 and B1 into VLAN 10 on their own switches
sleep 2.9
$CTLA show ports > "$OUT/portsA"
$CTLA show mac > "$OUT/macA"
$CTLB show mac > "$OUT/macB"
$CTLA vlan 1 10 > /dev/null
$CTLB vlan 1 10 > /dev/null

sleep 2.2
# t = 5.7: A1 goes back to VLAN 1 and the switch B crashes
$CTLA vlan 1 1 > /dev/null
BPID=$(pgrep -P $PAIR | sed -n 2p)
kill -9 "$BPID"
sleep 2.0
$CTLA show ports > "$OUT/portsA2"

grep -qE "^5 +up +trunk " "$OUT/portsA"
result "switch A has the link as port 5, always the trunk" $?
grep -q "^$A1 .* 1 " "$OUT/macA" && grep -q "^$B1 .* 5 " "$OUT/macA"
result "A learned that B's host is behind the trunk" $?
grep -q "^$B1 .* 1 " "$OUT/macB" && grep -q "^$A1 .* 5 " "$OUT/macB"
result "B learned that A's host is behind the trunk" $?

has "$OUT/b1" "len 13: hello from A1"
result "broadcast from A reaches the host on B (B1)" $?
has "$OUT/b2" "len 13: hello from A1"
result "...and the other host on B (B2)" $?
has "$OUT/a1" "len 11: reply to A1"
result "unicast reply comes back across the link" $?
has "$OUT/b1" "unicast to B1"
result "unicast from A reaches B1" $?
! has "$OUT/b2" "unicast to B1"
result "...but not B2: B learned where B1 is" $?
! has "$OUT/a2" "unicast to B1"
result "...and not A2: A learned that B1 is behind the trunk" $?
! has "$OUT/a2" "reply to A1"
result "the reply does not reach A2 either" $?

has "$OUT/b1" "vlan 10 pri 1 len 10: vlan10 msg"
result "VLAN 10 frame crosses the link and keeps its tag" $?
! has "$OUT/b2" "vlan10 msg"
result "VLAN 10 frame does not reach VLAN 1 on the other switch" $?
! has "$OUT/a2" "vlan10 msg"
result "VLAN 10 frame does not reach VLAN 1 on its own switch" $?

has "$OUT/a2" "after B died"
result "switch A keeps working when B crashes" $?
grep -qE "^5 +down " "$OUT/portsA2"
result "A shows the link as down" $?
grep -q "the other switch closed the link" switch_$PA.log
result "A logged that the link was closed" $?

kill -INT $PAIR
wait $PAIR
wait
grep -q "switch stopped cleanly" switch_$PA.log
result "switch A shut down cleanly" $?
[ ! -e "/dev/shm/vswitch_stats_$PA" ]
result "switch A removed its shared memory" $?

rm -f "/dev/shm/vswitch_stats_$PB" "/tmp/vswitch_$PB.sock"
rm -rf "$OUT"
echo "$pass passed, $fail failed (logs in switch_$PA.log and switch_$PB.log)"
[ "$fail" -eq 0 ]
