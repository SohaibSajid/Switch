#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int set(const char *path, int line, const char *val, int min, int max, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(val, &end, 10);
    if (errno || end == val || *end != '\0' || v < min || v > max) {
        fprintf(stderr, "%s:%d: value must be %d-%d\n", path, line, min, max);
        return -1;
    }
    *out = (int)v;
    return 0;
}

int config_load(const char *path, struct config *c)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return -1;
    }

    struct config next = *c;
    char buf[256];
    int line = 0;
    int bad = 0;

    while (fgets(buf, sizeof buf, f)) {
        line++;
        buf[strcspn(buf, "#\n")] = '\0';

        char key[32];
        char val[64];
        char extra[2];
        int n = sscanf(buf, " %31[^= \t] = %63s %1s", key, val, extra);
        if (n <= 0)
            continue;
        if (n != 2) {
            fprintf(stderr, "%s:%d: expected 'key = value'\n", path, line);
            bad = 1;
            continue;
        }

        if (strcmp(key, "age") == 0)
            bad |= set(path, line, val, 1, 86400, &next.age);
        else if (strcmp(key, "max_macs") == 0)
            bad |= set(path, line, val, 0, 1000000, &next.max_macs);
        else if (strcmp(key, "macs_per_port") == 0)
            bad |= set(path, line, val, 0, 1000000, &next.per_port);
        else if (strcmp(key, "wait") == 0)
            bad |= set(path, line, val, 0, 3600, &next.wait);
        else {
            fprintf(stderr, "%s:%d: unknown key '%s'\n", path, line, key);
            bad = 1;
        }
    }

    if (ferror(f)) {
        perror(path);
        bad = 1;
    }
    if (fclose(f) == EOF) {
        perror(path);
        bad = 1;
    }
    if (bad)
        return -1;

    *c = next;
    return 0;
}
