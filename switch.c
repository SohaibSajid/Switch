#define _POSIX_C_SOURCE 200809L

/*
 * Lock order. Whenever a thread needs more than one of these locks it takes
 * them in this order, never the other way round:
 *
 *   1. ports_lock        the port table: state, down flag, thread ids
 *   2. port->lock        vlan, trunk, mirror_to of one port; when two ports
 *                        are needed, the lower port number first
 *   3. queue lock        one per port, never two at the same time
 *   4. MAC bucket locks  lowest bucket first ("clear mac" takes all of them)
 *   5. leaf locks        MAC free list, packet refcount, stats mutex (shared
 *                        memory), log queue. Nothing else is taken while one
 *                        of these is held.
 *
 * wait_lock and tick_lock are only ever taken alone.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <poll.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "frame.h"
#include "log.h"
#include "mac_table.h"
#include "net.h"
#include "pkt.h"
#include "queue.h"
#include "shm_stats.h"
#include "sync.h"

#define MAX_PORTS 64
#define QUEUE_CAP 64
#define MAX_WAITERS 64
#define TICK_MS 10
#define DEFAULT_VLAN 1

enum { PORT_FREE, PORT_ACTIVE, PORT_DONE };
enum { SEND_OK, SEND_FULL, SEND_CLOSED, SEND_VLAN };

struct port {
    int id;
    int fd;                 /* socket of the host, or the read end of the link */
    int wfd;                /* same as fd, or the write end of the link */
    int link;               /* the pipe link to the other switch (always the trunk) */
    int wake[2];            /* link only: write to wake[1] to stop its threads */
    int state;              /* ports_lock */
    int down;               /* shut down by the admin: written with ports_lock
                               and lock held, read with either */
    pthread_t rx;
    pthread_t tx;
    struct queue q;

    pthread_mutex_t lock;   /* vlan, trunk, mirror_to */
    int vlan;
    int trunk;
    int mirror_to;
};

struct pcfg {
    int vlan;
    int trunk;
    int mirror_to;
};

struct waiter {
    pthread_t th;
    int fd;
    struct sockaddr_in addr;
    int used;               /* accept thread only */
    int done;               /* wait_lock */
};

static struct port *ports;
static int nports;              /* all ports, the link included */
static int nhosts;              /* ports for hosts */
static int link_fd[2] = { -1, -1 };    /* -t: read end, write end */
static pthread_mutex_t ports_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t port_cv = PTHREAD_COND_INITIALIZER;
static struct mac_table *table;
static const char *mac_shm;
static sem_t port_sem;

static struct config cfg;
static const char *cfg_path;
static const char *dump_path = "mac_table.txt";
static const char *log_path = "switch.log";
static int log_echo;
static int log_frames;
static atomic_int max_age;
static atomic_int max_per_port;
static atomic_int wait_secs;

static pthread_mutex_t tick_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tick_cond;
static atomic_int stopping;
static int stop_pipe[2] = { -1, -1 };
static sigset_t sigs;

static pthread_mutex_t wait_lock = PTHREAD_MUTEX_INITIALIZER;
static int nwaiting;
static struct waiter waiters[MAX_WAITERS];

static struct shm_stats *stats;
static const char *sock_path = "/tmp/vswitch.sock";
static const char *stats_name = STATS_NAME;
static int admin_fd = -1;

static struct port *port_by_id(int id)
{
    return id >= 1 && id <= nports ? &ports[id - 1] : NULL;
}

static void post_port(void)
{
    if (sem_post(&port_sem) == -1)
        log_msg("sem_post: %s", strerror(errno));
}

/* ---- the fast path ---- */

static void log_frame(int in, const struct frame_hdr *h,
                      const uint8_t *payload, const char *where)
{
    char src[MAC_STR_LEN];
    char dst[MAC_STR_LEN];
    char text[40];

    mac_format(h->src, src);
    mac_format(h->dst, dst);
    frame_text(payload, h->len, text, sizeof text);

    log_msg("port %d: %s -> %s vlan %u pri %u len %u \"%s\" => %s",
            in, src, dst, h->vlan, h->pri, h->len, text, where);
}

static void port_cfg(struct port *p, struct pcfg *c)
{
    lock(&p->lock);
    c->vlan = p->vlan;
    c->trunk = p->trunk;
    c->mirror_to = p->mirror_to;
    unlock(&p->lock);
}

/* Put a frame on the queue of port p. A normal port only takes frames of
 * its own VLAN, the trunk takes all of them. *sniffer is where p is
 * mirrored to (0 for nowhere). */
static int send_to(struct port *p, struct pkt *k, int *sniffer)
{
    lock(&p->lock);
    if (!p->trunk && p->vlan != k->hdr.vlan) {
        unlock(&p->lock);
        return SEND_VLAN;
    }
    pkt_ref(k);
    int r = queue_push(&p->q, k);
    if (r != QUEUE_OK)
        pkt_unref(k);
    *sniffer = p->mirror_to;
    unlock(&p->lock);

    if (r == QUEUE_FULL) {
        stats_add(stats, p->id, STAT_DROPPED, 1);
        return SEND_FULL;
    }
    return r == QUEUE_OK ? SEND_OK : SEND_CLOSED;
}

/* a copy for the sniffer; if its queue is full we drop the copy, the
 * mirrored port is never slowed down */
static void mirror_copy(int src, struct port *sniffer, struct pkt *k)
{
    pkt_ref(k);
    int r = queue_push(&sniffer->q, k);
    if (r != QUEUE_OK)
        pkt_unref(k);
    if (r == QUEUE_OK)
        stats_add(stats, src, STAT_MIRRORED, 1);
    else if (r == QUEUE_FULL)
        stats_add(stats, sniffer->id, STAT_DROPPED, 1);
}

