#ifndef PKT_H
#define PKT_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "frame.h"

struct pkt {
    pthread_mutex_t lock;
    int refs;
    struct frame_hdr hdr;
    size_t size;
    uint8_t data[];
};

struct pkt *pkt_new(const struct frame_hdr *h, const uint8_t *payload);
void pkt_ref(struct pkt *k);
void pkt_unref(struct pkt *k);

#endif
