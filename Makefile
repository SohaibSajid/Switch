CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0 -pthread
LDFLAGS = -pthread
LDLIBS = -lrt

HEADERS = $(wildcard *.h)
COMMON = frame.o mac_table.o net.o pkt.o queue.o config.o shm_stats.o log.o
PROGS = switch host swctl switch_pair supervisor bench test_main

all: $(PROGS)

switch: switch.o $(COMMON)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

swctl: swctl.o net.o frame.o shm_stats.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

host: host.o frame.o net.o
	$(CC) $(LDFLAGS) -o $@ $^

bench: bench.o
	$(CC) $(LDFLAGS) -o $@ $^

switch_pair: switch_pair.o
	$(CC) $(LDFLAGS) -o $@ $^

supervisor: supervisor.o mac_table.o frame.o log.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test_main: test_main.o frame.o mac_table.o pkt.o queue.o log.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# separate builds for the sanitizer, do not run them under valgrind
switch_tsan: switch.c frame.c mac_table.c net.c pkt.c queue.c config.c shm_stats.c log.c $(HEADERS)
	$(CC) $(CFLAGS) -fsanitize=thread -o $@ $(filter %.c,$^) $(LDLIBS)

test_tsan: test_main.c frame.c mac_table.c pkt.c queue.c log.c $(HEADERS)
	$(CC) $(CFLAGS) -fsanitize=thread -o $@ $(filter %.c,$^) $(LDLIBS)

frame.o: frame.c frame.h
mac_table.o: mac_table.c mac_table.h frame.h log.h sync.h
net.o: net.c net.h frame.h
pkt.o: pkt.c pkt.h frame.h sync.h
queue.o: queue.c queue.h pkt.h sync.h
log.o: log.c log.h sync.h
config.o: config.c config.h
shm_stats.o: shm_stats.c shm_stats.h sync.h
switch.o: switch.c shm_stats.h config.h frame.h log.h mac_table.h net.h pkt.h queue.h sync.h
swctl.o: swctl.c net.h frame.h shm_stats.h
host.o: host.c frame.h net.h
switch_pair.o: switch_pair.c
bench.o: bench.c
supervisor.o: supervisor.c mac_table.h frame.h
test_main.o: test_main.c frame.h mac_table.h pkt.h queue.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: all
	./run_tests.sh

clean:
	rm -f *.o $(PROGS) switch_tsan test_tsan

.PHONY: all clean test
