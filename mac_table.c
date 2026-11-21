#define _POSIX_C_SOURCE 200809L

#include "mac_table.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "log.h"
#include "sync.h"

#define EVICT_TRIES 8

static unsigned int mac_hash(const uint8_t *mac)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < MAC_LEN; i++) {
        h ^= mac[i];
        h *= 16777619u;
    }
    return h % MAC_BUCKETS;
}

static time_t now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        perror("clock_gettime");
        return 0;
    }
    return ts.tv_sec;
}

static unsigned long tick(struct mac_table *t)
{
    return atomic_fetch_add(&t->clock, 1) + 1;
}

/* the mutexes are robust: if a switch process dies while holding one, the
 * next owner gets EOWNERDEAD and marks it consistent again */
static void tlock(pthread_mutex_t *m)
{
    int err = pthread_mutex_lock(m);
    if (err == EOWNERDEAD)
        err = pthread_mutex_consistent(m);
    if (err)
        die_err("mac table lock", err);
}

static int find(struct mac_table *t, struct mac_bucket *b, const uint8_t *mac)
{
    for (int i = b->head; i != -1; i = t->pool[i].next)
        if (memcmp(t->pool[i].mac, mac, MAC_LEN) == 0)
            return i;
    return -1;
}

static int entry_alloc(struct mac_table *t)
{
    tlock(&t->free_lock);
    int i = t->free_head;
    if (i != -1)
        t->free_head = t->pool[i].next;
    unlock(&t->free_lock);
    return i;
}

static void entry_free(struct mac_table *t, int i)
{
    tlock(&t->free_lock);
    t->pool[i].next = t->free_head;
    t->free_head = i;
    unlock(&t->free_lock);
}

/* remove *link from its chain; the bucket lock is held */
static void unlink_entry(struct mac_table *t, int *link)
{
    int i = *link;
    *link = t->pool[i].next;
    atomic_fetch_sub(&t->per_port[t->pool[i].port], 1);
    atomic_fetch_sub(&t->count, 1);
    entry_free(t, i);
}

static int limit(struct mac_table *t)
{
    int k = atomic_load(&t->max_entries);
    return k > 0 && k < MAC_POOL ? k : MAC_POOL;
}

static int port_has_room(struct mac_table *t, int port)
{
    if (port == atomic_load(&t->trunk_port))
        return 1;
    int m = atomic_load(&t->max_per_port);
    return m == 0 || atomic_load(&t->per_port[port]) < m;
}

static int evict_lru(struct mac_table *t)
{
    uint8_t victim[MAC_LEN];
    unsigned long oldest = 0;
    int found = 0;

    for (int i = 0; i < MAC_BUCKETS; i++) {
        tlock(&t->bucket[i].lock);
        for (int e = t->bucket[i].head; e != -1; e = t->pool[e].next) {
            if (!found || t->pool[e].use < oldest) {
                oldest = t->pool[e].use;
                memcpy(victim, t->pool[e].mac, MAC_LEN);
                found = 1;
            }
        }
        unlock(&t->bucket[i].lock);
    }
    if (!found)
        return 0;

    struct mac_bucket *b = &t->bucket[mac_hash(victim)];
    int removed = 0;
    tlock(&b->lock);
    for (int *link = &b->head; *link != -1; link = &t->pool[*link].next) {
        if (memcmp(t->pool[*link].mac, victim, MAC_LEN) == 0) {
            if (t->pool[*link].use == oldest) {
                unlink_entry(t, link);
                removed = 1;
            }
            break;
        }
    }
    unlock(&b->lock);

    if (removed) {
        char s[MAC_STR_LEN];
        mac_format(victim, s);
        log_msg("MAC table full, evicted LRU entry %s", s);
    }
    return removed;
}

/* take one slot of the size limit, evicting the oldest entry if needed */
static int reserve(struct mac_table *t)
{
    for (int tries = 0; tries < EVICT_TRIES;) {
        int k = limit(t);
        int c = atomic_load(&t->count);
        if (c < k) {
            if (atomic_compare_exchange_weak(&t->count, &c, c + 1))
                return 0;
            continue;
        }
        if (!evict_lru(t))
            tries++;
    }
    return -1;
}

static int setup(struct mac_table *t, int max_entries, int max_per_port, int shared)
{
    pthread_mutexattr_t attr;
    int err;

    atomic_init(&t->max_entries, max_entries);
    atomic_init(&t->max_per_port, max_per_port);
    atomic_init(&t->trunk_port, 0);
    atomic_init(&t->count, 0);
    atomic_init(&t->clock, 0);
    for (int i = 0; i <= MAC_MAX_PORT; i++)
        atomic_init(&t->per_port[i], 0);

    for (int i = 0; i < MAC_POOL; i++)
        t->pool[i].next = i + 1 < MAC_POOL ? i + 1 : -1;
    t->free_head = 0;

    if ((err = pthread_mutexattr_init(&attr)) ||
        (err = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST)) ||
        (shared && (err = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED))))
        return err;

    err = pthread_mutex_init(&t->free_lock, &attr);
    for (int i = 0; !err && i < MAC_BUCKETS; i++) {
        t->bucket[i].head = -1;
        err = pthread_mutex_init(&t->bucket[i].lock, &attr);
    }
    int err2 = pthread_mutexattr_destroy(&attr);
    return err ? err : err2;
}

