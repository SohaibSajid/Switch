#define _POSIX_C_SOURCE 200809L

#include "mac_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void lock(pthread_mutex_t *m)
{
    int err = pthread_mutex_lock(m);
    if (err) {
        fprintf(stderr, "pthread_mutex_lock: %s\n", strerror(err));
        abort();
    }
}

static void unlock(pthread_mutex_t *m)
{
    int err = pthread_mutex_unlock(m);
    if (err) {
        fprintf(stderr, "pthread_mutex_unlock: %s\n", strerror(err));
        abort();
    }
}

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

static struct mac_entry *find(struct mac_bucket *b, const uint8_t *mac)
{
    for (struct mac_entry *e = b->head; e; e = e->next)
        if (memcmp(e->mac, mac, MAC_LEN) == 0)
            return e;
    return NULL;
}

int mac_table_init(struct mac_table *t)
{
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

    lock(&b->lock);
    struct mac_entry *e = find(b, mac);
    int port = e ? e->port : -1;
    unlock(&b->lock);

    return port;
}

int mac_table_insert(struct mac_table *t, const uint8_t *mac, int port)
{
    struct mac_bucket *b = &t->bucket[mac_hash(mac)];
    time_t ts = now();
    int old = -1;
    int ret;

    lock(&b->lock);
    struct mac_entry *e = find(b, mac);
    if (e) {
        old = e->port;
        e->port = port;
        e->seen = ts;
        ret = old == port ? MAC_SAME : MAC_MOVED;
    } else {
        e = malloc(sizeof *e);
        if (!e) {
            unlock(&b->lock);
            perror("malloc");
            return MAC_ERR;
        }
        memcpy(e->mac, mac, MAC_LEN);
        e->port = port;
        e->seen = ts;
        e->next = b->head;
        b->head = e;
        ret = MAC_NEW;
    }
    unlock(&b->lock);

    if (ret == MAC_MOVED) {
        char s[MAC_STR_LEN];
        mac_format(mac, s);
        printf("MAC moved: port %d -> port %d (%s)\n", old, port, s);
    }
    return ret;
}

size_t mac_table_count(struct mac_table *t)
{
    size_t n = 0;
    for (int i = 0; i < MAC_BUCKETS; i++) {
        lock(&t->bucket[i].lock);
        for (struct mac_entry *e = t->bucket[i].head; e; e = e->next)
            n++;
        unlock(&t->bucket[i].lock);
    }
    return n;
}
