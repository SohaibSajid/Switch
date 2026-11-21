#define _POSIX_C_SOURCE 200809L

#include "queue.h"

#include <errno.h>
#include <stdlib.h>
#include <time.h>

#include "sync.h"

/* tokens are counted in thousandths, so a refill of a few milliseconds
 * still adds something */
#define TOKEN 1000L
#define TOKEN_WAIT_MS 100

static int64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
        die_err("clock_gettime", errno);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct timespec in_ms(int ms)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
        die_err("clock_gettime", errno);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

static struct qitem *front(struct queue *q, int c)
{
    return &q->ring[c][q->head[c]];
}

static struct pkt *take(struct queue *q, int c)
{
    struct pkt *k = front(q, c)->pkt;
    q->head[c] = (q->head[c] + 1) % q->cap;
    q->count[c]--;
    q->total--;
    return k;
}

static int drain(struct queue *q)
{
    int n = 0;
    for (int c = 0; c < QUEUE_CLASSES; c++) {
        while (q->count[c] > 0) {
            pkt_unref(take(q, c));
            n++;
        }
        q->head[c] = 0;
    }
    return n;
}

/* Choose the class of the next frame. Needs q->total > 0. */
static int pick(struct queue *q)
{
    /* aging: a normal or low frame that waited too long goes first, but
     * not twice in a row, so high priority traffic still gets through */
    if (q->age_ms > 0 && !q->last_aged) {
        int64_t now = now_ms();
        int best = -1;
        int64_t longest = 0;
        for (int c = 1; c < QUEUE_CLASSES; c++) {
            if (q->count[c] == 0)
                continue;
            int64_t waited = now - front(q, c)->t;
            if (waited >= q->age_ms && waited > longest) {
                longest = waited;
                best = c;
            }
        }
        if (best >= 0) {
            q->last_aged = 1;
            return best;
        }
    }
    q->last_aged = 0;

    /* weighted round robin: each class may send weight[c] frames in a row,
     * an empty class is skipped */
    for (;;) {
        if (q->credit > 0 && q->count[q->cur] > 0) {
            q->credit--;
            return q->cur;
        }
        q->cur = (q->cur + 1) % QUEUE_CLASSES;
        q->credit = q->weight[q->cur];
    }
}

int queue_init(struct queue *q, int cap)
{
    int err;

    for (int c = 0; c < QUEUE_CLASSES; c++) {
        q->ring[c] = calloc((size_t)cap, sizeof *q->ring[c]);
        if (!q->ring[c]) {
            while (--c >= 0)
                free(q->ring[c]);
            return ENOMEM;
        }
    }

    pthread_condattr_t attr;
    if ((err = pthread_mutex_init(&q->lock, NULL)) ||
        (err = pthread_condattr_init(&attr)) ||
        (err = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) ||
        (err = pthread_cond_init(&q->wake, &attr)) ||
        (err = pthread_condattr_destroy(&attr))) {
        for (int c = 0; c < QUEUE_CLASSES; c++)
            free(q->ring[c]);
        return err;
    }

    q->cap = cap;
    q->total = 0;
    q->closing = 1;
    q->dropped = 0;
    for (int c = 0; c < QUEUE_CLASSES; c++) {
        q->head[c] = 0;
        q->count[c] = 0;
    }

    q->weight[0] = 5;
    q->weight[1] = 3;
    q->weight[2] = 1;
    q->age_ms = 200;
    q->cur = 0;
    q->credit = q->weight[0];
    q->last_aged = 0;

    q->rate = 0;
    q->tokens = 0;
    q->burst = 0;
    return 0;
}

void queue_destroy(struct queue *q)
{
    drain(q);
    pthread_cond_destroy(&q->wake);
    pthread_mutex_destroy(&q->lock);
    for (int c = 0; c < QUEUE_CLASSES; c++) {
        free(q->ring[c]);
        q->ring[c] = NULL;
    }
}

void queue_open(struct queue *q)
{
    lock(&q->lock);
    drain(q);
    q->dropped = 0;
    q->closing = 0;
    q->tokens = q->burst;
    q->last_aged = 0;
    unlock(&q->lock);
}

unsigned long queue_close(struct queue *q)
{
    lock(&q->lock);
    q->closing = 1;
    q->dropped += (unsigned long)drain(q);
    unsigned long dropped = q->dropped;
    cond_broadcast(&q->wake);
    unlock(&q->lock);
    return dropped;
}

int queue_flush(struct queue *q)
{
    lock(&q->lock);
    int n = drain(q);
    q->dropped += (unsigned long)n;
    unlock(&q->lock);
    return n;
}

int queue_push(struct queue *q, struct pkt *k)
{
    int c = k->hdr.pri;
    int64_t t = now_ms();

    lock(&q->lock);
    if (q->closing) {
        unlock(&q->lock);
        return QUEUE_CLOSED;
    }
    if (q->count[c] == q->cap) {
        q->dropped++;
        unlock(&q->lock);
        return QUEUE_FULL;
    }
    struct qitem *it = &q->ring[c][(q->head[c] + q->count[c]) % q->cap];
    it->pkt = k;
    it->t = t;
    q->count[c]++;
    q->total++;
    cond_signal(&q->wake);
    unlock(&q->lock);
    return QUEUE_OK;
}

struct pkt *queue_pop(struct queue *q)
{
    lock(&q->lock);
    for (;;) {
        if (q->closing) {
            unlock(&q->lock);
            return NULL;
        }
        if (q->total == 0) {
            cond_wait(&q->wake, &q->lock);
            continue;
        }
        if (q->rate > 0 && q->tokens < TOKEN) {
            /* out of tokens: the timer thread wakes us when it refills,
             * the timeout is only a safety net */
            struct timespec until = in_ms(TOKEN_WAIT_MS);
            int err = pthread_cond_timedwait(&q->wake, &q->lock, &until);
            if (err && err != ETIMEDOUT)
                die_err("pthread_cond_timedwait", err);
            continue;
        }
        break;
    }

    if (q->rate > 0)
        q->tokens -= TOKEN;
    struct pkt *k = take(q, pick(q));
    unlock(&q->lock);
    return k;
}

int queue_len(struct queue *q)
{
    lock(&q->lock);
    int n = q->total;
    unlock(&q->lock);
    return n;
}

void queue_set_qos(struct queue *q, const int *weight, int age_ms)
{
    lock(&q->lock);
    for (int c = 0; c < QUEUE_CLASSES; c++)
        q->weight[c] = weight[c];
    q->age_ms = age_ms;
    q->cur = 0;
    q->credit = q->weight[0];
    unlock(&q->lock);
}

void queue_set_rate(struct queue *q, int rate)
{
    lock(&q->lock);
    q->rate = rate;
    /* a bucket of about 100 ms worth of frames, at least one */
    q->burst = rate > 0 ? (rate / 10 > 0 ? rate / 10 : 1) * TOKEN : 0;
    q->tokens = q->burst;
    cond_broadcast(&q->wake);
    unlock(&q->lock);
}

int queue_rate(struct queue *q)
{
    lock(&q->lock);
    int r = q->rate;
    unlock(&q->lock);
    return r;
}

/* called by the timer thread: ms milliseconds have passed */
void queue_refill(struct queue *q, long ms)
{
    lock(&q->lock);
    if (q->rate > 0) {
        q->tokens += q->rate * ms;
        if (q->tokens > q->burst)
            q->tokens = q->burst;
        if (q->total > 0)
            cond_broadcast(&q->wake);
    }
    unlock(&q->lock);
}
