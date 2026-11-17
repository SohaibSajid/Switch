#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include "frame.h"

#define MAC_BUCKETS 16
#define MAC_MAX_PORT 64

enum { MAC_ERR = -1, MAC_NEW = 0, MAC_SAME = 1, MAC_MOVED = 2, MAC_LIMIT = 3 };

struct mac_entry {
    uint8_t mac[MAC_LEN];
    int port;
    time_t seen;
    unsigned long use;
    struct mac_entry *next;
};

struct mac_bucket {
    pthread_mutex_t lock;
    struct mac_entry *head;
};

struct mac_table {
    struct mac_bucket bucket[MAC_BUCKETS];
    atomic_int max_entries;
    atomic_int max_per_port;
    atomic_int count;
    atomic_ulong clock;
    atomic_int per_port[MAC_MAX_PORT + 1];
};

int mac_table_init(struct mac_table *t, int max_entries, int max_per_port);
void mac_table_destroy(struct mac_table *t);
int mac_table_lookup(struct mac_table *t, const uint8_t *mac);
int mac_table_insert(struct mac_table *t, const uint8_t *mac, int port);
int mac_table_flush_port(struct mac_table *t, int port);
int mac_table_age(struct mac_table *t, int max_age);
size_t mac_table_count(struct mac_table *t);
void mac_table_set_limits(struct mac_table *t, int max_entries, int max_per_port);
int mac_table_dump(struct mac_table *t, FILE *f);

#endif