static void forward(struct port *in, const struct pcfg *c, struct pkt *k,
                    char *where, size_t size)
{
    int bcast = mac_is_broadcast(k->hdr.dst);
    int sniffer[MAC_MAX_PORT + 1] = { 0 };
    uint8_t got[MAC_MAX_PORT + 1] = { 0 };
    int sent = 0;
    int full = 0;
    int handled = 0;

    got[in->id] = 1;

    if (!bcast) {
        int out = mac_table_lookup(table, k->hdr.dst);
        if (out == in->id) {
            snprintf(where, size, "drop (same port)");
            handled = 1;
        } else if (out > 0 && out <= nports) {
            int r = send_to(&ports[out - 1], k, &sniffer[out]);
            if (r == SEND_OK) {
                got[out] = 1;
                snprintf(where, size, "port %d", out);
                handled = 1;
            } else if (r == SEND_FULL) {
                snprintf(where, size, "port %d (dropped, queue full)", out);
                handled = 1;
            }
            /* SEND_VLAN: the host is on another VLAN, SEND_CLOSED: nobody is
             * on that port (a stale entry); either way the destination is
             * unknown for this frame and it is flooded */
        }
    }

    if (!handled) {
        for (int i = 0; i < nports; i++) {
            struct port *p = &ports[i];
            if (p == in)
                continue;
            int r = send_to(p, k, &sniffer[p->id]);
            if (r == SEND_OK) {
                got[p->id] = 1;
                sent++;
            } else if (r == SEND_FULL) {
                full++;
            }
        }
        snprintf(where, size, "flood (%s, %d ports, %d dropped)",
                 bcast ? "broadcast" : "unknown", sent, full);
    }

    /* mirroring: one copy per sniffer, and none for a port that gets the
     * frame anyway or for the sender itself */
    for (int i = 1; i <= nports; i++) {
        int s = sniffer[i];
        if (got[i] && s > 0 && s <= nports && !got[s]) {
            mirror_copy(i, &ports[s - 1], k);
            got[s] = 1;
        }
    }
    int s = c->mirror_to;
    if (s > 0 && s <= nports && !got[s])
        mirror_copy(in->id, &ports[s - 1], k);
}

/* Make the threads of port p stop waiting for the host: shutdown() for a
 * socket; the link is made of pipes, so there we write to the wake pipe. */
static void port_stop(struct port *p)
{
    if (p->link) {
        if (write(p->wake[1], "x", 1) == -1)
            log_msg("link wake pipe: %s", strerror(errno));
    } else if (shutdown(p->fd, SHUT_RDWR) == -1 && errno != ENOTCONN) {
        log_msg("shutdown: %s", strerror(errno));
    }
}

static void *sender(void *arg)
{
    struct port *p = arg;
    struct pkt *k;

    while ((k = queue_pop(&p->q))) {
        int r = write_full_w(p->wfd, p->link ? p->wake[0] : -1, k->data, k->size);
        int saved = errno;
        pkt_unref(k);
        if (r == 0)
            stats_add(stats, p->id, STAT_OUT, 1);
        if (r == -1) {
            if (saved != ECANCELED)
                log_msg("port %d: send failed (%s)", p->id, strerror(saved));
            port_stop(p);
            break;
        }
    }
    return NULL;
}

static void close_port(struct port *p)
{
    unsigned long dropped = queue_close(&p->q);
    int flushed = mac_table_flush_port(table, p->id);

    port_stop(p);
    int err = pthread_join(p->tx, NULL);
    if (err)
        log_msg("pthread_join: %s", strerror(err));

    log_msg("port %d: closed, %lu frames dropped, %d MACs flushed",
            p->id, dropped, flushed);
}

static void *receiver(void *arg)
{
    struct port *p = arg;
    struct frame_hdr h;
    uint8_t payload[MAX_PAYLOAD];
    char where[64];
    struct pcfg c;
    int r;

    while ((r = frame_read_w(p->fd, p->link ? p->wake[0] : -1, &h, payload)) == 1) {
        stats_add(stats, p->id, STAT_IN, 1);
        if (mac_is_broadcast(h.src)) {
            log_msg("port %d: broadcast source MAC, frame dropped", p->id);
            continue;
        }

        /* A host on a normal port does not tag its frames, the port decides
         * the VLAN. Frames on the trunk keep their own tag. */
        port_cfg(p, &c);
        if (c.trunk) {
            if (h.vlan < 1 || h.vlan > 4094) {
                log_msg("port %d: trunk frame with bad vlan %u dropped", p->id, h.vlan);
                continue;
            }
        } else {
            h.vlan = (uint16_t)c.vlan;
        }

        int learned = mac_table_insert(table, h.src, p->id);
        if (learned == MAC_LIMIT) {
            log_msg("port %d: port security, more than %d MACs, shutting port down",
                    p->id, atomic_load(&max_per_port));
            break;
        }
        if (learned == MAC_ERR)
            log_msg("port %d: MAC table insert failed", p->id);

        struct pkt *k = pkt_new(&h, payload);
        if (!k) {
            log_msg("pkt_new: %s", strerror(errno));
            continue;
        }
        forward(p, &c, k, where, sizeof where);
        pkt_unref(k);
        if (log_frames)
            log_frame(p->id, &h, payload, where);
    }

    if (p->link && r == 0)
        log_msg("port %d: the other switch closed the link", p->id);
    else if (r == 0)
        log_msg("port %d: host disconnected", p->id);
    else if (r == -1 && errno == ECANCELED)
        log_msg("port %d: link stopped", p->id);
    else if (r == -1)
        log_msg("port %d: closing (%s)", p->id, strerror(errno));

    close_port(p);
    stats_port_up(stats, p->id, 0);

    lock(&ports_lock);
    if (close(p->fd) == -1)
        log_msg("close: %s", strerror(errno));
    if (p->link && close(p->wfd) == -1)
        log_msg("close: %s", strerror(errno));
    p->fd = -1;
    p->wfd = -1;
    p->state = PORT_DONE;
    /* a port the admin shut down does not go back to the free ports, and
     * the link was never one of them */
    if (!p->down && !p->link)
        post_port();
    cond_broadcast(&port_cv);
    unlock(&ports_lock);
    return NULL;
}

/* ---- helper threads ---- */

/* wait ms milliseconds, or until the switch is stopping: 0 means stop */
static int tick_wait(int ms)
{
    struct timespec until;
    if (clock_gettime(CLOCK_MONOTONIC, &until) == -1)
        die_err("clock_gettime", errno);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }

    lock(&tick_lock);
    int err = 0;
    while (!atomic_load(&stopping) && err != ETIMEDOUT) {
        err = pthread_cond_timedwait(&tick_cond, &tick_lock, &until);
        if (err && err != ETIMEDOUT)
            die_err("pthread_cond_timedwait", err);
    }
    int go = !atomic_load(&stopping);
    unlock(&tick_lock);
    return go;
}

static void *ager(void *arg)
{
    (void)arg;
    while (tick_wait(1000))
        mac_table_age(table, atomic_load(&max_age));
    return NULL;
}

