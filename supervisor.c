#define _POSIX_C_SOURCE 200809L

/*
 * Runs ./switch and starts it again when it crashes. The MAC table is kept
 * in a shared memory object that the supervisor creates, so the new switch
 * finds everything the old one had learned.
 *
 *   ./supervisor [-M mac_shm_name] [-n max_restarts] <switch arguments>
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mac_table.h"

#define DEFAULT_SHM "/vswitch_mac"

static volatile sig_atomic_t stop_requested;
static volatile pid_t child;

/* signals for the supervisor are passed on to the switch it runs; SIGINT and
 * SIGTERM also mean that it must not be restarted afterwards */
static void pass_on(int sig)
{
    if (sig == SIGINT || sig == SIGTERM)
        stop_requested = 1;
    pid_t c = child;
    if (c > 0)
        kill(c, sig);
}

static void pause_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR && !stop_requested)
        ;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-M mac_shm_name] [-n max_restarts] <switch arguments>\n"
            "example: %s -n 5 -c switch.conf 9000 8\n",
            prog, prog);
}

int main(int argc, char **argv)
{
    const char *shm = DEFAULT_SHM;
    const char *bin = getenv("SWITCH_BIN");
    int max_restarts = 100;

    if (!bin)
        bin = "./switch";

    /* our own options come first, everything after them belongs to the switch */
    int first = 1;
    while (first < argc) {
        if (strcmp(argv[first], "-M") == 0 && first + 1 < argc) {
            shm = argv[first + 1];
            first += 2;
        } else if (strcmp(argv[first], "-n") == 0 && first + 1 < argc) {
            max_restarts = atoi(argv[first + 1]);
            first += 2;
        } else {
            break;
        }
    }
    if (first >= argc || strcmp(argv[first], "-h") == 0) {
        usage(argv[0]);
        return first >= argc ? 1 : 0;
    }

    struct mac_table *t = mac_table_shm_create(shm, 0, 0);
    if (!t) {
        /* left over from a supervisor that was killed: start from scratch */
        mac_table_shm_unlink(shm);
        t = mac_table_shm_create(shm, 0, 0);
        if (!t)
            return 1;
    }
    /* the switch maps it by itself; we only needed to create it */
    mac_table_shm_close(t);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = pass_on;
    if (sigemptyset(&sa.sa_mask) == -1 || sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1 || sigaction(SIGHUP, &sa, NULL) == -1 ||
        sigaction(SIGUSR1, &sa, NULL) == -1) {
        perror("sigaction");
        mac_table_shm_unlink(shm);
        return 1;
    }

    int restarts = 0;
    int result = 0;
    for (;;) {
        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            result = 1;
            break;
        }
        if (pid == 0) {
            char **args = calloc((size_t)(argc - first) + 4, sizeof *args);
            if (!args) {
                perror("calloc");
                _exit(127);
            }
            int n = 0;
            args[n++] = (char *)bin;
            args[n++] = "-M";
            args[n++] = (char *)shm;
            for (int i = first; i < argc; i++)
                args[n++] = argv[i];
            args[n] = NULL;
            execv(bin, args);
            perror(bin);
            _exit(127);
        }
        child = pid;
        printf("supervisor: switch started (pid %d)\n", (int)pid);
        fflush(stdout);

        int st;
        pid_t w;
        while ((w = waitpid(pid, &st, 0)) == -1 && errno == EINTR)
            ;
        child = 0;
        if (w == -1) {
            perror("waitpid");
            result = 1;
            break;
        }

        if (WIFEXITED(st)) {
            printf("supervisor: switch exited with status %d\n", WEXITSTATUS(st));
            result = WEXITSTATUS(st);
            break;
        }
        printf("supervisor: switch died from signal %d%s\n", WTERMSIG(st),
               stop_requested ? "" : ", starting it again");
        fflush(stdout);
        if (stop_requested)
            break;
        if (++restarts > max_restarts) {
            printf("supervisor: too many restarts, giving up\n");
            result = 1;
            break;
        }
        pause_ms(300);
        if (stop_requested)
            break;
    }

    mac_table_shm_unlink(shm);
    fflush(stdout);
    return result;
}
