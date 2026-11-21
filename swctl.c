#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "net.h"
#include "shm_stats.h"

static const char *sock_path = "/tmp/vswitch.sock";
static const char *stats_name = STATS_NAME;

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-s admin_socket] [-S shm_name] <command>\n"
            "commands:\n"
            "  show mac                    list the MAC table\n"
            "  show ports                  list ports: state, vlan, queues, counters\n"
            "  shutdown <port>             disconnect the host and close the port\n"
            "  up <port>                   open a shut down port again\n"
            "  vlan <port> <vlan|trunk>    put a port in a VLAN (1-4094) or make it the trunk\n"
            "  trunk <port|off>            choose the trunk port, or none\n"
            "  mirror <port> <sniffer|off> copy the traffic of a port to a sniffer port\n"
            "  rate <port> <frames/s>      rate limit for a port, 0 = unlimited\n"
            "  clear mac                   empty the MAC table\n"
            "  stats                       print counters once (shared memory)\n"
            "  stats --watch               print counters every second (shared memory)\n",
            prog);
}

static int send_command(const char *cmd)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof addr.sun_path) {
        fprintf(stderr, "socket path too long: %s\n", sock_path);
        return 1;
    }
    strcpy(addr.sun_path, sock_path);

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == -1) {
        perror("socket");
        return 1;
    }
    if (connect(s, (struct sockaddr *)&addr, sizeof addr) == -1) {
        fprintf(stderr, "cannot reach switch at %s: %s\n", sock_path, strerror(errno));
        close(s);
        return 1;
    }

    char line[160];
    int n = snprintf(line, sizeof line, "%s\n", cmd);
    if (n < 0 || (size_t)n >= sizeof line) {
        fprintf(stderr, "command too long\n");
        close(s);
        return 1;
    }
    if (write_full(s, line, (size_t)n) == -1 || shutdown(s, SHUT_WR) == -1) {
        perror("send");
        close(s);
        return 1;
    }

    char buf[4096];
    ssize_t r;
    int status = 0;
    int first = 1;
    while ((r = read(s, buf, sizeof buf)) != 0) {
        if (r < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            status = 1;
            break;
        }
        if (first && strncmp(buf, "error:", 6) == 0)
            status = 1;
        first = 0;
        if (fwrite(buf, 1, (size_t)r, stdout) != (size_t)r) {
            perror("stdout");
            status = 1;
            break;
        }
    }
    if (close(s) == -1)
        perror("close");
    return status;
}

static void print_stats(const struct shm_stats *snap)
{
    fprintf(stdout, "%-4s  %-5s  %10s  %10s  %10s  %10s\n",
            "PORT", "STATE", "IN", "OUT", "DROPPED", "MIRRORED");
    for (int id = 1; id <= snap->nports && id <= STATS_MAX_PORTS; id++) {
        const uint64_t *c = snap->port[id].count;
        fprintf(stdout, "%-4d  %-5s  %10llu  %10llu  %10llu  %10llu\n",
                id, snap->port[id].up ? "up" : "down",
                (unsigned long long)c[STAT_IN], (unsigned long long)c[STAT_OUT],
                (unsigned long long)c[STAT_DROPPED],
                (unsigned long long)c[STAT_MIRRORED]);
    }
    fflush(stdout);
}

static int run_stats(int watch)
{
    struct shm_stats *s = stats_attach(stats_name);
    if (!s) {
        fprintf(stderr, "cannot open shared memory %s (is the switch running?): %s\n",
                stats_name, strerror(errno));
        return 1;
    }

    int clear = watch && isatty(STDOUT_FILENO);
    struct shm_stats snap;
    for (;;) {
        stats_snapshot(s, &snap);
        if (kill(snap.pid, 0) == -1 && errno == ESRCH) {
            printf("switch (pid %d) has stopped\n", (int)snap.pid);
            break;
        }
        if (clear)
            fputs("\033[H\033[2J", stdout);
        if (watch) {
            time_t now = time(NULL);
            char when[32];
            strftime(when, sizeof when, "%H:%M:%S", localtime(&now));
            printf("switch pid %d   %s   (Ctrl+C to stop)\n", (int)snap.pid, when);
        }
        print_stats(&snap);
        if (!watch)
            break;

        struct timespec ts = { 1, 0 };
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
            ;
    }

    stats_detach(s);
    return 0;
}

int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "s:S:h")) != -1) {
        if (opt == 's')
            sock_path = optarg;
        else if (opt == 'S')
            stats_name = optarg;
        else {
            usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }
    if (optind >= argc) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[optind], "stats") == 0) {
        int watch = optind + 1 < argc && strcmp(argv[optind + 1], "--watch") == 0;
        return run_stats(watch);
    }

    char cmd[128] = "";
    for (int i = optind; i < argc; i++) {
        if (strlen(cmd) + strlen(argv[i]) + 2 > sizeof cmd) {
            fprintf(stderr, "command too long\n");
            return 1;
        }
        if (i > optind)
            strcat(cmd, " ");
        strcat(cmd, argv[i]);
    }
    return send_command(cmd);
}
