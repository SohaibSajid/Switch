#define _POSIX_C_SOURCE 200809L

/*
 * Load generator for the MAC table lock question: the same hash table with
 *   global   one mutex for the whole table
 *   striped  one mutex per bucket (what mac_table.c does)
 *   rwlock   one read-write lock: lookups share it, learning excludes
 *
 * Every thread runs a mix of lookups (what the forwarding code does for each
 * frame) and learns (the source MAC of a frame), on the same table.
 *
 *   ./bench [-t max_threads] [-s seconds] [-w percent_learns] [-u 0|1]
 *
 * -u 0 leaves out the LRU stamp that a lookup writes into the entry (the
 * real table does write it), to see what that costs.
 *
 * prints CSV: mode,threads,ops_per_sec
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MACS 4096
#define BUCKETS 32

enum { GLOBAL, STRIPED, RWLOCK, MODES };
static const char *mode_name[MODES] = { "global", "striped", "rwlock" };

struct entry {
    uint8_t mac[6];
    int port;
    atomic_ulong use;
    struct entry *next;
};

/* one cache line each, so that two stripes do not fight over the same line */
struct stripe {
    _Alignas(64) pthread_mutex_t lock;
    char pad[64];
};

struct table {
    int mode;
    pthread_mutex_t global;
    struct stripe stripe[BUCKETS];
    pthread_rwlock_t rw;
    struct entry *head[BUCKETS];
    struct entry pool[MACS];
};

struct worker {
    struct table *t;
    int id;
    int learn_percent;
    unsigned long ops;
};

static pthread_barrier_t start_line;
static atomic_int stop;
static int lru_stamp = 1;

static void check(int err, const char *what)
{
    if (err) {
        fprintf(stderr, "%s: %s\n", what, strerror(err));
        exit(1);
    }
}

static unsigned int hash(const uint8_t *mac)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) {
        h ^= mac[i];
        h *= 16777619u;
    }
    return h % BUCKETS;
}

static void make_mac(int n, uint8_t *mac)
{
    mac[0] = 0x02;
    mac[1] = 0;
    mac[2] = (uint8_t)(n >> 8);
    mac[3] = (uint8_t)n;
    mac[4] = (uint8_t)(n * 7);
    mac[5] = (uint8_t)(n * 13);
}

static void table_init(struct table *t, int mode)
{
    memset(t, 0, sizeof *t);
    t->mode = mode;
    check(pthread_mutex_init(&t->global, NULL), "mutex_init");
    for (int i = 0; i < BUCKETS; i++)
        check(pthread_mutex_init(&t->stripe[i].lock, NULL), "mutex_init");
    check(pthread_rwlock_init(&t->rw, NULL), "rwlock_init");

    for (int n = 0; n < MACS; n++) {
        struct entry *e = &t->pool[n];
        make_mac(n, e->mac);
        e->port = n % 8 + 1;
        unsigned int b = hash(e->mac);
        e->next = t->head[b];
        t->head[b] = e;
    }
}

static void table_destroy(struct table *t)
{
    pthread_mutex_destroy(&t->global);
    for (int i = 0; i < BUCKETS; i++)
        pthread_mutex_destroy(&t->stripe[i].lock);
    pthread_rwlock_destroy(&t->rw);
}

static struct entry *find(struct table *t, unsigned int b, const uint8_t *mac)
{
    for (struct entry *e = t->head[b]; e; e = e->next)
        if (memcmp(e->mac, mac, 6) == 0)
            return e;
    return NULL;
}

static void enter(struct table *t, unsigned int b, int write)
{
    if (t->mode == GLOBAL)
        check(pthread_mutex_lock(&t->global), "lock");
    else if (t->mode == STRIPED)
        check(pthread_mutex_lock(&t->stripe[b].lock), "lock");
    else if (write)
        check(pthread_rwlock_wrlock(&t->rw), "wrlock");
    else
        check(pthread_rwlock_rdlock(&t->rw), "rdlock");
}

static void leave(struct table *t, unsigned int b)
{
    if (t->mode == GLOBAL)
        check(pthread_mutex_unlock(&t->global), "unlock");
    else if (t->mode == STRIPED)
        check(pthread_mutex_unlock(&t->stripe[b].lock), "unlock");
    else
        check(pthread_rwlock_unlock(&t->rw), "unlock");
}

