#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "frame.h"

ssize_t read_full(int fd, void *buf, size_t n);
int write_full(int fd, const void *buf, size_t n);

int frame_read(int fd, struct frame_hdr *h, uint8_t *payload);
int frame_write(int fd, const struct frame_hdr *h, const uint8_t *payload);

#endif
