#define _POSIX_C_SOURCE 200809L

/*
 * Starts two switches and joins them with two pipes (one for each
 * direction, like two_way.c in Lab 05). Each switch gets its end as an
 * extra port, the trunk: ./switch -t <read fd>,<write fd> ...
 *
 *     switch A  --- pipe ab --->  switch B
 *               <--- pipe ba ---
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_EXTRA 16

static pid_t pids[2];

static void forward_signal(int sig)
{
    for (int i = 0; i < 2; i++)
        if (pids[i] > 0)
            kill(pids[i], sig);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s <tcp_port_A> <tcp_port_B> <num_ports> [switch options]\n"
            "starts ./switch twice (or $SWITCH_BIN) and joins the two with a pipe link;\n"
            "each switch gets its own admin socket /tmp/vswitch_<port>.sock,\n"
            "stats object /vswitch_stats_<port> and log file switch_<port>.log\n",
            prog);
}

/* rd and wr are the descriptors this switch keeps, the other two are closed */
static pid_t start_switch(const char *bin, const char *port, const char *nports,
                          char **extra, int nextra, int rd, int wr, int *close_fds)
{
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return -1;
    }
    if (pid > 0)
        return pid;

    for (int i = 0; i < 2; i++)
        if (close(close_fds[i]) == -1)
            perror("close");

    char link[32];
    char sock[64];
    char shm[64];
    char log[64];
    char dump[64];
    snprintf(link, sizeof link, "%d,%d", rd, wr);
    snprintf(sock, sizeof sock, "/tmp/vswitch_%s.sock", port);
    snprintf(shm, sizeof shm, "/vswitch_stats_%s", port);
    snprintf(log, sizeof log, "switch_%s.log", port);
    snprintf(dump, sizeof dump, "mac_table_%s.txt", port);

    char *args[MAX_EXTRA + 16];
    int n = 0;
    args[n++] = (char *)bin;
    args[n++] = "-t";
    args[n++] = link;
    args[n++] = "-s";
    args[n++] = sock;
    args[n++] = "-S";
    args[n++] = shm;
    args[n++] = "-l";
    args[n++] = log;
    args[n++] = "-d";
    args[n++] = dump;
    for (int i = 0; i < nextra; i++)
        args[n++] = extra[i];
    args[n++] = (char *)port;
    args[n++] = (char *)nports;
    args[n] = NULL;

    execv(bin, args);
    perror(bin);
    _exit(127);
}

int main(int argc, char **argv)
{
    if (argc < 4 || argc - 4 > MAX_EXTRA) {
        usage(argv[0]);
        return 1;
    }
    const char *bin = getenv("SWITCH_BIN");
    if (!bin)
        bin = "./switch";

    int ab[2];
    int ba[2];
    if (pipe(ab) == -1 || pipe(ba) == -1) {
        perror("pipe");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = forward_signal;
    if (sigemptyset(&sa.sa_mask) == -1 || sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    /* A reads ba and writes ab, B the other way round */
    int not_a[2] = { ab[0], ba[1] };
    int not_b[2] = { ba[0], ab[1] };
    pids[0] = start_switch(bin, argv[1], argv[3], argv + 4, argc - 4, ba[0], ab[1], not_a);
    pids[1] = start_switch(bin, argv[2], argv[3], argv + 4, argc - 4, ab[0], ba[1], not_b);

    /* the switches have their ends now; if we kept ours, nobody would ever
     * see end-of-file on the pipes when a switch dies */
    int fds[4] = { ab[0], ab[1], ba[0], ba[1] };
    for (int i = 0; i < 4; i++)
        if (close(fds[i]) == -1)
            perror("close");

    if (pids[0] == -1 || pids[1] == -1) {
        forward_signal(SIGTERM);
    }

    int status = 0;
    int left = (pids[0] > 0) + (pids[1] > 0);
    while (left > 0) {
        int st;
        pid_t p = waitpid(-1, &st, 0);
        if (p == -1) {
            if (errno == EINTR)
                continue;
            perror("waitpid");
            return 1;
        }
        left--;
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
            status = 1;
        printf("switch_pair: switch %s %s\n", p == pids[0] ? "A" : "B",
               WIFEXITED(st) ? "exited" : "was killed");
    }
    return pids[0] == -1 || pids[1] == -1 ? 1 : status;
}
