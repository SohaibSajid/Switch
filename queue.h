#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>
#include <stdint.h>

#include "pkt.h"

#define QUEUE_CLASSES 3

enum { QUEUE_OK = 0, QUEUE_FULL = -1, QUEUE_CLOSED = -2 };

struct qitem {
    struct pkt *pkt;
    int64_t t;
};

/* One output queue per port: a ring for each priority class, plus the
 * scheduler state (weighted round robin, aging) and the token bucket.
 * Everything is protected by lock. */
struct queue {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    struct qitem *ring[QUEUE_CLASSES];
    int head[QUEUE_CLASSES];
    int count[QUEUE_CLASSES];
    int cap;
    int total;
    int closing;
    unsigned long dropped;

    int weight[QUEUE_CLASSES];
    int age_ms;
    int cur;
    int credit;
    int last_aged;

    int rate;
    long tokens;
    long burst;
};

int queue_init(struct queue *q, int cap);
void queue_destroy(struct queue *q);
void queue_open(struct queue *q);
unsigned long queue_close(struct queue *q);
int queue_flush(struct queue *q);
int queue_push(struct queue *q, struct pkt *k);
struct pkt *queue_pop(struct queue *q);
int queue_len(struct queue *q);

void queue_set_qos(struct queue *q, const int *weight, int age_ms);
void queue_set_rate(struct queue *q, int rate);
void queue_refill(struct queue *q, long ms);
int queue_rate(struct queue *q);

#endif