/* the token bucket timer: every tick each port gets the tokens it earned */
static void *refiller(void *arg)
{
    (void)arg;
    struct timespec prev;
    struct timespec cur;

    if (clock_gettime(CLOCK_MONOTONIC, &prev) == -1)
        die_err("clock_gettime", errno);
    while (tick_wait(TICK_MS)) {
        if (clock_gettime(CLOCK_MONOTONIC, &cur) == -1)
            die_err("clock_gettime", errno);
        long ms = (cur.tv_sec - prev.tv_sec) * 1000 +
                  (cur.tv_nsec - prev.tv_nsec) / 1000000;
        if (ms < 1)
            continue;
        prev = cur;
        for (int i = 0; i < nports; i++)
            queue_refill(&ports[i].q, ms);
    }
    return NULL;
}

static void begin_shutdown(void)
{
    lock(&tick_lock);
    atomic_store(&stopping, 1);
    cond_broadcast(&tick_cond);
    unlock(&tick_lock);

    /* wake the hosts that are waiting for a free port */
    lock(&wait_lock);
    int n = nwaiting;
    unlock(&wait_lock);
    for (int i = 0; i < n; i++)
        post_port();

    if (write(stop_pipe[1], "x", 1) == -1)
        log_msg("write stop pipe: %s", strerror(errno));
}

/* ---- live changes: used by swctl and by the config file ---- */

/* lock two ports, the lower number first; either may be NULL */
static void lock_two(struct port *x, struct port *y)
{
    if (x && y && x->id > y->id) {
        struct port *t = x;
        x = y;
        y = t;
    }
    if (x)
        lock(&x->lock);
    if (y && y != x)
        lock(&y->lock);
}

static void unlock_two(struct port *x, struct port *y)
{
    if (x)
        unlock(&x->lock);
    if (y && y != x)
        unlock(&y->lock);
}

/* ports_lock is held, p is FREE or DONE */
static void reap_locked(struct port *p)
{
    if (p->state != PORT_DONE)
        return;
    int err = pthread_join(p->rx, NULL);
    if (err)
        log_msg("pthread_join: %s", strerror(err));
    p->state = PORT_FREE;
}

static void take_token(void)
{
    struct timespec until;
    if (clock_gettime(CLOCK_REALTIME, &until) == -1) {
        log_msg("clock_gettime: %s", strerror(errno));
        return;
    }
    until.tv_sec += 2;
    while (sem_timedwait(&port_sem, &until) == -1) {
        if (errno == EINTR)
            continue;
        log_msg("could not take the port out of the free list (%s)", strerror(errno));
        return;
    }
}

static const char *op_down(int id)
{
    struct port *p = port_by_id(id);
    if (!p)
        return "no such port";
    if (p->link)
        return "this is the link to the other switch";

    lock(&ports_lock);
    if (p->down) {
        unlock(&ports_lock);
        return "port is already shut down";
    }
    lock(&p->lock);
    p->down = 1;
    unlock(&p->lock);

    /* with a host on it the port holds a semaphore token; without one the
     * token is in the semaphore and we have to take it out */
    int busy = p->state == PORT_ACTIVE;
    if (busy) {
        queue_close(&p->q);
        port_stop(p);
        while (p->state == PORT_ACTIVE)
            cond_wait(&port_cv, &ports_lock);
    }
    reap_locked(p);
    unlock(&ports_lock);

    if (!busy)
        take_token();
    log_msg("port %d: shut down by admin", id);
    return NULL;
}

static const char *op_up(int id)
{
    struct port *p = port_by_id(id);
    if (!p)
        return "no such port";
    if (p->link)
        return "this is the link to the other switch";

    lock(&ports_lock);
    if (!p->down) {
        unlock(&ports_lock);
        return "port is already up";
    }
    lock(&p->lock);
    p->down = 0;
    unlock(&p->lock);
    post_port();
    unlock(&ports_lock);

    log_msg("port %d: brought up by admin", id);
    return NULL;
}

static const char *op_vlan(int id, int vlan)
{
    struct port *p = port_by_id(id);
    if (!p)
        return "no such port";
    if (vlan < 1 || vlan > 4094)
        return "vlan must be 1-4094";

    lock(&p->lock);
    if (p->trunk) {
        unlock(&p->lock);
        return "port is the trunk, it carries every VLAN";
    }
    if (p->vlan == vlan) {
        unlock(&p->lock);
        return NULL;
    }
    p->vlan = vlan;
    /* what the port learned and what is queued for it belongs to the old
     * VLAN; doing this under the port lock means no frame of the old VLAN
     * can be queued after we are done */
    int flushed = mac_table_flush_port(table, id);
    int dropped = queue_flush(&p->q);
    unlock(&p->lock);

    log_msg("port %d: vlan %d (%d MACs flushed, %d queued frames dropped)",
            id, vlan, flushed, dropped);
    return NULL;
}

/* id 0 takes the trunk away */
static const char *op_trunk(int id)
{
    struct port *np = NULL;

    if (id != 0 && !(np = port_by_id(id)))
        return "no such port";

    lock(&ports_lock);
    struct port *old = NULL;
    for (int i = 0; i < nports; i++)
        if (ports[i].trunk)
            old = &ports[i];
    if (old == np) {
        unlock(&ports_lock);
        return NULL;
    }
    if (old && old->link) {
        unlock(&ports_lock);
        return "the link to the other switch is the trunk";
    }

    lock_two(old, np);
    int flushed = 0;
    if (old) {
        old->trunk = 0;
        flushed = mac_table_flush_port(table, old->id);
    }
    if (np)
        np->trunk = 1;
    mac_table_set_trunk(table, id);
    unlock_two(old, np);
    unlock(&ports_lock);

    if (np)
        log_msg("port %d: is now the trunk", id);
    else
        log_msg("no trunk port any more");
    if (old && flushed)
        log_msg("port %d: no longer the trunk, %d MACs flushed", old->id, flushed);
    return NULL;
}

