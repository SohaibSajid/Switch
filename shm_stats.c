#define _POSIX_C_SOURCE 200809L

#include "shm_stats.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "sync.h"

static void stats_lock(struct shm_stats *s)
{
    int err = pthread_mutex_lock(&s->lock);
    if (err == EOWNERDEAD)
        err = pthread_mutex_consistent(&s->lock);
    if (err)
        die_err("stats lock", err);
}

static void stats_unlock(struct shm_stats *s)
{
    unlock(&s->lock);
}

struct shm_stats *stats_create(const char *name, int nports)
{
    if (shm_unlink(name) == -1 && errno != ENOENT)
        perror("shm_unlink");

    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) {
        perror("shm_open");
        return NULL;
    }
    if (ftruncate(fd, sizeof(struct shm_stats)) == -1) {
        perror("ftruncate");
        close(fd);
        shm_unlink(name);
        return NULL;
    }

    struct shm_stats *s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, 0);
    if (close(fd) == -1)
        perror("close");
    if (s == MAP_FAILED) {
        perror("mmap");
        shm_unlink(name);
        return NULL;
    }

    memset(s, 0, sizeof *s);
    s->pid = getpid();
    s->nports = nports;

    pthread_mutexattr_t attr;
    int err;
    if ((err = pthread_mutexattr_init(&attr)) ||
        (err = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED)) ||
        (err = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST)) ||
        (err = pthread_mutex_init(&s->lock, &attr)) ||
        (err = pthread_mutexattr_destroy(&attr))) {
        fprintf(stderr, "stats mutex: %s\n", strerror(err));
        munmap(s, sizeof *s);
        shm_unlink(name);
        return NULL;
    }
    return s;
}

void stats_destroy(struct shm_stats *s, const char *name)
{
    pthread_mutex_destroy(&s->lock);
    if (munmap(s, sizeof *s) == -1)
        perror("munmap");
    if (shm_unlink(name) == -1)
        perror("shm_unlink");
}

struct shm_stats *stats_attach(const char *name)
{
    int fd = shm_open(name, O_RDWR, 0);
    if (fd == -1)
        return NULL;

    struct shm_stats *s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, 0);
    int saved = errno;
    if (close(fd) == -1)
        perror("close");
    if (s == MAP_FAILED) {
        errno = saved;
        return NULL;
    }
    return s;
}

void stats_detach(struct shm_stats *s)
{
    if (munmap(s, sizeof *s) == -1)
        perror("munmap");
}

void stats_add(struct shm_stats *s, int port, int field, uint64_t n)
{
    if (port < 1 || port > STATS_MAX_PORTS)
        return;
    stats_lock(s);
    s->port[port].count[field] += n;
    stats_unlock(s);
}

void stats_port_up(struct shm_stats *s, int port, int up)
{
    if (port < 1 || port > STATS_MAX_PORTS)
        return;
    stats_lock(s);
    s->port[port].up = up;
    if (up)
        memset(s->port[port].count, 0, sizeof s->port[port].count);
    stats_unlock(s);
}

void stats_snapshot(struct shm_stats *s, struct shm_stats *out)
{
    stats_lock(s);
    out->pid = s->pid;
    out->nports = s->nports;
    memcpy(out->port, s->port, sizeof out->port);
    stats_unlock(s);
}
