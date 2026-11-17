#define _POSIX_C_SOURCE 200809L

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
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "frame.h"
#include "mac_table.h"
#include "net.h"
#include "pkt.h"
#include "queue.h"
#include "sync.h"

#define MAX_PORTS 64
#define QUEUE_CAP 64

enum { PORT_FREE, PORT_ACTIVE, PORT_DONE };

struct port {
    int id;
    int fd;
    int state;
    pthread_t rx;
    pthread_t tx;
    struct queue q;
};

static struct port *ports;
static int nports;
static pthread_mutex_t ports_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mac_table table;
static sem_t port_sem;

static struct config cfg = { 30, 1024, 16, 5 };
static const char *cfg_path;
static const char *dump_path = "mac_table.txt";
static atomic_int max_age;
static atomic_int max_per_port;
static atomic_int wait_secs;

static pthread_mutex_t age_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t age_cond;
static atomic_int stopping;
static int stop_pipe[2] = { -1, -1 };
static sigset_t sigs;

static void apply_config(void)
{
    atomic_store(&max_age, cfg.age);
    atomic_store(&max_per_port, cfg.per_port);
    atomic_store(&wait_secs, cfg.wait);
    mac_table_set_limits(&table, cfg.max_macs, cfg.per_port);
}

static void post_port(void)
{
    if (sem_post(&port_sem) == -1)
        perror("sem_post");
}

static void log_frame(int in, const struct frame_hdr *h,
                      const uint8_t *payload, const char *where)
{
    char src[MAC_STR_LEN];
    char dst[MAC_STR_LEN];
    char text[64];

    mac_format(h->src, src);
    mac_format(h->dst, dst);
    frame_text(payload, h->len, text, sizeof text);

    printf("port %d: %s -> %s vlan %u pri %u len %u \"%s\" => %s\n",
           in, src, dst, h->vlan, h->pri, h->len, text, where);
}

static int send_to(struct port *p, struct pkt *k)
{
    pkt_ref(k);
    int r = queue_push(&p->q, k);
    if (r != QUEUE_OK)
        pkt_unref(k);
    return r;
}

static void forward(struct port *in, struct pkt *k, char *where, size_t size)
{
    int bcast = mac_is_broadcast(k->hdr.dst);

    if (!bcast) {
        int out = mac_table_lookup(&table, k->hdr.dst);
        if (out == in->id) {
            snprintf(where, size, "drop (same port)");
            return;
        }
        if (out > 0) {
            int r = send_to(&ports[out - 1], k);
            if (r == QUEUE_OK)
                snprintf(where, size, "port %d", out);
            else if (r == QUEUE_FULL)
                snprintf(where, size, "port %d (dropped, queue full)", out);
            else
                snprintf(where, size, "port %d (dropped, port closed)", out);
            return;
        }
    }

    int sent = 0;
    int full = 0;
    for (int i = 0; i < nports; i++) {
        if (&ports[i] == in)
            continue;
        int r = send_to(&ports[i], k);
        if (r == QUEUE_OK)
            sent++;
        else if (r == QUEUE_FULL)
            full++;
    }
    snprintf(where, size, "flood (%s, %d ports, %d dropped)",
             bcast ? "broadcast" : "unknown", sent, full);
}

static void *sender(void *arg)
{
    struct port *p = arg;
    struct pkt *k;

    while ((k = queue_pop(&p->q))) {
        int r = write_full(p->fd, k->data, k->size);
        int saved = errno;
        pkt_unref(k);
        if (r == -1) {
            printf("port %d: send failed (%s)\n", p->id, strerror(saved));
            if (shutdown(p->fd, SHUT_RDWR) == -1 && errno != ENOTCONN)
                perror("shutdown");
            break;
        }
    }
    return NULL;
}