/* sniffer 0 stops the mirroring of port id */
static const char *op_mirror(int id, int sniffer)
{
    struct port *p = port_by_id(id);
    struct port *s = NULL;

    if (!p)
        return "no such port";
    if (sniffer != 0 && !(s = port_by_id(sniffer)))
        return "no such sniffer port";
    if (s == p)
        return "a port cannot be mirrored to itself";

    lock(&ports_lock);
    lock_two(p, s);
    const char *err = NULL;
    if (s && s->down)
        err = "sniffer port is shut down";
    else
        p->mirror_to = sniffer;
    unlock_two(p, s);
    unlock(&ports_lock);

    if (err)
        return err;
    if (s)
        log_msg("port %d: mirrored to port %d", id, sniffer);
    else
        log_msg("port %d: mirroring stopped", id);
    return NULL;
}

static const char *op_rate(int id, int rate)
{
    struct port *p = port_by_id(id);
    if (!p)
        return "no such port";
    if (rate < 0)
        return "rate must be 0 (unlimited) or more";

    queue_set_rate(&p->q, rate);
    log_msg("port %d: rate limit %d frames/s", id, rate);
    return NULL;
}

static void config_error(const char *what, int id, const char *err)
{
    if (err)
        log_msg("config: %s for port %d: %s", what, id, err);
}

/* Bring the switch in line with cfg. old is the config before a reload;
 * only what differs from it is applied, so a reload does not undo what the
 * admin did with swctl to things the file did not change. */
static void apply_config(const struct config *old)
{
    atomic_store(&max_age, cfg.age);
    atomic_store(&max_per_port, cfg.per_port);
    atomic_store(&wait_secs, cfg.wait);
    mac_table_set_limits(table, cfg.max_macs, cfg.per_port);

    int qos = !old || old->qos_age != cfg.qos_age ||
              memcmp(old->weight, cfg.weight, sizeof cfg.weight) != 0;

    for (int i = 0; i < nports; i++) {
        struct port *p = &ports[i];
        int id = p->id;

        if (qos)
            queue_set_qos(&p->q, cfg.weight, cfg.qos_age);

        int rate = cfg.port_rate[id] >= 0 ? cfg.port_rate[id] : cfg.rate;
        int was = !old ? -1 : old->port_rate[id] >= 0 ? old->port_rate[id] : old->rate;
        if (old ? rate != was : rate != 0)
            config_error("rate", id, op_rate(id, rate));

        if (cfg.vlan[id] != 0 && (!old || cfg.vlan[id] != old->vlan[id]))
            config_error("vlan", id, op_vlan(id, cfg.vlan[id]));

        if (cfg.mirror[id] != 0 && (!old || cfg.mirror[id] != old->mirror[id]))
            config_error("mirror", id, op_mirror(id, cfg.mirror[id] > 0 ? cfg.mirror[id] : 0));
    }

    if (cfg.trunk >= 0 && (!old || cfg.trunk != old->trunk))
        config_error("trunk", cfg.trunk, op_trunk(cfg.trunk));

    for (int id = nports + 1; id <= CFG_MAX_PORT; id++)
        if (cfg.vlan[id] || cfg.mirror[id] || cfg.port_rate[id] >= 0)
            if (!old || cfg.vlan[id] != old->vlan[id] ||
                cfg.mirror[id] != old->mirror[id] ||
                cfg.port_rate[id] != old->port_rate[id])
                log_msg("config: port %d does not exist, setting ignored", id);
}

/* ---- signals ---- */

static void reload_config(void)
{
    if (!cfg_path) {
        log_msg("SIGHUP: no config file given (-c), nothing to reload");
        return;
    }
    struct config next = cfg;
    if (config_load(cfg_path, &next) == -1) {
        log_msg("SIGHUP: config error in %s, keeping old settings", cfg_path);
        return;
    }
    struct config old = cfg;
    cfg = next;
    apply_config(&old);
    log_msg("SIGHUP: config reloaded from %s (age %d, max_macs %d, "
            "macs_per_port %d, wait %d, rate %d)",
            cfg_path, cfg.age, cfg.max_macs, cfg.per_port, cfg.wait, cfg.rate);
}

static void dump_macs(void)
{
    FILE *f = fopen(dump_path, "w");
    if (!f) {
        log_msg("%s: %s", dump_path, strerror(errno));
        return;
    }
    int n = mac_table_dump(table, f);
    if (fclose(f) == EOF) {
        log_msg("%s: %s", dump_path, strerror(errno));
        return;
    }
    log_msg("SIGUSR1: %d MAC entries written to %s", n, dump_path);
}

static void *signal_loop(void *arg)
{
    (void)arg;

    for (;;) {
        int sig;
        int err = sigwait(&sigs, &sig);
        if (err)
            die_err("sigwait", err);

        if (sig == SIGINT || sig == SIGTERM) {
            log_msg("%s: shutting down", sig == SIGINT ? "SIGINT" : "SIGTERM");
            begin_shutdown();
            return NULL;
        }
        if (sig == SIGHUP)
            reload_config();
        else if (sig == SIGUSR1)
            dump_macs();
    }
}

/* ---- the admin socket ---- */

static void show_ports(FILE *f)
{
    struct shm_stats snap;
    stats_snapshot(stats, &snap);

    fprintf(f, "%-4s  %-10s  %-5s  %-6s  %-6s  %4s  %5s  %9s  %9s  %9s  %9s\n",
            "PORT", "STATE", "VLAN", "MIRROR", "RATE", "MACS", "QUEUE",
            "IN", "OUT", "DROPPED", "MIRRORED");
    for (int i = 0; i < nports; i++) {
        struct port *p = &ports[i];
        int id = p->id;
        char vlan[8];
        char mirror[8] = "-";
        char rate[12] = "-";

        lock(&ports_lock);
        const char *state = p->down ? "admin-down" :
                            p->state == PORT_ACTIVE ? "up" : "down";
        struct pcfg c;
        port_cfg(p, &c);
        unlock(&ports_lock);

        if (c.trunk)
            snprintf(vlan, sizeof vlan, "trunk");
        else
            snprintf(vlan, sizeof vlan, "%d", c.vlan);
        if (c.mirror_to)
            snprintf(mirror, sizeof mirror, "->%d", c.mirror_to);
        int r = queue_rate(&p->q);
        if (r)
            snprintf(rate, sizeof rate, "%d/s", r);

        const uint64_t *n = snap.port[id].count;
        fprintf(f, "%-4d  %-10s  %-5s  %-6s  %-6s  %4d  %5d  %9llu  %9llu  %9llu  %9llu\n",
                id, state, vlan, mirror, rate, mac_table_port_count(table, id),
                queue_len(&p->q), (unsigned long long)n[STAT_IN],
                (unsigned long long)n[STAT_OUT], (unsigned long long)n[STAT_DROPPED],
                (unsigned long long)n[STAT_MIRRORED]);
    }
}

