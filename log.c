#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sync.h"

#define LOG_QUEUE 8192
#define LOG_LINE 192

struct entry {
    struct timespec ts;
    char text[LOG_LINE];
};

static struct {
    pthread_mutex_t lock;
    pthread_cond_t more;
    struct entry *ring;
    int head;
    int count;
    int running;
    int closing;
    int echo;
    unsigned long dropped;
    FILE *file;
    pthread_t thread;
} L = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER,
        NULL, 0, 0, 0, 0, 0, 0, NULL, 0 };

static void write_entry(const struct entry *e)
{
    struct tm tm;
    char when[32];

    if (!localtime_r(&e->ts.tv_sec, &tm) ||
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm) == 0)
        snprintf(when, sizeof when, "?");

    if (fprintf(L.file, "%s.%03ld %s\n", when, e->ts.tv_nsec / 1000000, e->text) < 0)
        perror("log write");
    if (L.echo)
        printf("%s\n", e->text);
}

static void *logger(void *arg)
{
    (void)arg;
    struct entry e;

    lock(&L.lock);
    for (;;) {
        while (L.count == 0 && !L.closing)
            cond_wait(&L.more, &L.lock);
        if (L.count == 0)
            break;

        e = L.ring[L.head];
        L.head = (L.head + 1) % LOG_QUEUE;
        L.count--;
        unsigned long lost = L.dropped;
        L.dropped = 0;
        int idle = L.count == 0;
        unlock(&L.lock);

        if (lost) {
            struct entry w;
            memset(&w, 0, sizeof w);
            w.ts = e.ts;
            snprintf(w.text, sizeof w.text,
                     "log queue was full, %lu messages lost", lost);
            write_entry(&w);
        }
        write_entry(&e);
        if (idle) {
            if (fflush(L.file) == EOF)
                perror("log flush");
            if (L.echo && fflush(stdout) == EOF)
                perror("stdout");
        }
        lock(&L.lock);
    }
    unlock(&L.lock);
    return NULL;
}

int log_start(const char *path, int echo)
{
    FILE *f = fopen(path, "a");
    if (!f) {
        perror(path);
        return -1;
    }
    struct entry *ring = calloc(LOG_QUEUE, sizeof *ring);
    if (!ring) {
        perror("calloc");
        fclose(f);
        return -1;
    }

    lock(&L.lock);
    L.ring = ring;
    L.head = 0;
    L.count = 0;
    L.closing = 0;
    L.dropped = 0;
    L.echo = echo;
    L.file = f;
    unlock(&L.lock);

    int err = pthread_create(&L.thread, NULL, logger, NULL);
    if (err) {
        fprintf(stderr, "pthread_create: %s\n", strerror(err));
        free(ring);
        fclose(f);
        return -1;
    }
    lock(&L.lock);
    L.running = 1;
    unlock(&L.lock);
    return 0;
}

void log_stop(void)
{
    lock(&L.lock);
    if (!L.running) {
        unlock(&L.lock);
        return;
    }
    L.closing = 1;
    cond_broadcast(&L.more);
    unlock(&L.lock);

    int err = pthread_join(L.thread, NULL);
    if (err)
        fprintf(stderr, "pthread_join logger: %s\n", strerror(err));

    lock(&L.lock);
    L.running = 0;
    unlock(&L.lock);
    if (fclose(L.file) == EOF)
        perror("log close");
    free(L.ring);
    L.ring = NULL;
    L.file = NULL;
}

void log_msg(const char *fmt, ...)
{
    struct entry e;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(e.text, sizeof e.text, fmt, ap);
    va_end(ap);
    if (clock_gettime(CLOCK_REALTIME, &e.ts) == -1) {
        e.ts.tv_sec = 0;
        e.ts.tv_nsec = 0;
    }

    lock(&L.lock);
    if (!L.running || L.closing) {
        unlock(&L.lock);
        fprintf(stderr, "%s\n", e.text);
        return;
    }
    if (L.count == LOG_QUEUE) {
        L.dropped++;
        unlock(&L.lock);
        return;
    }
    L.ring[(L.head + L.count) % LOG_QUEUE] = e;
    L.count++;
    cond_signal(&L.more);
    unlock(&L.lock);
}
