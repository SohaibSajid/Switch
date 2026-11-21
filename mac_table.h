#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include "frame.h"

/* one lock per bucket (striped locking). "clear mac" holds all of them at
 * once, and ThreadSanitizer gives up when a thread holds 64 locks, so keep
 * this well below that */
#define MAC_BUCKETS 32
/* entries live in a fixed pool and are linked by index, not by pointer, so
 * the whole table can sit in shared memory and survive a crash */
#define MAC_POOL 32768
#define MAC_MAX_PORT 66

enum { MAC_ERR = -1, MAC_NEW = 0, MAC_SAME = 1, MAC_MOVED = 2, MAC_LIMIT = 3 };

struct mac_entry {
    uint8_t mac[MAC_LEN];
    int port;
    int next;
    time_t seen;
    unsigned long use;
};

struct mac_bucket {
    pthread_mutex_t lock;
    int head;
};

struct mac_table {
    struct mac_bucket bucket[MAC_BUCKETS];
    pthread_mutex_t free_lock;
    int free_head;
    atomic_int max_entries;
    atomic_int max_per_port;
    atomic_int trunk_port;
    atomic_int count;
    atomic_ulong clock;
    atomic_int per_port[MAC_MAX_PORT + 1];
    struct mac_entry pool[MAC_POOL];
};

int mac_table_init(struct mac_table *t, int max_entries, int max_per_port);
void mac_table_destroy(struct mac_table *t);
int mac_table_lookup(struct mac_table *t, const uint8_t *mac);
int mac_table_insert(struct mac_table *t, const uint8_t *mac, int port);
int mac_table_flush_port(struct mac_table *t, int port);
int mac_table_age(struct mac_table *t, int max_age);
size_t mac_table_count(struct mac_table *t);
void mac_table_set_limits(struct mac_table *t, int max_entries, int max_per_port);
void mac_table_set_trunk(struct mac_table *t, int port);
int mac_table_dump(struct mac_table *t, FILE *f);
int mac_table_clear(struct mac_table *t);
int mac_table_port_count(struct mac_table *t, int port);

/* table in a POSIX shared memory object, for the supervisor */
struct mac_table *mac_table_shm_create(const char *name, int max_entries, int max_per_port);
struct mac_table *mac_table_shm_attach(const char *name, int *fixed);
void mac_table_shm_close(struct mac_table *t);
void mac_table_shm_unlink(const char *name);

#endif
