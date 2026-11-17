#ifndef SYNC_H
#define SYNC_H

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void die_err(const char *what, int err)
{
    fprintf(stderr, "%s: %s\n", what, strerror(err));
    abort();
}

static inline void lock(pthread_mutex_t *m)
{
    int err = pthread_mutex_lock(m);
    if (err)
        die_err("pthread_mutex_lock", err);
}

static inline void unlock(pthread_mutex_t *m)
{
    int err = pthread_mutex_unlock(m);
    if (err)
        die_err("pthread_mutex_unlock", err);
}

static inline void cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    int err = pthread_cond_wait(c, m);
    if (err)
        die_err("pthread_cond_wait", err);
}

static inline void cond_signal(pthread_cond_t *c)
{
    int err = pthread_cond_signal(c);
    if (err)
        die_err("pthread_cond_signal", err);
}

static inline void cond_broadcast(pthread_cond_t *c)
{
    int err = pthread_cond_broadcast(c);
    if (err)
        die_err("pthread_cond_broadcast", err);
}

#endif