static int split(char *line, char **w, int max)
{
    int n = 0;
    char *save = NULL;

    for (char *t = strtok_r(line, " \t", &save); t; t = strtok_r(NULL, " \t", &save)) {
        if (n == max)
            return max + 1;
        w[n++] = t;
    }
    return n;
}

static int number_arg(const char *s, int min, int max, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *s == '\0' || *end != '\0' || v < min || v > max)
        return -1;
    *out = (int)v;
    return 0;
}

static void run_command(FILE *f, char *line)
{
    char *w[4];
    int n = split(line, w, 4);
    int a = 0;
    int b = 0;
    const char *err = NULL;

    if (n == 2 && strcmp(w[0], "show") == 0 && strcmp(w[1], "mac") == 0) {
        fprintf(f, "%d entries\n", mac_table_dump(table, f));
    } else if (n == 2 && strcmp(w[0], "show") == 0 && strcmp(w[1], "ports") == 0) {
        show_ports(f);
    } else if (n == 2 && strcmp(w[0], "clear") == 0 && strcmp(w[1], "mac") == 0) {
        fprintf(f, "cleared %d MAC entries\n", mac_table_clear(table));
    } else if (n == 2 && strcmp(w[0], "shutdown") == 0 && number_arg(w[1], 1, CFG_MAX_PORT, &a) == 0) {
        if ((err = op_down(a)))
            fprintf(f, "error: port %d: %s\n", a, err);
        else
            fprintf(f, "port %d is shut down\n", a);
    } else if (n == 2 && strcmp(w[0], "up") == 0 && number_arg(w[1], 1, CFG_MAX_PORT, &a) == 0) {
        if ((err = op_up(a)))
            fprintf(f, "error: port %d: %s\n", a, err);
        else
            fprintf(f, "port %d is up\n", a);
    } else if (n == 3 && strcmp(w[0], "vlan") == 0 && number_arg(w[1], 1, CFG_MAX_PORT, &a) == 0 &&
               (strcmp(w[2], "trunk") == 0 || number_arg(w[2], 1, 4094, &b) == 0)) {
        int trunk = strcmp(w[2], "trunk") == 0;
        err = trunk ? op_trunk(a) : op_vlan(a, b);
        if (err)
            fprintf(f, "error: port %d: %s\n", a, err);
        else if (trunk)
            fprintf(f, "port %d is the trunk\n", a);
        else
            fprintf(f, "port %d is in vlan %d\n", a, b);
    } else if (n == 2 && strcmp(w[0], "trunk") == 0 &&
               (strcmp(w[1], "off") == 0 || number_arg(w[1], 1, CFG_MAX_PORT, &a) == 0)) {
        int off = strcmp(w[1], "off") == 0;
        if ((err = op_trunk(off ? 0 : a)))
            fprintf(f, "error: %s\n", err);
        else if (off)
            fprintf(f, "no trunk port\n");
        else
            fprintf(f, "port %d is the trunk\n", a);
    } else if (n == 3 && strcmp(w[0], "mirror") == 0 && number_arg(w[1], 1, CFG_MAX_PORT, &a) == 0 &&
               (strcmp(w[2], "off") == 0 || number_arg(w[2], 1, CFG_MAX_PORT, &b) == 0)) {
        int off = strcmp(w[2], "off") == 0;
        if ((err = op_mirror(a, off ? 0 : b)))
            fprintf(f, "error: port %d: %s\n", a, err);
        else if (off)
            fprintf(f, "port %d is not mirrored\n", a);
        else
            fprintf(f, "port %d is mirrored to port %d\n", a, b);
    } else if (n == 3 && strcmp(w[0], "rate") == 0 && number_arg(w[1], 1, CFG_MAX_PORT, &a) == 0 &&
               number_arg(w[2], 0, 1000000, &b) == 0) {
        if ((err = op_rate(a, b)))
            fprintf(f, "error: port %d: %s\n", a, err);
        else if (b)
            fprintf(f, "port %d is limited to %d frames/s\n", a, b);
        else
            fprintf(f, "port %d has no rate limit\n", a);
    } else {
        fprintf(f, "error: unknown command or wrong arguments\n"
                   "commands: show mac | show ports | clear mac | shutdown <port> | up <port>\n"
                   "          vlan <port> <1-4094|trunk> | trunk <port|off> | mirror <port> <sniffer|off>\n"
                   "          rate <port> <frames/s, 0 = unlimited>\n");
    }
}

static void admin_reply(int fd, char *cmd)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f) {
        log_msg("open_memstream: %s", strerror(errno));
        return;
    }

    run_command(f, cmd);

    if (fclose(f) == EOF) {
        log_msg("admin reply: %s", strerror(errno));
        free(buf);
        return;
    }
    if (write_full(fd, buf, len) == -1)
        log_msg("admin reply: %s", strerror(errno));
    free(buf);
}

static int read_cmd(int fd, char *buf, size_t size)
{
    size_t got = 0;
    while (got + 1 < size) {
        ssize_t r = read(fd, buf + got, size - 1 - got);
        if (r == 0)
            break;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got += (size_t)r;
        if (memchr(buf, '\n', got))
            break;
    }
    buf[got] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';
    return (int)got;
}

static void *admin_loop(void *arg)
{
    (void)arg;
    struct pollfd pfd[2];
    pfd[0].fd = admin_fd;
    pfd[0].events = POLLIN;
    pfd[1].fd = stop_pipe[0];
    pfd[1].events = POLLIN;

    while (!atomic_load(&stopping)) {
        if (poll(pfd, 2, -1) == -1) {
            if (errno == EINTR)
                continue;
            log_msg("admin poll: %s", strerror(errno));
            break;
        }
        if (pfd[1].revents)
            break;
        if (!(pfd[0].revents & POLLIN))
            continue;

        int c = accept(admin_fd, NULL, NULL);
        if (c == -1) {
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK &&
                errno != ECONNABORTED)
                log_msg("admin accept: %s", strerror(errno));
            continue;
        }

        struct timeval tv = { 2, 0 };
        if (setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == -1)
            log_msg("setsockopt: %s", strerror(errno));

        char cmd[128];
        if (read_cmd(c, cmd, sizeof cmd) > 0) {
            log_msg("admin: %s", cmd);
            admin_reply(c, cmd);
        }
        if (close(c) == -1)
            log_msg("close: %s", strerror(errno));
    }
    return NULL;
}

