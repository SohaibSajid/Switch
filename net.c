#define _POSIX_C_SOURCE 200809L

#include "net.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

/* wait until fd is ready for events; -1 with ECANCELED if wake fired first */
static int wait_ready(int fd, short events, int wake)
{
    struct pollfd p[2];

    p[0].fd = fd;
    p[0].events = events;
    p[1].fd = wake;
    p[1].events = POLLIN;
    for (;;) {
        if (poll(p, 2, -1) == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (p[1].revents) {
            errno = ECANCELED;
            return -1;
        }
        return 0;
    }
}

ssize_t read_full_w(int fd, int wake, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        if (wake >= 0 && wait_ready(fd, POLLIN, wake) == -1)
            return -1;
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r == 0)
            break;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got += (size_t)r;
    }
    return (ssize_t)got;
}

int write_full_w(int fd, int wake, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, (const char *)buf + done, n - done);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (wake >= 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (wait_ready(fd, POLLOUT, wake) == -1)
                    return -1;
                continue;
            }
            return -1;
        }
        done += (size_t)w;
    }
    return 0;
}

ssize_t read_full(int fd, void *buf, size_t n)
{
    return read_full_w(fd, -1, buf, n);
}

int write_full(int fd, const void *buf, size_t n)
{
    return write_full_w(fd, -1, buf, n);
}

int frame_read_w(int fd, int wake, struct frame_hdr *h, uint8_t *payload)
{
    uint8_t buf[FRAME_HDR];

    ssize_t r = read_full_w(fd, wake, buf, FRAME_HDR);
    if (r == 0)
        return 0;
    if (r < 0)
        return -1;
    if (r < FRAME_HDR || frame_unpack(buf, h) == -1) {
        errno = EPROTO;
        return -1;
    }

    r = read_full_w(fd, wake, payload, h->len);
    if (r < 0)
        return -1;
    if (r < h->len) {
        errno = EPROTO;
        return -1;
    }
    return 1;
}

int frame_read(int fd, struct frame_hdr *h, uint8_t *payload)
{
    return frame_read_w(fd, -1, h, payload);
}

int frame_write(int fd, const struct frame_hdr *h, const uint8_t *payload)
{
    uint8_t buf[FRAME_HDR + MAX_PAYLOAD];

    if (frame_pack(h, buf) == -1) {
        errno = EINVAL;
        return -1;
    }
    memcpy(buf + FRAME_HDR, payload, h->len);
    return write_full(fd, buf, FRAME_HDR + (size_t)h->len);
}
