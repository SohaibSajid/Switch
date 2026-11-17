#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>

#include "pkt.h"

enum { QUEUE_OK = 0, QUEUE_FULL = -1, QUEUE_CLOSED = -2 };

struct queue {
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    struct pkt **item;
    int cap;
    int head;
    int tail;
    int count;
    int closing;
    unsigned long dropped;
};

int queue_init(struct queue *q, int cap);
void queue_destroy(struct queue *q);
void queue_open(struct queue *q);
unsigned long queue_close(struct queue *q);
int queue_push(struct queue *q, struct pkt *k);
struct pkt *queue_pop(struct queue *q);

#endif