static int open_admin(void)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof addr.sun_path) {
        fprintf(stderr, "socket path too long: %s\n", sock_path);
        return -1;
    }
    strcpy(addr.sun_path, sock_path);

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == -1) {
        perror("socket");
        return -1;
    }
    if (unlink(sock_path) == -1 && errno != ENOENT)
        perror(sock_path);
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) == -1) {
        perror(sock_path);
        close(s);
        return -1;
    }
    int flags = fcntl(s, F_GETFL);
    if (listen(s, 8) == -1 || flags == -1 ||
        fcntl(s, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("admin socket");
        close(s);
        unlink(sock_path);
        return -1;
    }
    return s;
}

/* ---- hosts coming and going ---- */

/* Give the host on fd a port. The caller holds one token of port_sem.
 * Returns NULL if there is no usable port (the token is not used then). */
static struct port *start_port(int fd)
{
    struct port *p = NULL;

    lock(&ports_lock);
    for (int i = 0; i < nhosts; i++) {
        reap_locked(&ports[i]);
        if (!p && ports[i].state == PORT_FREE && !ports[i].down)
            p = &ports[i];
    }
    if (!p) {
        unlock(&ports_lock);
        return NULL;
    }

    p->state = PORT_ACTIVE;
    p->fd = fd;
    p->wfd = fd;
    queue_open(&p->q);
    stats_port_up(stats, p->id, 1);

    int err = pthread_create(&p->tx, NULL, sender, p);
    if (err) {
        log_msg("pthread_create: %s", strerror(err));
        queue_close(&p->q);
        goto undo;
    }
    err = pthread_create(&p->rx, NULL, receiver, p);
    if (err) {
        log_msg("pthread_create: %s", strerror(err));
        close_port(p);
        goto undo;
    }
    unlock(&ports_lock);
    return p;

undo:
    stats_port_up(stats, p->id, 0);
    p->fd = -1;
    p->state = PORT_FREE;
    unlock(&ports_lock);
    return NULL;
}

/* The pipe link to the other switch: always port nhosts + 1, always the
 * trunk, started with the switch and never one of the free ports. */
static int start_link(void)
{
    struct port *p = &ports[nhosts];

    int flags = fcntl(link_fd[1], F_GETFL);
    if (flags == -1 || fcntl(link_fd[1], F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl on link");
        return -1;
    }

    lock(&ports_lock);
    p->fd = link_fd[0];
    p->wfd = link_fd[1];
    p->state = PORT_ACTIVE;
    lock(&p->lock);
    p->trunk = 1;
    unlock(&p->lock);
    mac_table_set_trunk(table, p->id);
    queue_open(&p->q);
    stats_port_up(stats, p->id, 1);

    int err = pthread_create(&p->tx, NULL, sender, p);
    if (err) {
        fprintf(stderr, "pthread_create: %s\n", strerror(err));
        queue_close(&p->q);
        p->state = PORT_FREE;
        unlock(&ports_lock);
        return -1;
    }
    err = pthread_create(&p->rx, NULL, receiver, p);
    if (err) {
        fprintf(stderr, "pthread_create: %s\n", strerror(err));
        close_port(p);
        p->state = PORT_FREE;
        unlock(&ports_lock);
        return -1;
    }
    unlock(&ports_lock);
    log_msg("port %d: link to the other switch is up (trunk)", p->id);
    return 0;
}

static void reject(int fd)
{
    const char *msg = "switch full";
    struct frame_hdr h;

    memset(&h, 0, sizeof h);
    memset(h.dst, 0xFF, MAC_LEN);
    h.len = (uint16_t)strlen(msg);
    h.vlan = 1;
    h.pri = PRI_HIGH;

    log_msg("switch full, rejecting connection");
    if (frame_write(fd, &h, (const uint8_t *)msg) == -1)
        log_msg("switch full reply: %s", strerror(errno));
    if (close(fd) == -1)
        log_msg("close: %s", strerror(errno));
}

/* the caller holds a token */
static void admit(int fd, const struct sockaddr_in *addr)
{
    struct port *p = start_port(fd);
    if (!p) {
        post_port();
        reject(fd);
        return;
    }

    char ip[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof ip))
        snprintf(ip, sizeof ip, "?");
    log_msg("port %d: host connected from %s:%u", p->id, ip, ntohs(addr->sin_port));
}

/* wait for a free port, at most wait_secs seconds */
static int wait_port(void)
{
    struct timespec until;

    lock(&wait_lock);
    if (atomic_load(&stopping)) {
        unlock(&wait_lock);
        return -1;
    }
    nwaiting++;
    unlock(&wait_lock);

    int r = 0;
    if (clock_gettime(CLOCK_REALTIME, &until) == -1) {
        log_msg("clock_gettime: %s", strerror(errno));
        r = -1;
    } else {
        until.tv_sec += atomic_load(&wait_secs);
        while (sem_timedwait(&port_sem, &until) == -1) {
            if (errno == EINTR)
                continue;
            if (errno != ETIMEDOUT)
                log_msg("sem_timedwait: %s", strerror(errno));
            r = -1;
            break;
        }
    }

    lock(&wait_lock);
    nwaiting--;
    unlock(&wait_lock);
    return r;
}

/* One thread per host that has to wait, so the hosts that wait do not hold
 * up each other (or the accept thread). */
static void *waiter(void *arg)
{
    struct waiter *w = arg;

    int got = wait_port();
    if (atomic_load(&stopping)) {
        /* going down anyway: just hang up */
        if (close(w->fd) == -1)
            log_msg("close: %s", strerror(errno));
    } else if (got == 0) {
        admit(w->fd, &w->addr);
    } else {
        reject(w->fd);
    }

    lock(&wait_lock);
    w->done = 1;
    unlock(&wait_lock);
    return NULL;
}

static void join_waiters(int all)
{
    for (int i = 0; i < MAX_WAITERS; i++) {
        if (!waiters[i].used)
            continue;
        lock(&wait_lock);
        int done = waiters[i].done;
        unlock(&wait_lock);
        if (!done && !all)
            continue;

        int err = pthread_join(waiters[i].th, NULL);
        if (err)
            log_msg("pthread_join waiter: %s", strerror(err));
        waiters[i].used = 0;
    }
}

