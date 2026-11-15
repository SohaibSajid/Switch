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
#include <time.h>
#include <unistd.h>

#include "frame.h"
#include "net.h"

static int sock;
static uint8_t self[MAC_LEN];
static uint16_t vlan = 1;
static uint8_t pri = PRI_NORMAL;

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

static void *receiver(void *arg)
{
    (void)arg;
    struct frame_hdr h;
    uint8_t payload[MAX_PAYLOAD];
    char src[MAC_STR_LEN];
    char text[128];
    int r;

    while ((r = frame_read(sock, &h, payload)) == 1) {
        mac_format(h.src, src);
        frame_text(payload, h.len, text, sizeof text);
        printf("recv from %s vlan %u pri %u len %u: %s\n",
               src, h.vlan, h.pri, h.len, text);
    }
    if (r == 0)
        printf("switch closed the connection\n");
    else
        printf("receive error: %s\n", strerror(errno));
    return NULL;
}

static int parse_dst(const char *s, uint8_t *dst)
{
    if (strcmp(s, "bcast") == 0) {
        memset(dst, 0xFF, MAC_LEN);
        return 0;
    }
    return mac_parse(s, dst);
}

static void fill(struct frame_hdr *h, const uint8_t *dst, size_t len)
{
    h->len = (uint16_t)(len > MAX_PAYLOAD ? MAX_PAYLOAD : len);
    memcpy(h->src, self, MAC_LEN);
    memcpy(h->dst, dst, MAC_LEN);
    h->vlan = vlan;
    h->pri = pri;
}

static void cmd_send(const uint8_t *dst, const char *text)
{
    struct frame_hdr h;
    fill(&h, dst, strlen(text));
    if (frame_write(sock, &h, (const uint8_t *)text) == -1)
        printf("send failed: %s\n", strerror(errno));
}

static void cmd_slow(const uint8_t *dst, const char *text)
{
    struct frame_hdr h;
    uint8_t buf[FRAME_HDR + MAX_PAYLOAD];

    fill(&h, dst, strlen(text));
    if (frame_pack(&h, buf) == -1)
        return;
    memcpy(buf + FRAME_HDR, text, h.len);

    for (size_t i = 0; i < FRAME_HDR + (size_t)h.len; i++) {
        if (write_full(sock, buf + i, 1) == -1) {
            printf("send failed: %s\n", strerror(errno));
            return;
        }
        sleep_ms(20);
    }
}

static void cmd_burst(const uint8_t *dst, int count)
{
    char text[32];
    for (int i = 0; i < count; i++) {
        snprintf(text, sizeof text, "burst %d", i);
        cmd_send(dst, text);
    }
}

static void cmd_half(const uint8_t *dst)
{
    struct frame_hdr h;
    uint8_t buf[FRAME_HDR + 10];

    fill(&h, dst, 100);
    if (frame_pack(&h, buf) == -1)
        return;
    memset(buf + FRAME_HDR, 'x', 10);
    if (write_full(sock, buf, sizeof buf) == -1)
        printf("send failed: %s\n", strerror(errno));
}

static void help(void)
{
    printf("commands:\n"
           "  send <mac|bcast> <text>   send one frame\n"
           "  slow <mac|bcast> <text>   send one frame a byte at a time\n"
           "  burst <mac|bcast> <n>     send n frames fast\n"
           "  half <mac|bcast>          send half a frame, then quit\n"
           "  vlan <1-4094>             set vlan for next frames\n"
           "  pri <0-2>                 set priority for next frames\n"
           "  sleep <ms>                wait (for scripts)\n"
           "  quit\n");
}

static int run_line(char *line)
{
    char cmd[16];
    char arg[32];
    int off = 0;
    uint8_t dst[MAC_LEN];

    line[strcspn(line, "\n")] = '\0';
    int n = sscanf(line, "%15s %31s %n", cmd, arg, &off);
    if (n < 1)
        return 0;

    if (strcmp(cmd, "quit") == 0)
        return 1;
    if (strcmp(cmd, "help") == 0) {
        help();
        return 0;
    }
    if (n < 2) {
        printf("missing argument, type help\n");
        return 0;
    }

    if (strcmp(cmd, "vlan") == 0) {
        int v = atoi(arg);
        if (v < 1 || v > 4094)
            printf("vlan must be 1-4094\n");
        else
            vlan = (uint16_t)v;
        return 0;
    }
    if (strcmp(cmd, "pri") == 0) {
        int v = atoi(arg);
        if (v < 0 || v > 2)
            printf("pri must be 0-2\n");
        else
            pri = (uint8_t)v;
        return 0;
    }
    if (strcmp(cmd, "sleep") == 0) {
        sleep_ms(atol(arg));
        return 0;
    }

    if (parse_dst(arg, dst) == -1) {
        printf("bad MAC: %s\n", arg);
        return 0;
    }

    if (strcmp(cmd, "send") == 0)
        cmd_send(dst, line + off);
    else if (strcmp(cmd, "slow") == 0)
        cmd_slow(dst, line + off);
    else if (strcmp(cmd, "burst") == 0)
        cmd_burst(dst, atoi(line + off));
    else if (strcmp(cmd, "half") == 0) {
        cmd_half(dst);
        return 1;
    } else
        printf("unknown command, type help\n");
    return 0;
}

static int connect_to(const char *ip, const char *port_str)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;

    int port = atoi(port_str);
    if (port < 1 || port > 65535 || inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "bad address %s:%s\n", ip, port_str);
        return -1;
    }
    addr.sin_port = htons((uint16_t)port);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == -1) {
        perror("socket");
        return -1;
    }
    if (connect(s, (struct sockaddr *)&addr, sizeof addr) == -1) {
        perror("connect");
        close(s);
        return -1;
    }
    return s;
}

int main(int argc, char **argv)
{
    if (argc != 4 || mac_parse(argv[3], self) == -1 || mac_is_broadcast(self)) {
        fprintf(stderr, "usage: %s <switch_ip> <tcp_port> <mac>\n", argv[0]);
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

    sock = connect_to(argv[1], argv[2]);
    if (sock == -1)
        return 1;

    pthread_t rx;
    int err = pthread_create(&rx, NULL, receiver, NULL);
    if (err) {
        fprintf(stderr, "pthread_create: %s\n", strerror(err));
        close(sock);
        return 1;
    }

    char line[MAX_PAYLOAD + 64];
    while (fgets(line, sizeof line, stdin))
        if (run_line(line))
            break;

    if (shutdown(sock, SHUT_RDWR) == -1 && errno != ENOTCONN)
        perror("shutdown");
    err = pthread_join(rx, NULL);
    if (err)
        fprintf(stderr, "pthread_join: %s\n", strerror(err));
    if (close(sock) == -1)
        perror("close");
    return 0;
}
