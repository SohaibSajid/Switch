# Virtual Switch

A software network switch in C for the Operating Systems course (Fall 2026).
One multithreaded server (`switch`), a test host (`host`), an admin tool
(`swctl`), and a few extra programs for the bonus part.

## Build

```
make            # builds everything with gcc -std=c11 -Wall -Wextra -Werror -g -O0
make clean      # removes the programs and object files
make test       # builds and runs all tests (takes about 3 minutes)
```

Programs: `switch`, `host`, `swctl`, `switch_pair`, `supervisor`, `bench`,
`test_main` (unit tests).

## Running

Start a switch with 8 ports and a few hosts:

```
./switch 9000 8
./host 127.0.0.1 9000 02:00:00:00:00:0A
./host 127.0.0.1 9000 02:00:00:00:00:0B
```

`host` reads commands from stdin (type `help`): `send <mac|bcast> <text>`,
`slow`, `burst <mac|bcast> <n> [size]`, `half`, `vlan <n>`, `pri <0-2>`,
`src <mac>`, `sleep <ms>`, `quit`. Frames it receives are printed.

### switch options

```
./switch [options] <tcp_port> <num_ports 1-64>
  -c file   config file, reloaded on SIGHUP
  -l file   log file (default switch.log)
  -v        also print the log on stdout
  -f        log every forwarded frame (off by default, it is a lot)
  -d file   where SIGUSR1 dumps the MAC table (default mac_table.txt)
  -s path   admin UNIX socket (default /tmp/vswitch.sock)
  -S name   shared memory object for the counters (default /vswitch_stats)
  -M name   keep the MAC table in this shared memory object (used by supervisor)
  -t R,W    pipe link to another switch (used by switch_pair)
  -a secs   MAC aging time (30)        -k n   MAC table size (1024)
  -m n      MACs per port (16)         -w secs  wait for a free port (5)
  -r n      rate limit in frames/s per port (0 = off)
```

`-k 0` and `-m 0` mean no limit (the table holds at most 32768 entries).
Command line options win over the config file.

### swctl

```
./swctl [-s admin_socket] [-S shm_name] <command>
  show mac                    MAC table
  show ports                  state, VLAN, mirror, rate limit, queue, counters
  shutdown <port>             disconnect the host, close the port
  up <port>                   open a shut down port again
  vlan <port> <vlan|trunk>    VLAN 1-4094, or make the port the trunk
  trunk <port|off>            choose the trunk port, or none
  mirror <port> <sniffer|off> copy all traffic of a port to the sniffer port
  rate <port> <frames/s>      rate limit, 0 = unlimited
  clear mac
  stats                       counters from shared memory
  stats --watch               the same, every second
```

`swctl` exits with 0 when the command worked and 1 when it did not.
`stats` reads the counters (in, out, dropped, mirrored per port) directly from
shared memory; everything else goes over the UNIX socket.

### Config file

One `key = value` per line, `#` starts a comment.

```
age = 30            # seconds before an idle MAC is removed
max_macs = 1024     # MAC table size, 0 = no limit
macs_per_port = 16  # port security: more MACs than this shuts the port down
wait = 5            # seconds a new host waits for a free port
rate = 0            # frames per second per port, 0 = off
weights = 5:3:1     # weighted round robin: high:normal:low
qos_age = 200       # ms after which a waiting normal/low frame goes first, 0 = off
trunk = 8           # port number, or "off"
vlan.3 = 20         # port 3 is in VLAN 20
rate.2 = 100        # port 2 has its own rate limit
mirror.3 = 7        # traffic of port 3 is copied to port 7 ("off" to stop)
```

`kill -HUP <switch pid>` reads the file again. A file with an error is
rejected as a whole and the old settings stay. Only the values that changed
since the last read are applied, so a reload does not undo what you did with
`swctl` to things the file did not touch.

### Signals

| Signal | What happens |
|---|---|
| `SIGINT`, `SIGTERM` | clean shutdown: all threads joined, sockets closed, memory freed |
| `SIGHUP` | reload the config file |
| `SIGUSR1` | write the MAC table to the dump file |

One thread calls `sigwait`; every other thread has the signals blocked.
`SIGPIPE` is ignored.

## How it behaves

- **Frames**: 17 byte header + payload as in the project text. Partial reads
  and writes are handled; a host that disconnects in the middle of a frame,
  or sends a bad length or priority, loses its port and nobody else notices.
- **Learning and forwarding**: the source MAC is learned; known unicast goes
  to one port, unknown unicast and broadcast go to every other port of the
  VLAN. A MAC that shows up on a new port is moved ("MAC moved" in the log).
- **VLANs**: a host on a normal port does not tag its frames. The port's VLAN
  (1 by default) is used, whatever the `vlan` field in the frame says. The
  trunk port carries all VLANs and keeps the `vlan` field, so on the trunk the
  field of the frame decides (it must be 1-4094).
- **Priority queues**: three queues per port (64 frames each), weighted round
  robin 5:3:1, plus aging so that low priority frames are never starved.
- **Rate limit**: token bucket per port, refilled by a timer thread every
  10 ms. A sender without tokens waits on a condition variable with a timeout.
- **Mirroring**: copies go into the sniffer's queue. If that is full the copy
  is dropped; the mirrored port is never slowed down. A sniffer that would
  get the frame anyway gets it once.