static void spawn_waiter(int fd, const struct sockaddr_in *addr)
{
    join_waiters(0);

    struct waiter *w = NULL;
    for (int i = 0; i < MAX_WAITERS && !w; i++)
        if (!waiters[i].used)
            w = &waiters[i];
    if (!w) {
        reject(fd);
        return;
    }

    w->fd = fd;
    w->addr = *addr;
    w->done = 0;
    int err = pthread_create(&w->th, NULL, waiter, w);
    if (err) {
        log_msg("pthread_create: %s", strerror(err));
        reject(fd);
        return;
    }
    w->used = 1;
}

static int open_listener(int tcp_port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == -1) {
        perror("socket");
        return -1;
    }

    int yes = 1;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) == -1) {
        perror("setsockopt");
        close(s);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)tcp_port);

    if (bind(s, (struct sockaddr *)&addr, sizeof addr) == -1) {
        perror("bind");
        close(s);
        return -1;
    }
    if (listen(s, 128) == -1) {
        perror("listen");
        close(s);
        return -1;
    }
    int flags = fcntl(s, F_GETFL);
    if (flags == -1 || fcntl(s, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl");
        close(s);
        return -1;
    }
    return s;
}

static void accept_loop(int listener)
{
    struct pollfd pfd[2];
    pfd[0].fd = listener;
    pfd[0].events = POLLIN;
    pfd[1].fd = stop_pipe[0];
    pfd[1].events = POLLIN;

    while (!atomic_load(&stopping)) {
        if (poll(pfd, 2, -1) == -1) {
            if (errno == EINTR)
                continue;
            log_msg("poll: %s", strerror(errno));
            return;
        }
        if (pfd[1].revents)
            return;
        if (!(pfd[0].revents & POLLIN))
            continue;

        struct sockaddr_in addr;
        socklen_t alen = sizeof addr;

        int fd = accept(listener, (struct sockaddr *)&addr, &alen);
        if (fd == -1) {
            if (errno != EINTR && errno != ECONNABORTED &&
                errno != EAGAIN && errno != EWOULDBLOCK)
                log_msg("accept: %s", strerror(errno));
            continue;
        }

        if (sem_trywait(&port_sem) == 0) {
            admit(fd, &addr);
        } else if (errno != EAGAIN && errno != EINTR) {
            log_msg("sem_trywait: %s", strerror(errno));
            reject(fd);
        } else if (atomic_load(&wait_secs) == 0) {
            reject(fd);
        } else {
            spawn_waiter(fd, &addr);
        }
    }
}

static void stop_ports(void)
{
    lock(&ports_lock);
    for (int i = 0; i < nports; i++)
        if (ports[i].state == PORT_ACTIVE)
            port_stop(&ports[i]);

    for (int i = 0; i < nports; i++) {
        while (ports[i].state == PORT_ACTIVE)
            cond_wait(&port_cv, &ports_lock);
        reap_locked(&ports[i]);
    }
    unlock(&ports_lock);
}

static void free_ports(int n)
{
    for (int i = 0; i < n; i++) {
        queue_destroy(&ports[i].q);
        pthread_mutex_destroy(&ports[i].lock);
        if (ports[i].link) {
            close(ports[i].wake[0]);
            close(ports[i].wake[1]);
        }
    }
    free(ports);
    ports = NULL;
}

static void join_thread(pthread_t t, const char *name)
{
    int err = pthread_join(t, NULL);
    if (err)
        log_msg("pthread_join %s: %s", name, strerror(err));
}

static int parse_int(const char *s, int min, int max, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *s == '\0' || *end != '\0' || v < min || v > max)
        return -1;
    *out = (int)v;
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [options] <tcp_port> <num_ports 1-%d>\n"
            "  -c file   config file (reloaded on SIGHUP)\n"
            "  -l file   log file (default switch.log)\n"
            "  -v        also print the log on stdout\n"
            "  -f        log every frame that is forwarded\n"
            "  -d file   where SIGUSR1 dumps the MAC table (default mac_table.txt)\n"
            "  -s path   admin UNIX socket (default /tmp/vswitch.sock)\n"
            "  -S name   shared memory object for the counters (default /vswitch_stats)\n"
            "  -M name   keep the MAC table in this shared memory object\n"
            "  -t R,W    pipe link to another switch: read from fd R, write to fd W\n"
            "            (one more port, always the trunk; see switch_pair)\n"
            "  -a secs   MAC aging time (30)         -k n  MAC table size (1024)\n"
            "  -m n      MACs per port (16)          -w secs  wait for a free port (5)\n"
            "  -r n      rate limit, frames/s per port (0 = off)\n"
            "0 for -k or -m means no limit\n",
            prog, MAX_PORTS);
}

