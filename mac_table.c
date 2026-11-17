#define _POSIX_C_SOURCE 200809L

#include "mac_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static struct mac_entry *find(struct mac_bucket *b, const uint8_t *mac)
{
    for (struct mac_entry *e = b->head; e; e = e->next)
        if (memcmp(e->mac, mac, MAC_LEN) == 0)
            return e;
    return NULL;
}

static void unlink_entry(struct mac_table *t, struct mac_entry **link)
{
    struct mac_entry *e = *link;
    *link = e->next;
    atomic_fetch_sub(&t->per_port[e->port], 1);
    atomic_fetch_sub(&t->count, 1);
    free(e);
}

static int port_has_room(struct mac_table *t, int port)
{
    return t->max_per_port == 0 ||
           atomic_load(&t->per_port[port]) < t->max_per_port;
}

static int evict_lru(struct mac_table *t)
{
    uint8_t victim[MAC_LEN];
    unsigned long oldest = 0;
    int found = 0;

    for (int i = 0; i < MAC_BUCKETS; i++) {
        lock(&t->bucket[i].lock);
        for (struct mac_entry *e = t->bucket[i].head; e; e = e->next) {
            if (!found || e->use < oldest) {
                oldest = e->use;
                memcpy(victim, e->mac, MAC_LEN);
                found = 1;
            }
        }
        unlock(&t->bucket[i].lock);
    }
    if (!found)
        return 0;

    struct mac_bucket *b = &t->bucket[mac_hash(victim)];
    int removed = 0;
    lock(&b->lock);
    for (struct mac_entry **link = &b->head; *link; link = &(*link)->next) {
        if (memcmp((*link)->mac, victim, MAC_LEN) == 0) {
            if ((*link)->use == oldest) {
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
        printf("MAC table full, evicted LRU entry %s\n", s);
    }
    return removed;
}

static int reserve(struct mac_table *t)
{
    if (t->max_entries == 0) {
        atomic_fetch_add(&t->count, 1);
        return 0;
    }
    for (int tries = 0; tries < EVICT_TRIES;) {
        int c = atomic_load(&t->count);
        if (c < t->max_entries) {
            if (atomic_compare_exchange_weak(&t->count, &c, c + 1))
                return 0;
            continue;
        }
        if (!evict_lru(t))
            tries++;
    }
    return -1;
}

int mac_table_init(struct mac_table *t, int max_entries, int max_per_port)
{
    t->max_entries = max_entries;
    t->max_per_port = max_per_port;
    atomic_init(&t->count, 0);
    atomic_init(&t->clock, 0);
    for (int i = 0; i <= MAC_MAX_PORT; i++)
        atomic_init(&t->per_port[i], 0);

    for (int i = 0; i < MAC_BUCKETS; i++) {
        t->bucket[i].head = NULL;
        int err = pthread_mutex_init(&t->bucket[i].lock, NULL);
        if (err) {
            while (--i >= 0)
                pthread_mutex_destroy(&t->bucket[i].lock);
            return err;
        }
    }
    return 0;
}

void mac_table_destroy(struct mac_table *t)
{
    for (int i = 0; i < MAC_BUCKETS; i++) {
        struct mac_entry *e = t->bucket[i].head;
        while (e) {
            struct mac_entry *next = e->next;
            free(e);
            e = next;
        }
        t->bucket[i].head = NULL;
        pthread_mutex_destroy(&t->bucket[i].lock);
    }
}

int mac_table_lookup(struct mac_table *t, const uint8_t *mac)
{
    struct mac_bucket *b = &t->bucket[mac_hash(mac)];
    unsigned long use = tick(t);

    lock(&b->lock);
    struct mac_entry *e = find(b, mac);
    int port = -1;
    if (e) {
        port = e->port;
        e->use = use;
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

    lock(&b->lock);
    struct mac_entry *e = find(b, mac);
    if (e) {
        old = e->port;
        if (old != port) {
            if (!port_has_room(t, port)) {
                unlock(&b->lock);
                return MAC_LIMIT;
            }
            atomic_fetch_sub(&t->per_port[old], 1);
            atomic_fetch_add(&t->per_port[port], 1);
            e->port = port;
            ret = MAC_MOVED;
        }
        e->seen = ts;
        e->use = use;
    }
    unlock(&b->lock);

    if (ret == MAC_MOVED) {
        char s[MAC_STR_LEN];
        mac_format(mac, s);
        printf("MAC moved: port %d -> port %d (%s)\n", old, port, s);
    }
    if (e)
        return ret;

    if (!port_has_room(t, port))
        return MAC_LIMIT;
    if (reserve(t) == -1)
        return MAC_ERR;

    e = malloc(sizeof *e);
    if (!e) {
        perror("malloc");
        atomic_fetch_sub(&t->count, 1);
        return MAC_ERR;
    }

    lock(&b->lock);
    if (find(b, mac)) {
        unlock(&b->lock);
        free(e);
        atomic_fetch_sub(&t->count, 1);
        return mac_table_insert(t, mac, port);
    }
    memcpy(e->mac, mac, MAC_LEN);
    e->port = port;
    e->seen = ts;
    e->use = use;
    e->next = b->head;
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
        lock(&b->lock);
        struct mac_entry **link = &b->head;
        while (*link) {
            if ((*link)->port == port) {
                unlink_entry(t, link);
                n++;
            } else {
                link = &(*link)->next;
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
        lock(&b->lock);
        struct mac_entry **link = &b->head;
        while (*link) {
            if (ts - (*link)->seen >= max_age) {
                char s[MAC_STR_LEN];
                mac_format((*link)->mac, s);
                printf("MAC aged out: %s (port %d)\n", s, (*link)->port);
                unlink_entry(t, link);
                n++;
            } else {
                link = &(*link)->next;
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
