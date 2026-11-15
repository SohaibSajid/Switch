#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "frame.h"
#include "mac_table.h"
#include "net.h"

#define MAX_PORTS 64

enum { PORT_FREE, PORT_ACTIVE, PORT_DONE };

struct port {
    int id;
    int fd;
    int state;
    pthread_t rx;
};

static struct port *ports;
static int nports;
static pthread_mutex_t ports_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mac_table table;

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

static void log_frame(int in, const struct frame_hdr *h,
                      const uint8_t *payload, int out)
{
    char src[MAC_STR_LEN];
    char dst[MAC_STR_LEN];
    char text[64];
    char where[32];

    mac_format(h->src, src);
    mac_format(h->dst, dst);
    frame_text(payload, h->len, text, sizeof text);

    if (mac_is_broadcast(h->dst))
        snprintf(where, sizeof where, "flood (broadcast)");
    else if (out < 0)
        snprintf(where, sizeof where, "flood (unknown)");
    else if (out == in)
        snprintf(where, sizeof where, "drop (same port)");
    else
        snprintf(where, sizeof where, "port %d", out);

    printf("port %d: %s -> %s vlan %u pri %u len %u \"%s\" => %s\n",
           in, src, dst, h->vlan, h->pri, h->len, text, where);
}

static void *receiver(void *arg)
{
    struct port *p = arg;
    struct frame_hdr h;
    uint8_t payload[MAX_PAYLOAD];
    int r;

    while ((r = frame_read(p->fd, &h, payload)) == 1) {
        if (mac_is_broadcast(h.src)) {
            printf("port %d: broadcast source MAC, frame dropped\n", p->id);
            continue;
        }
        if (mac_table_insert(&table, h.src, p->id) == MAC_ERR)
            fprintf(stderr, "port %d: MAC table insert failed\n", p->id);

        int out = mac_is_broadcast(h.dst) ? -1 : mac_table_lookup(&table, h.dst);
        log_frame(p->id, &h, payload, out);
    }

    if (r == 0)
        printf("port %d: host disconnected\n", p->id);
    else
        printf("port %d: closing (%s)\n", p->id, strerror(errno));

    if (close(p->fd) == -1)
        perror("close");

    lock(&ports_lock);
    p->state = PORT_DONE;
    unlock(&ports_lock);
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

        struct port *p = grab_port();
        if (!p) {
            printf("switch full, rejecting connection\n");
            if (close(fd) == -1)
                perror("close");
            continue;
        }

        p->fd = fd;
        int err = pthread_create(&p->rx, NULL, receiver, p);
        if (err) {
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
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

int main(int argc, char **argv)
{
    int tcp_port;

    if (argc != 3 || parse_int(argv[1], 1, 65535, &tcp_port) == -1 ||
        parse_int(argv[2], 1, MAX_PORTS, &nports) == -1) {
        fprintf(stderr, "usage: %s <tcp_port> <num_ports 1-%d>\n", argv[0], MAX_PORTS);
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
    }

    int err = mac_table_init(&table);
    if (err) {
        fprintf(stderr, "mac_table_init: %s\n", strerror(err));
        free(ports);
        return 1;
    }

    int listener = open_listener(tcp_port);
    if (listener == -1) {
        mac_table_destroy(&table);
        free(ports);
        return 1;
    }

    printf("switch listening on TCP %d with %d ports\n", tcp_port, nports);
    accept_loop(listener);
    return 0;
}