int main(int argc, char **argv)
{
    int tcp_port;
    int opt;
    int bad = 0;
    struct config cli;

    config_defaults(&cfg);
    cli.age = cli.max_macs = cli.per_port = cli.wait = cli.rate = -1;

    while ((opt = getopt(argc, argv, "c:l:vfd:a:k:m:w:r:s:S:M:t:")) != -1) {
        if (opt == 'c')
            cfg_path = optarg;
        else if (opt == 'l')
            log_path = optarg;
        else if (opt == 'v')
            log_echo = 1;
        else if (opt == 'f')
            log_frames = 1;
        else if (opt == 'd')
            dump_path = optarg;
        else if (opt == 's')
            sock_path = optarg;
        else if (opt == 'S')
            stats_name = optarg;
        else if (opt == 'M')
            mac_shm = optarg;
        else if (opt == 't') {
            char extra;
            if (sscanf(optarg, "%d,%d%c", &link_fd[0], &link_fd[1], &extra) != 2 ||
                link_fd[0] < 3 || link_fd[1] < 3 || link_fd[0] == link_fd[1] ||
                fcntl(link_fd[0], F_GETFD) == -1 || fcntl(link_fd[1], F_GETFD) == -1) {
                fprintf(stderr, "-t needs two open file descriptors, like -t 5,8\n");
                bad = 1;
            }
        }
        else if (opt == 'a')
            bad |= parse_int(optarg, 1, 86400, &cli.age);
        else if (opt == 'k')
            bad |= parse_int(optarg, 0, 32768, &cli.max_macs);
        else if (opt == 'm')
            bad |= parse_int(optarg, 0, 32768, &cli.per_port);
        else if (opt == 'w')
            bad |= parse_int(optarg, 0, 3600, &cli.wait);
        else if (opt == 'r')
            bad |= parse_int(optarg, 0, 1000000, &cli.rate);
        else
            bad = 1;
    }

    if (bad || argc - optind != 2 ||
        parse_int(argv[optind], 1, 65535, &tcp_port) == -1 ||
        parse_int(argv[optind + 1], 1, MAX_PORTS, &nhosts) == -1) {
        usage(argv[0]);
        return 1;
    }

    nports = nhosts + (link_fd[0] >= 0 ? 1 : 0);

    if (cfg_path && config_load(cfg_path, &cfg) == -1)
        return 1;
    if (cli.age >= 0)
        cfg.age = cli.age;
    if (cli.max_macs >= 0)
        cfg.max_macs = cli.max_macs;
    if (cli.per_port >= 0)
        cfg.per_port = cli.per_port;
    if (cli.wait >= 0)
        cfg.wait = cli.wait;
    if (cli.rate >= 0)
        cfg.rate = cli.rate;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    if (sigemptyset(&sa.sa_mask) == -1 || sigaction(SIGPIPE, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    if (sigemptyset(&sigs) == -1 || sigaddset(&sigs, SIGINT) == -1 ||
        sigaddset(&sigs, SIGTERM) == -1 || sigaddset(&sigs, SIGHUP) == -1 ||
        sigaddset(&sigs, SIGUSR1) == -1) {
        perror("sigset");
        return 1;
    }
    int err = pthread_sigmask(SIG_BLOCK, &sigs, NULL);
    if (err) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(err));
        return 1;
    }

    if (log_start(log_path, log_echo) == -1)
        return 1;

    int listener = open_listener(tcp_port);
    if (listener == -1) {
        log_stop();
        return 1;
    }
    if (pipe(stop_pipe) == -1) {
        perror("pipe");
        close(listener);
        log_stop();
        return 1;
    }

    if (mac_shm) {
        int fixed = 0;
        table = mac_table_shm_attach(mac_shm, &fixed);
        if (table) {
            log_msg("MAC table taken over from %s (%zu entries, %d damaged ones dropped)",
                    mac_shm, mac_table_count(table), fixed);
        } else {
            table = mac_table_shm_create(mac_shm, cfg.max_macs, cfg.per_port);
        }
        if (!table) {
            log_stop();
            return 1;
        }
        mac_table_set_trunk(table, 0);
    } else {
        table = calloc(1, sizeof *table);
        if (!table) {
            perror("calloc");
            log_stop();
            return 1;
        }
        err = mac_table_init(table, cfg.max_macs, cfg.per_port);
        if (err) {
            fprintf(stderr, "mac_table_init: %s\n", strerror(err));
            log_stop();
            return 1;
        }
    }

    ports = calloc((size_t)nports, sizeof *ports);
    if (!ports) {
        perror("calloc");
        log_stop();
        return 1;
    }
    for (int i = 0; i < nports; i++) {
        ports[i].id = i + 1;
        ports[i].fd = -1;
        ports[i].wfd = -1;
        ports[i].state = PORT_FREE;
        ports[i].vlan = DEFAULT_VLAN;
        ports[i].link = i >= nhosts;
        err = pthread_mutex_init(&ports[i].lock, NULL);
        if (!err)
            err = queue_init(&ports[i].q, QUEUE_CAP);
        if (!err && ports[i].link && pipe(ports[i].wake) == -1)
            err = errno;
        if (err) {
            fprintf(stderr, "port init: %s\n", strerror(err));
            free_ports(i);
            log_stop();
            return 1;
        }
    }

    if (sem_init(&port_sem, 0, (unsigned int)nhosts) == -1) {
        perror("sem_init");
        return 1;
    }

    stats = stats_create(stats_name, nports);
    if (!stats)
        return 1;
    admin_fd = open_admin();
    if (admin_fd == -1) {
        stats_destroy(stats, stats_name);
        return 1;
    }

    apply_config(NULL);

    pthread_condattr_t attr;
    pthread_t age_thread;
    pthread_t refill_thread;
    pthread_t sig_thread;
    pthread_t admin_thread;
    if ((err = pthread_condattr_init(&attr)) ||
        (err = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) ||
        (err = pthread_cond_init(&tick_cond, &attr)) ||
        (err = pthread_condattr_destroy(&attr)) ||
        (err = pthread_create(&age_thread, NULL, ager, NULL)) ||
        (err = pthread_create(&refill_thread, NULL, refiller, NULL)) ||
        (err = pthread_create(&admin_thread, NULL, admin_loop, NULL)) ||
        (err = pthread_create(&sig_thread, NULL, signal_loop, NULL))) {
        fprintf(stderr, "thread setup: %s\n", strerror(err));
        return 1;
    }

    if (link_fd[0] >= 0 && start_link() == -1)
        begin_shutdown();

    log_msg("switch listening on TCP %d with %d ports%s "
            "(age %ds, max %d MACs, %d per port, wait %ds, rate %d)",
            tcp_port, nhosts, nports > nhosts ? " and a link" : "",
            cfg.age, cfg.max_macs, cfg.per_port, cfg.wait, cfg.rate);

    accept_loop(listener);

    if (!atomic_load(&stopping)) {
        err = pthread_kill(sig_thread, SIGTERM);
        if (err)
            log_msg("pthread_kill: %s", strerror(err));
    }

    if (close(listener) == -1)
        log_msg("close: %s", strerror(errno));
    join_waiters(1);
    stop_ports();
    join_thread(sig_thread, "signal");
    join_thread(age_thread, "aging");
    join_thread(refill_thread, "token timer");
    join_thread(admin_thread, "admin");

    if (close(admin_fd) == -1)
        log_msg("close: %s", strerror(errno));
    if (unlink(sock_path) == -1)
        log_msg("%s: %s", sock_path, strerror(errno));
    stats_destroy(stats, stats_name);

    if (mac_shm) {
        mac_table_shm_close(table);
    } else {
        mac_table_destroy(table);
        free(table);
    }
    free_ports(nports);
    if (sem_destroy(&port_sem) == -1)
        log_msg("sem_destroy: %s", strerror(errno));
    pthread_cond_destroy(&tick_cond);
    close(stop_pipe[0]);
    close(stop_pipe[1]);

    log_msg("switch stopped cleanly");
    log_stop();
    return 0;
}