static void close_port(struct port *p)
{
    unsigned long dropped = queue_close(&p->q);
    int flushed = mac_table_flush_port(&table, p->id);

    if (shutdown(p->fd, SHUT_RDWR) == -1 && errno != ENOTCONN)
        perror("shutdown");
    int err = pthread_join(p->tx, NULL);
    if (err)
        fprintf(stderr, "pthread_join: %s\n", strerror(err));

    printf("port %d: closed, %lu frames dropped, %d MACs flushed\n",
           p->id, dropped, flushed);
}

static void *receiver(void *arg)
{
    struct port *p = arg;
    struct frame_hdr h;
    uint8_t payload[MAX_PAYLOAD];
    char where[64];
    int r;

    while ((r = frame_read(p->fd, &h, payload)) == 1) {
        if (mac_is_broadcast(h.src)) {
            printf("port %d: broadcast source MAC, frame dropped\n", p->id);
            continue;
        }
        int learned = mac_table_insert(&table, h.src, p->id);
        if (learned == MAC_LIMIT) {
            printf("port %d: port security, more than %d MACs, shutting port down\n",
                   p->id, atomic_load(&max_per_port));
            break;
        }
        if (learned == MAC_ERR)
            fprintf(stderr, "port %d: MAC table insert failed\n", p->id);

        struct pkt *k = pkt_new(&h, payload);
        if (!k) {
            perror("pkt_new");
            continue;
        }
        forward(p, k, where, sizeof where);
        pkt_unref(k);
        log_frame(p->id, &h, payload, where);
    }

    if (r == 0)
        printf("port %d: host disconnected\n", p->id);
    else if (r == -1)
        printf("port %d: closing (%s)\n", p->id, strerror(errno));

    close_port(p);

    lock(&ports_lock);
    if (close(p->fd) == -1)
        perror("close");
    p->fd = -1;
    p->state = PORT_DONE;
    unlock(&ports_lock);
    post_port();
    return NULL;
}

static void *ager(void *arg)
{
    (void)arg;

    lock(&age_lock);
    while (!stopping) {
        struct timespec until;
        if (clock_gettime(CLOCK_MONOTONIC, &until) == -1)
            die_err("clock_gettime", errno);
        until.tv_sec += 1;

        int err = 0;
        while (!stopping && err != ETIMEDOUT) {
            err = pthread_cond_timedwait(&age_cond, &age_lock, &until);
            if (err && err != ETIMEDOUT)
                die_err("pthread_cond_timedwait", err);
        }
        if (stopping)
            break;

        unlock(&age_lock);
        mac_table_age(&table, atomic_load(&max_age));
        lock(&age_lock);
    }
    unlock(&age_lock);
    return NULL;
}

static void begin_shutdown(void)
{
    lock(&age_lock);
    atomic_store(&stopping, 1);
    cond_broadcast(&age_cond);
    unlock(&age_lock);

    if (write(stop_pipe[1], "x", 1) == -1)
        perror("write stop pipe");
    post_port();
}

static void reload_config(void)
{
    if (!cfg_path) {
        printf("SIGHUP: no config file given (-c), nothing to reload\n");
        return;
    }
    struct config next = cfg;
    if (config_load(cfg_path, &next) == -1) {
        printf("SIGHUP: config error in %s, keeping old settings\n", cfg_path);
        return;
    }
    cfg = next;
    apply_config();
    printf("SIGHUP: config reloaded from %s (age %d, max_macs %d, "
           "macs_per_port %d, wait %d)\n",
           cfg_path, cfg.age, cfg.max_macs, cfg.per_port, cfg.wait);
}

static void dump_macs(void)
{
    FILE *f = fopen(dump_path, "w");
    if (!f) {
        perror(dump_path);
        return;
    }
    int n = mac_table_dump(&table, f);
    if (fclose(f) == EOF) {
        perror(dump_path);
        return;
    }
    printf("SIGUSR1: %d MAC entries written to %s\n", n, dump_path);
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
            printf("%s: shutting down\n", sig == SIGINT ? "SIGINT" : "SIGTERM");
            begin_shutdown();
            return NULL;
        }
        if (sig == SIGHUP)
            reload_config();
        else if (sig == SIGUSR1)
            dump_macs();
    }
}