- **Port limit**: a semaphore holds the free ports. A host that finds no
  free port waits up to `wait` seconds in its own thread, then gets a frame
  with the text `switch full` and is disconnected.
- **Log**: every thread puts messages on a queue, one logger thread writes
  them with timestamps to the log file.

## Bonus programs

**Two switches** (`switch_pair`):

```
./switch_pair 9000 9001 4        # two switches with 4 ports each
./host 127.0.0.1 9000 02:00:00:00:00:0A     # on switch A
./host 127.0.0.1 9001 02:00:00:00:00:0C     # on switch B
./swctl -s /tmp/vswitch_9001.sock -S /vswitch_stats_9001 show ports
```

It creates two pipes and starts `./switch -t R,W` twice (set `SWITCH_BIN` for
another binary). Each switch gets one extra port, the last one, which is
always the trunk. Each switch learns the hosts of the other one behind its
trunk port. Admin socket, shared memory and log of each switch are named
after its TCP port: `/tmp/vswitch_<port>.sock`, `/vswitch_stats_<port>`,
`switch_<port>.log`.

**Supervisor**:

```
./supervisor [-M mac_shm_name] [-n max_restarts] <switch arguments>
./supervisor -n 5 -c switch.conf 9000 8
```

It creates the MAC table in shared memory (`/vswitch_mac` by default), starts
the switch with `-M`, and starts it again when it dies from a signal. The new
switch checks the table, throws away anything damaged, and goes on with what
the old one had learned. `SIGINT`/`SIGTERM` to the supervisor stop the switch
and the supervisor; `SIGHUP`/`SIGUSR1` are passed on to the switch. A switch
that exits with an error status (bad arguments, port in use) is not restarted.

**Benchmark** (`bench`, `bench_plot.py`): compares one global lock, striped
locks and a read-write lock on the same hash table.

```
./bench -t 16 -s 2 -w 10 > bench_10pct.csv
./bench -t 16 -s 2 -w 1  > bench_1pct.csv
python3 bench_plot.py        # writes bench_results.svg
```

## Tests

`make test` runs `./run_tests.sh`. The scripts start their own switch on
their own port, so they can run on a machine where nothing else uses ports
9100-9900.

| Script | What it checks |
|---|---|
| `test_main` | frame packing, MAC table (threads, limits, trunk, shared memory repair), queue (WRR order, aging, token bucket, close) |
| `test_proto.sh` | partial reads, bad frames, half frames |
| `test_forward.sh` | forwarding, flooding, a host that never reads |
| `test_tier1.sh` | port semaphore and `switch full`, port security, aging |
| `test_signals.sh` | config file, SIGHUP, SIGUSR1, SIGINT |
| `test_control.sh` | swctl, shared memory counters, `stats --watch` |
| `test_vlan.sh` | VLANs, trunk, mirroring |
| `test_admin.sh` | shutdown/up, port accounting, many hosts waiting |
| `test_qos.sh` | rate limit and priority order |
| `test_link.sh` | two switches, one crashing |
| `test_super.sh` | supervisor, MAC table survives `kill -9` |
| `test_stress.sh` | traffic + admin commands + signals all at once |

### ThreadSanitizer and valgrind

Use separate builds, one tool per run.

```
make switch_tsan test_tsan
SWITCH="setarch $(uname -m) -R ./switch_tsan" SWITCH_BIN=... ./run_tests.sh
```

On WSL2 and some Ubuntu kernels ThreadSanitizer stops with "unexpected memory
mapping"; `setarch -R` (no address randomisation) fixes that. `SWITCH_BIN`
must be a single executable, so for the pair and supervisor tests put the
`setarch` or `valgrind` command in a small wrapper script that ends in
`exec ... ./switch "$@"`.

```
valgrind --leak-check=full --error-exitcode=9 ./switch 9000 8
WARMUP=3 SWITCH="valgrind -q --leak-check=full ./switch" ./test_vlan.sh
```

The test scripts wait `WARMUP` seconds (default 0.3) for the switch to start;
raise it for the slow tools.

## Files

| File | Content |
|---|---|
| `switch.c` | the switch: ports, receiver/sender threads, forwarding, admin socket, signals, reconfiguration |
| `queue.c` | per-port output queue: 3 classes, WRR + aging, token bucket |
| `mac_table.c` | MAC table: striped locks, aging, LRU, port security, shared memory version |
| `pkt.c` | reference counted frames |
| `log.c` | log queue and logger thread |
| `config.c` | config file |
| `shm_stats.c` | counters in shared memory (process-shared mutex) |
| `net.c`, `frame.c` | read/write helpers, frame header packing |
| `host.c`, `swctl.c` | test host, admin tool |
| `switch_pair.c`, `supervisor.c`, `bench.c` | bonus |
| `REPORT.html`, `REPORT.pdf` | the report (3 pages) |
| `bench_*.csv`, `bench_results.svg`, `bench_plot.py` | benchmark data and graph |
| `run_tests.sh`, `test_*.sh` | tests |

## Limits

64 host ports (plus the link port), 64 frames per queue class, 32768 MAC
entries, 128 hosts waiting at the same time is not a problem but at most 64
waiters are kept; the 65th is rejected at once.
