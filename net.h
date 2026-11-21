#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "frame.h"

ssize_t read_full(int fd, void *buf, size_t n);
int write_full(int fd, const void *buf, size_t n);

/* Same, but also wake on a second descriptor: if wake becomes readable the
 * call stops with errno = ECANCELED. Used for the pipes between two
 * switches, where shutdown() does not exist. wake = -1 means no wake fd;
 * for writing, fd must be non-blocking when wake is used. */
ssize_t read_full_w(int fd, int wake, void *buf, size_t n);
int write_full_w(int fd, int wake, const void *buf, size_t n);

int frame_read(int fd, struct frame_hdr *h, uint8_t *payload);
int frame_read_w(int fd, int wake, struct frame_hdr *h, uint8_t *payload);
int frame_write(int fd, const struct frame_hdr *h, const uint8_t *payload);

#endif