int mac_table_init(struct mac_table *t, int max_entries, int max_per_port)
{
    return setup(t, max_entries, max_per_port, 0);
}

void mac_table_destroy(struct mac_table *t)
{
    for (int i = 0; i < MAC_BUCKETS; i++)
        pthread_mutex_destroy(&t->bucket[i].lock);
    pthread_mutex_destroy(&t->free_lock);
}

int mac_table_lookup(struct mac_table *t, const uint8_t *mac)
{
    struct mac_bucket *b = &t->bucket[mac_hash(mac)];
    unsigned long use = tick(t);
    int port = -1;

    tlock(&b->lock);
    int e = find(t, b, mac);
    if (e != -1) {
        port = t->pool[e].port;
        t->pool[e].use = use;
    }
    unlock(&b->lock);

    return port;
}

int mac_table_insert(struct mac_table *t, const uint8_t *mac, int port)
{
    if (port < 1 || port > MAC_MAX_PORT)
        return MAC_ERR;

    struct mac_bucket *b = &t->bucket[mac_hash(mac)];
    time_t ts = now();
    unsigned long use = tick(t);
    int old = -1;
    int ret = MAC_SAME;

    tlock(&b->lock);
    int e = find(t, b, mac);
    if (e != -1) {
        old = t->pool[e].port;
        if (old != port) {
            if (!port_has_room(t, port)) {
                unlock(&b->lock);
                return MAC_LIMIT;
            }
            atomic_fetch_sub(&t->per_port[old], 1);
            atomic_fetch_add(&t->per_port[port], 1);
            t->pool[e].port = port;
            ret = MAC_MOVED;
        }
        t->pool[e].seen = ts;
        t->pool[e].use = use;
    }
    unlock(&b->lock);

    if (ret == MAC_MOVED) {
        char s[MAC_STR_LEN];
        mac_format(mac, s);
        log_msg("MAC moved: port %d -> port %d (%s)", old, port, s);
    }
    if (e != -1)
        return ret;

    if (!port_has_room(t, port))
        return MAC_LIMIT;
    if (reserve(t) == -1)
        return MAC_ERR;

    e = entry_alloc(t);
    if (e == -1) {
        atomic_fetch_sub(&t->count, 1);
        return MAC_ERR;
    }

    tlock(&b->lock);
    if (find(t, b, mac) != -1) {
        /* somebody else learned it while we were not holding the lock */
        unlock(&b->lock);
        entry_free(t, e);
        atomic_fetch_sub(&t->count, 1);
        return mac_table_insert(t, mac, port);
    }
    memcpy(t->pool[e].mac, mac, MAC_LEN);
    t->pool[e].port = port;
    t->pool[e].seen = ts;
    t->pool[e].use = use;
    t->pool[e].next = b->head;
    b->head = e;
    atomic_fetch_add(&t->per_port[port], 1);
    unlock(&b->lock);

    return MAC_NEW;
}

int mac_table_flush_port(struct mac_table *t, int port)
{
    int n = 0;
    for (int i = 0; i < MAC_BUCKETS; i++) {
        struct mac_bucket *b = &t->bucket[i];
        tlock(&b->lock);
        int *link = &b->head;
        while (*link != -1) {
            if (t->pool[*link].port == port) {
                unlink_entry(t, link);
                n++;
            } else {
                link = &t->pool[*link].next;
            }
        }
        unlock(&b->lock);
    }
    return n;
}

int mac_table_age(struct mac_table *t, int max_age)
{
    time_t ts = now();
    int n = 0;

    for (int i = 0; i < MAC_BUCKETS; i++) {
        struct mac_bucket *b = &t->bucket[i];
        tlock(&b->lock);
        int *link = &b->head;
        while (*link != -1) {
            struct mac_entry *e = &t->pool[*link];
            if (ts - e->seen >= max_age) {
                char s[MAC_STR_LEN];
                mac_format(e->mac, s);
                log_msg("MAC aged out: %s (port %d)", s, e->port);
                unlink_entry(t, link);
                n++;
            } else {
                link = &e->next;
            }
        }
        unlock(&b->lock);
    }
    return n;
}

size_t mac_table_count(struct mac_table *t)
{
    return (size_t)atomic_load(&t->count);
}

void mac_table_set_limits(struct mac_table *t, int max_entries, int max_per_port)
{
    atomic_store(&t->max_entries, max_entries);
    atomic_store(&t->max_per_port, max_per_port);
}

void mac_table_set_trunk(struct mac_table *t, int port)
{
    atomic_store(&t->trunk_port, port);
}

