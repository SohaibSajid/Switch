#!/bin/bash

PORT=${1:-9800}
SWITCH=${SWITCH:-./switch}
LOG=switch_sig.log
OUT=$(mktemp -d)
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

host() {
    printf "$2" | ./host 127.0.0.1 "$PORT" "$1" > "$3"
}

wait_log() {
    for _ in $(seq 50); do
        grep -qF -- "$1" "$LOG" && return 0
        sleep 0.1
    done
    return 1
}

printf '# test config\nage = 60\nmax_macs = 100\nmacs_per_port = 8\nwait = 1\n' > "$CFG"

$SWITCH -c "$CFG" -d "$DUMP" "$PORT" 4 > "$LOG" 2>&1 &
SW=$!
sleep "${WARMUP:-0.3}"

grep -q "age 60s, max 100 MACs, 8 per port, wait 1s" "$LOG"
result "config file read at startup" $?

host 02:00:00:00:00:0A "send bcast hello\nsleep 8000\nquit\n" "$OUT/a" &
HA=$!
sleep 0.3

kill -USR1 "$SW"
wait_log "SIGUSR1: 1 MAC entries written"
result "SIGUSR1 writes the dump" $?
grep -q "02:00:00:00:00:0A     1" "$DUMP" 2>/dev/null
result "dump lists MAC and port" $?

printf 'age = 60\nmax_macs = 100\nmacs_per_port = 1\nwait = 1\n' > "$CFG"
kill -HUP "$SW"
wait_log "SIGHUP: config reloaded"
result "SIGHUP reloads the config" $?

host 02:00:00:00:00:0B "send bcast one\nsrc 02:00:00:00:00:0C\nsend bcast two\nsleep 300\nquit\n" /dev/null
grep -q "port security, more than 1 MACs" "$LOG"
result "reloaded limit takes effect" $?

printf 'age = soon\n' > "$CFG"
kill -HUP "$SW"
wait_log "keeping old settings"
result "bad config is rejected, old kept" $?

exec 3<>/dev/tcp/127.0.0.1/"$PORT"
sleep 0.3
kill -INT "$SW"
for _ in $(seq 100); do
    kill -0 "$SW" 2>/dev/null || break
    sleep 0.1
done
! kill -0 "$SW" 2>/dev/null
result "SIGINT stops the switch within 10s" $?
wait "$SW"
code=$?
[ "$code" -eq 0 ]
result "switch exits with status 0" $?
exec 3>&-

grep -q "switch stopped cleanly" "$LOG"
result "shutdown completes" $?
wait $HA
grep -q "switch closed the connection" "$OUT/a"
result "connected host is disconnected" $?

rm -rf "$OUT"
echo "$pass passed, $fail failed (switch output in $LOG)"
[ "$fail" -eq 0 ]