static void reap(struct port *p)
{
    int err = pthread_join(p->rx, NULL);
    if (err)
        fprintf(stderr, "pthread_join: %s\n", strerror(err));
    p->state = PORT_FREE;
}

static struct port *grab_port(void)
{
    struct port *free_port = NULL;

    lock(&ports_lock);
    for (int i = 0; i < nports; i++) {
        if (ports[i].state == PORT_DONE)
            reap(&ports[i]);
        if (ports[i].state == PORT_FREE && !free_port)
            free_port = &ports[i];
    }
    if (free_port)
        free_port->state = PORT_ACTIVE;
    unlock(&ports_lock);

    return free_port;
}

static void release_port(struct port *p)
{
    lock(&ports_lock);
    p->state = PORT_FREE;
    unlock(&ports_lock);
    post_port();
}

static int wait_port(void)
{
    struct timespec until;
    if (clock_gettime(CLOCK_REALTIME, &until) == -1) {
        perror("clock_gettime");
        return -1;
    }
    until.tv_sec += atomic_load(&wait_secs);

    while (sem_timedwait(&port_sem, &until) == -1) {
        if (errno == EINTR)
            continue;
        if (errno != ETIMEDOUT)
            perror("sem_timedwait");
        return -1;
    }
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

    printf("switch full, rejecting connection\n");
    if (frame_write(fd, &h, (const uint8_t *)msg) == -1)
        perror("switch full reply");
    if (close(fd) == -1)
        perror("close");
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
    if (listen(s, 16) == -1) {
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

static void free_ports(int n)
{
    for (int i = 0; i < n; i++)
        queue_destroy(&ports[i].q);
    free(ports);
    ports = NULL;
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
            perror("poll");
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
                perror("accept");
            continue;
        }

        if (wait_port() == -1) {
            reject(fd);
            continue;
        }
        if (atomic_load(&stopping)) {
            post_port();
            if (close(fd) == -1)
                perror("close");
            return;
        }

        struct port *p = grab_port();
        if (!p) {
            post_port();
            reject(fd);
            continue;
        }

        p->fd = fd;
        queue_open(&p->q);

        int err = pthread_create(&p->tx, NULL, sender, p);
        if (err) {
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
            queue_close(&p->q);
            if (close(fd) == -1)
                perror("close");
            release_port(p);
            continue;
        }

        err = pthread_create(&p->rx, NULL, receiver, p);
        if (err) {
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
            close_port(p);
            if (close(fd) == -1)
                perror("close");
            release_port(p);
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip))
            snprintf(ip, sizeof ip, "?");
        printf("port %d: host connected from %s:%u\n",
               p->id, ip, ntohs(addr.sin_port));
    }
}

static void stop_ports(void)
{
    lock(&ports_lock);
    for (int i = 0; i < nports; i++)
        if (ports[i].state == PORT_ACTIVE &&
            shutdown(ports[i].fd, SHUT_RDWR) == -1 && errno != ENOTCONN)
            perror("shutdown");
    unlock(&ports_lock);

    for (int i = 0; i < nports; i++) {
        lock(&ports_lock);
        int busy = ports[i].state != PORT_FREE;
        unlock(&ports_lock);
        if (!busy)
            continue;

        int err = pthread_join(ports[i].rx, NULL);
        if (err)
            fprintf(stderr, "pthread_join: %s\n", strerror(err));
        lock(&ports_lock);
        ports[i].state = PORT_FREE;
        unlock(&ports_lock);
    }
}

static void join_thread(pthread_t t, const char *name)
{
    int err = pthread_join(t, NULL);
    if (err)
        fprintf(stderr, "pthread_join %s: %s\n", name, strerror(err));
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-c config] [-d dump_file] [-a age_secs] [-k max_macs]\n"
            "          [-m macs_per_port] [-w wait_secs] <tcp_port> <num_ports 1-%d>\n"
            "defaults: -a 30 -k 1024 -m 16 -w 5 -d mac_table.txt "
            "(0 for -k or -m means no limit)\n",
            prog, MAX_PORTS);
}