int mac_table_dump(struct mac_table *t, FILE *f)
{
    time_t ts = now();
    int n = 0;

    fprintf(f, "%-17s  %4s  %s\n", "MAC", "PORT", "IDLE");
    for (int i = 0; i < MAC_BUCKETS; i++) {
        tlock(&t->bucket[i].lock);
        for (int e = t->bucket[i].head; e != -1; e = t->pool[e].next) {
            char s[MAC_STR_LEN];
            mac_format(t->pool[e].mac, s);
            fprintf(f, "%-17s  %4d  %lds\n", s, t->pool[e].port,
                    (long)(ts - t->pool[e].seen));
            n++;
        }
        unlock(&t->bucket[i].lock);
    }
    return n;
}

/* takes every bucket lock, lowest first, so nobody sees half a clear */
int mac_table_clear(struct mac_table *t)
{
    int n = 0;

    for (int i = 0; i < MAC_BUCKETS; i++)
        tlock(&t->bucket[i].lock);

    for (int i = 0; i < MAC_BUCKETS; i++) {
        int *link = &t->bucket[i].head;
        while (*link != -1) {
            unlink_entry(t, link);
            n++;
        }
    }

    for (int i = MAC_BUCKETS - 1; i >= 0; i--)
        unlock(&t->bucket[i].lock);
    return n;
}

int mac_table_port_count(struct mac_table *t, int port)
{
    if (port < 1 || port > MAC_MAX_PORT)
        return 0;
    return atomic_load(&t->per_port[port]);
}

/* ---- shared memory, used by the supervisor ---- */

/* After a crash: nobody else is using the table, so walk it, throw away
 * anything that does not look right and rebuild the counters and the free
 * list from what is left. */
static void repair(struct mac_table *t, int *fixed)
{
    static unsigned char seen[MAC_POOL];
    int kept = 0;
    int dropped = 0;

    memset(seen, 0, sizeof seen);
    tlock(&t->free_lock);
    unlock(&t->free_lock);
    for (int i = 0; i <= MAC_MAX_PORT; i++)
        atomic_store(&t->per_port[i], 0);

    for (int b = 0; b < MAC_BUCKETS; b++) {
        tlock(&t->bucket[b].lock);
        int *link = &t->bucket[b].head;
        while (*link != -1) {
            int i = *link;
            struct mac_entry *e = i >= 0 && i < MAC_POOL ? &t->pool[i] : NULL;
            if (!e || seen[i] || e->port < 1 || e->port > MAC_MAX_PORT ||
                mac_hash(e->mac) != (unsigned int)b) {
                *link = -1;
                dropped++;
                break;
            }
            seen[i] = 1;
            kept++;
            atomic_fetch_add(&t->per_port[e->port], 1);
            link = &e->next;
        }
        unlock(&t->bucket[b].lock);
    }
    atomic_store(&t->count, kept);

    t->free_head = -1;
    for (int i = MAC_POOL - 1; i >= 0; i--) {
        if (!seen[i]) {
            t->pool[i].next = t->free_head;
            t->free_head = i;
        }
    }
    if (fixed)
        *fixed = dropped;
}

static struct mac_table *map(const char *name, int create)
{
    int fd = shm_open(name, create ? O_CREAT | O_EXCL | O_RDWR : O_RDWR, 0600);
    if (fd == -1) {
        if (create || errno != ENOENT)
            perror(name);
        return NULL;
    }
    if (create && ftruncate(fd, sizeof(struct mac_table)) == -1) {
        perror("ftruncate");
        close(fd);
        shm_unlink(name);
        return NULL;
    }
    struct mac_table *t = mmap(NULL, sizeof *t, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, 0);
    int saved = errno;
    if (close(fd) == -1)
        perror("close");
    if (t == MAP_FAILED) {
        errno = saved;
        perror("mmap");
        if (create)
            shm_unlink(name);
        return NULL;
    }
    return t;
}

struct mac_table *mac_table_shm_create(const char *name, int max_entries, int max_per_port)
{
    struct mac_table *t = map(name, 1);
    if (!t)
        return NULL;
    int err = setup(t, max_entries, max_per_port, 1);
    if (err) {
        fprintf(stderr, "mac table init: %s\n", strerror(err));
        munmap(t, sizeof *t);
        shm_unlink(name);
        return NULL;
    }
    return t;
}

/* NULL if there is no table yet; *fixed is the number of broken entries
 * that had to be thrown away */
struct mac_table *mac_table_shm_attach(const char *name, int *fixed)
{
    struct mac_table *t = map(name, 0);
    if (t)
        repair(t, fixed);
    return t;
}

void mac_table_shm_close(struct mac_table *t)
{
    if (munmap(t, sizeof *t) == -1)
        perror("munmap");
}

void mac_table_shm_unlink(const char *name)
{
    if (shm_unlink(name) == -1 && errno != ENOENT)
        perror("shm_unlink");
}