static int lookup(struct table *t, const uint8_t *mac, unsigned long stamp)
{
    unsigned int b = hash(mac);
    int port = -1;

    enter(t, b, 0);
    struct entry *e = find(t, b, mac);
    if (e) {
        port = e->port;
        /* the LRU stamp is an atomic so that it can be written under a
         * read lock */
        if (lru_stamp)
            atomic_store_explicit(&e->use, stamp, memory_order_relaxed);
    }
    leave(t, b);
    return port;
}

static void learn(struct table *t, const uint8_t *mac, int port, unsigned long stamp)
{
    unsigned int b = hash(mac);

    enter(t, b, 1);
    struct entry *e = find(t, b, mac);
    if (e) {
        e->port = port;
        atomic_store_explicit(&e->use, stamp, memory_order_relaxed);
    }
    leave(t, b);
}

static void *worker(void *arg)
{
    struct worker *w = arg;
    uint32_t x = 2463534242u + (uint32_t)w->id * 7919u;
    uint8_t mac[6];
    unsigned long ops = 0;

    int r = pthread_barrier_wait(&start_line);
    if (r != 0 && r != PTHREAD_BARRIER_SERIAL_THREAD)
        check(r, "barrier_wait");
    while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        make_mac((int)(x % MACS), mac);
        if ((int)((x >> 12) % 100) < w->learn_percent)
            learn(w->t, mac, (int)(x % 8) + 1, ops);
        else
            lookup(w->t, mac, ops);
        ops++;
    }
    w->ops = ops;
    return NULL;
}

static double run(int mode, int threads, int secs, int learn_percent)
{
    static struct table t;
    pthread_t th[64];
    struct worker w[64];

    table_init(&t, mode);
    atomic_store(&stop, 0);
    check(pthread_barrier_init(&start_line, NULL, (unsigned)threads + 1), "barrier_init");

    for (int i = 0; i < threads; i++) {
        w[i].t = &t;
        w[i].id = i;
        w[i].learn_percent = learn_percent;
        w[i].ops = 0;
        check(pthread_create(&th[i], NULL, worker, &w[i]), "pthread_create");
    }

    struct timespec t0, t1, nap = { secs, 0 };
    int r = pthread_barrier_wait(&start_line);
    if (r != 0 && r != PTHREAD_BARRIER_SERIAL_THREAD)
        check(r, "barrier_wait");
    if (clock_gettime(CLOCK_MONOTONIC, &t0) == -1) {
        perror("clock_gettime");
        exit(1);
    }
    while (nanosleep(&nap, &nap) == -1 && errno == EINTR)
        ;
    atomic_store(&stop, 1);
    if (clock_gettime(CLOCK_MONOTONIC, &t1) == -1) {
        perror("clock_gettime");
        exit(1);
    }

    unsigned long total = 0;
    for (int i = 0; i < threads; i++) {
        check(pthread_join(th[i], NULL), "pthread_join");
        total += w[i].ops;
    }
    check(pthread_barrier_destroy(&start_line), "barrier_destroy");
    table_destroy(&t);

    double secs_real = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    return (double)total / secs_real;
}

int main(int argc, char **argv)
{
    int max_threads = 16;
    int secs = 1;
    int learn_percent = 10;
    int opt;

    while ((opt = getopt(argc, argv, "t:s:w:u:")) != -1) {
        if (opt == 't')
            max_threads = atoi(optarg);
        else if (opt == 's')
            secs = atoi(optarg);
        else if (opt == 'w')
            learn_percent = atoi(optarg);
        else if (opt == 'u')
            lru_stamp = atoi(optarg) != 0;
        else {
            fprintf(stderr, "usage: %s [-t max_threads] [-s seconds] [-w percent_learns] [-u 0|1]\n", argv[0]);
            return 1;
        }
    }
    if (max_threads < 1 || max_threads > 64 || secs < 1 || learn_percent < 0 || learn_percent > 100) {
        fprintf(stderr, "bad option value\n");
        return 1;
    }

    printf("mode,threads,ops_per_sec\n");
    for (int threads = 1; threads <= max_threads; threads *= 2)
        for (int m = 0; m < MODES; m++) {
            double ops = run(m, threads, secs, learn_percent);
            printf("%s,%d,%.0f\n", mode_name[m], threads, ops);
            fflush(stdout);
        }
    return 0;
}