int main(int argc, char **argv)
{
    int tcp_port;
    int opt;
    int bad = 0;
    struct config cli = { -1, -1, -1, -1 };

    while ((opt = getopt(argc, argv, "c:d:a:k:m:w:")) != -1) {
        if (opt == 'c')
            cfg_path = optarg;
        else if (opt == 'd')
            dump_path = optarg;
        else if (opt == 'a')
            bad |= parse_int(optarg, 1, 86400, &cli.age);
        else if (opt == 'k')
            bad |= parse_int(optarg, 0, 1000000, &cli.max_macs);
        else if (opt == 'm')
            bad |= parse_int(optarg, 0, 1000000, &cli.per_port);
        else if (opt == 'w')
            bad |= parse_int(optarg, 0, 3600, &cli.wait);
        else
            bad = 1;
    }

    if (bad || argc - optind != 2 ||
        parse_int(argv[optind], 1, 65535, &tcp_port) == -1 ||
        parse_int(argv[optind + 1], 1, MAX_PORTS, &nports) == -1) {
        usage(argv[0]);
        return 1;
    }

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

    if (setvbuf(stdout, NULL, _IOLBF, 0) != 0)
        perror("setvbuf");

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

    int listener = open_listener(tcp_port);
    if (listener == -1)
        return 1;
    if (pipe(stop_pipe) == -1) {
        perror("pipe");
        close(listener);
        return 1;
    }

    ports = calloc((size_t)nports, sizeof *ports);
    if (!ports) {
        perror("calloc");
        return 1;
    }
    for (int i = 0; i < nports; i++) {
        ports[i].id = i + 1;
        ports[i].fd = -1;
        ports[i].state = PORT_FREE;
        err = queue_init(&ports[i].q, QUEUE_CAP);
        if (err) {
            fprintf(stderr, "queue_init: %s\n", strerror(err));
            free_ports(i);
            return 1;
        }
    }

    err = mac_table_init(&table, cfg.max_macs, cfg.per_port);
    if (err) {
        fprintf(stderr, "mac_table_init: %s\n", strerror(err));
        free_ports(nports);
        return 1;
    }
    apply_config();

    if (sem_init(&port_sem, 0, (unsigned int)nports) == -1) {
        perror("sem_init");
        return 1;
    }

    pthread_condattr_t attr;
    pthread_t age_thread;
    pthread_t sig_thread;
    if ((err = pthread_condattr_init(&attr)) ||
        (err = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) ||
        (err = pthread_cond_init(&age_cond, &attr)) ||
        (err = pthread_condattr_destroy(&attr)) ||
        (err = pthread_create(&age_thread, NULL, ager, NULL)) ||
        (err = pthread_create(&sig_thread, NULL, signal_loop, NULL))) {
        fprintf(stderr, "thread setup: %s\n", strerror(err));
        return 1;
    }

    printf("switch listening on TCP %d with %d ports "
           "(age %ds, max %d MACs, %d per port, wait %ds)\n",
           tcp_port, nports, cfg.age, cfg.max_macs, cfg.per_port, cfg.wait);

    accept_loop(listener);

    if (!atomic_load(&stopping)) {
        err = pthread_kill(sig_thread, SIGTERM);
        if (err)
            fprintf(stderr, "pthread_kill: %s\n", strerror(err));
    }

    if (close(listener) == -1)
        perror("close");
    stop_ports();
    join_thread(sig_thread, "signal");
    join_thread(age_thread, "aging");

    mac_table_destroy(&table);
    free_ports(nports);
    if (sem_destroy(&port_sem) == -1)
        perror("sem_destroy");
    pthread_cond_destroy(&age_cond);
    close(stop_pipe[0]);
    close(stop_pipe[1]);

    printf("switch stopped cleanly\n");
    return 0;
}
