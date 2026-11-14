CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0 -pthread
LDFLAGS = -pthread

SRCS = frame.c mac_table.c
OBJS = $(SRCS:.c=.o)

all: test_main

test_main: test_main.o $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

test_tsan: test_main.c $(SRCS)
	$(CC) $(CFLAGS) -fsanitize=thread -o $@ $^

frame.o: frame.c frame.h
mac_table.o: mac_table.c mac_table.h frame.h
test_main.o: test_main.c frame.h mac_table.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o test_main test_tsan

.PHONY: all clean
