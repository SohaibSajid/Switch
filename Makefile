CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0 -pthread
LDFLAGS = -pthread

COMMON = frame.o mac_table.o net.o
PROGS = switch host test_main

all: $(PROGS)

switch: switch.o $(COMMON)
	$(CC) $(LDFLAGS) -o $@ $^

host: host.o frame.o net.o
	$(CC) $(LDFLAGS) -o $@ $^

test_main: test_main.o frame.o mac_table.o
	$(CC) $(LDFLAGS) -o $@ $^

switch_tsan: switch.c frame.c mac_table.c net.c
	$(CC) $(CFLAGS) -fsanitize=thread -o $@ $^

test_tsan: test_main.c frame.c mac_table.c
	$(CC) $(CFLAGS) -fsanitize=thread -o $@ $^

frame.o: frame.c frame.h
mac_table.o: mac_table.c mac_table.h frame.h
net.o: net.c net.h frame.h
switch.o: switch.c frame.h mac_table.h net.h
host.o: host.c frame.h net.h
test_main.o: test_main.c frame.h mac_table.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o $(PROGS) switch_tsan test_tsan

.PHONY: all clean
