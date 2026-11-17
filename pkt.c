#define _POSIX_C_SOURCE 200809L

#include "pkt.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "sync.h"

struct pkt *pkt_new(const struct frame_hdr *h, const uint8_t *payload)
{
    struct pkt *k = malloc(sizeof *k + FRAME_HDR + h->len);
    if (!k)
        return NULL;

    if (frame_pack(h, k->data) == -1) {
        free(k);
        errno = EINVAL;
        return NULL;
    }

    int err = pthread_mutex_init(&k->lock, NULL);
    if (err) {
        free(k);
        errno = err;
        return NULL;
    }

    memcpy(k->data + FRAME_HDR, payload, h->len);
    k->hdr = *h;
    k->size = FRAME_HDR + (size_t)h->len;
    k->refs = 1;
    return k;
}

void pkt_ref(struct pkt *k)
{
    lock(&k->lock);
    k->refs++;
    unlock(&k->lock);
}

void pkt_unref(struct pkt *k)
{
    lock(&k->lock);
    int left = --k->refs;
    unlock(&k->lock);

    if (left == 0) {
        pthread_mutex_destroy(&k->lock);
        free(k);
    }
}
