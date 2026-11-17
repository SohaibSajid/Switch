#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

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

static int max_age = 30;
static int max_entries = 1024;
static int max_per_port = 16;
static int wait_secs = 5;

static pthread_mutex_t age_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t age_cond;
static int stopping;

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
    if (close(p->fd) == -1)
        perror("close");

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
                   p->id, max_per_port);
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
        mac_table_age(&table, max_age);
        lock(&age_lock);
    }
    unlock(&age_lock);
    return NULL;
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
    until.tv_sec += wait_secs;

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
    for (;;) {
        struct sockaddr_in addr;
        socklen_t alen = sizeof addr;

        int fd = accept(listener, (struct sockaddr *)&addr, &alen);
        if (fd == -1) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            perror("accept");
            continue;
        }

        if (wait_port() == -1) {
            reject(fd);
            continue;
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

int main(int argc, char **argv)
{
    int tcp_port;
    int opt;
    int bad = 0;

    while ((opt = getopt(argc, argv, "a:k:m:w:")) != -1) {
        if (opt == 'a')
            bad |= parse_int(optarg, 1, 86400, &max_age);
        else if (opt == 'k')
            bad |= parse_int(optarg, 0, 1000000, &max_entries);
        else if (opt == 'm')
            bad |= parse_int(optarg, 0, 1000000, &max_per_port);
        else if (opt == 'w')
            bad |= parse_int(optarg, 0, 3600, &wait_secs);
        else
            bad = 1;
    }

    if (bad || argc - optind != 2 ||
        parse_int(argv[optind], 1, 65535, &tcp_port) == -1 ||
        parse_int(argv[optind + 1], 1, MAX_PORTS, &nports) == -1) {
        fprintf(stderr,
                "usage: %s [-a age_secs] [-k max_macs] [-m macs_per_port] [-w wait_secs]\n"
                "          <tcp_port> <num_ports 1-%d>\n"
                "defaults: -a 30 -k 1024 -m 16 -w 5 (0 for -k or -m means no limit)\n",
                argv[0], MAX_PORTS);
        return 1;
    }

    if (setvbuf(stdout, NULL, _IOLBF, 0) != 0)
        perror("setvbuf");

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    if (sigemptyset(&sa.sa_mask) == -1 || sigaction(SIGPIPE, &sa, NULL) == -1) {
        perror("sigaction");
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
        int err = queue_init(&ports[i].q, QUEUE_CAP);
        if (err) {
            fprintf(stderr, "queue_init: %s\n", strerror(err));
            free_ports(i);
            return 1;
        }
    }

    int err = mac_table_init(&table, max_entries, max_per_port);
    if (err) {
        fprintf(stderr, "mac_table_init: %s\n", strerror(err));
        free_ports(nports);
        return 1;
    }

    if (sem_init(&port_sem, 0, (unsigned int)nports) == -1) {
        perror("sem_init");
        mac_table_destroy(&table);
        free_ports(nports);
        return 1;
    }

    pthread_condattr_t attr;
    pthread_t age_thread;
    if ((err = pthread_condattr_init(&attr)) ||
        (err = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) ||
        (err = pthread_cond_init(&age_cond, &attr)) ||
        (err = pthread_condattr_destroy(&attr)) ||
        (err = pthread_create(&age_thread, NULL, ager, NULL))) {
        fprintf(stderr, "aging thread setup: %s\n", strerror(err));
        return 1;
    }

    int listener = open_listener(tcp_port);
    if (listener == -1)
        return 1;

    printf("switch listening on TCP %d with %d ports "
           "(age %ds, max %d MACs, %d per port, wait %ds)\n",
           tcp_port, nports, max_age, max_entries, max_per_port, wait_secs);
    accept_loop(listener);
    return 0;
}
