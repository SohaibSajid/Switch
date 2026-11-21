#ifndef SHM_STATS_H
#define SHM_STATS_H

#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>

#define STATS_NAME "/vswitch_stats"
#define STATS_MAX_PORTS 66

enum { STAT_IN, STAT_OUT, STAT_DROPPED, STAT_MIRRORED, STAT_FIELDS };

struct port_stats {
    int up;
    uint64_t count[STAT_FIELDS];
};

struct shm_stats {
    pthread_mutex_t lock;
    pid_t pid;
    int nports;
    struct port_stats port[STATS_MAX_PORTS + 1];
};

struct shm_stats *stats_create(const char *name, int nports);
void stats_destroy(struct shm_stats *s, const char *name);
struct shm_stats *stats_attach(const char *name);
void stats_detach(struct shm_stats *s);

void stats_add(struct shm_stats *s, int port, int field, uint64_t n);
void stats_port_up(struct shm_stats *s, int port, int up);
void stats_snapshot(struct shm_stats *s, struct shm_stats *out);

#endif
