#define _POSIX_C_SOURCE 200809L

#include "queue.h"

#include <errno.h>
#include <stdlib.h>

#include "sync.h"

static int drain(struct queue *q)
{
    int n = 0;
    while (q->count > 0) {
        pkt_unref(q->item[q->head]);
        q->head = (q->head + 1) % q->cap;
        q->count--;
        n++;
    }
    q->head = 0;
    q->tail = 0;
    return n;
}

int queue_init(struct queue *q, int cap)
{
    q->item = calloc((size_t)cap, sizeof *q->item);
    if (!q->item)
        return ENOMEM;

    int err = pthread_mutex_init(&q->lock, NULL);
    if (err) {
        free(q->item);
        return err;
    }
    err = pthread_cond_init(&q->not_empty, NULL);
    if (err) {
        pthread_mutex_destroy(&q->lock);
        free(q->item);
        return err;
    }

    q->cap = cap;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->closing = 1;
    q->dropped = 0;
    return 0;
}

void queue_destroy(struct queue *q)
{
    drain(q);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
    free(q->item);
    q->item = NULL;
}

void queue_open(struct queue *q)
{
    lock(&q->lock);
    drain(q);
    q->dropped = 0;
    q->closing = 0;
    unlock(&q->lock);
}

unsigned long queue_close(struct queue *q)
{
    lock(&q->lock);
    q->closing = 1;
    q->dropped += (unsigned long)drain(q);
    unsigned long dropped = q->dropped;
    cond_broadcast(&q->not_empty);
    unlock(&q->lock);
    return dropped;
}

int queue_push(struct queue *q, struct pkt *k)
{
    lock(&q->lock);
    if (q->closing) {
        unlock(&q->lock);
        return QUEUE_CLOSED;
    }
    if (q->count == q->cap) {
        q->dropped++;
        unlock(&q->lock);
        return QUEUE_FULL;
    }
    q->item[q->tail] = k;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;
    cond_signal(&q->not_empty);
    unlock(&q->lock);
    return QUEUE_OK;
}

struct pkt *queue_pop(struct queue *q)
{
    lock(&q->lock);
    while (q->count == 0 && !q->closing)
        cond_wait(&q->not_empty, &q->lock);
    if (q->closing) {
        unlock(&q->lock);
        return NULL;
    }
    struct pkt *k = q->item[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
    unlock(&q->lock);
    return k;
}
