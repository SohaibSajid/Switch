#define _POSIX_C_SOURCE 200809L

#include "net.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

ssize_t read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
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

int write_full(int fd, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, (const char *)buf + done, n - done);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)w;
    }
    return 0;
}

int frame_read(int fd, struct frame_hdr *h, uint8_t *payload)
{
    uint8_t buf[FRAME_HDR];

    ssize_t r = read_full(fd, buf, FRAME_HDR);
    if (r == 0)
        return 0;
    if (r < 0)
        return -1;
    if (r < FRAME_HDR || frame_unpack(buf, h) == -1) {
        errno = EPROTO;
        return -1;
    }

    r = read_full(fd, payload, h->len);
    if (r < 0)
        return -1;
    if (r < h->len) {
        errno = EPROTO;
        return -1;
    }
    return 1;
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
