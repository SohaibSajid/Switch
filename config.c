#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int number(const char *val, int min, int max, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(val, &end, 10);
    if (errno || end == val || *end != '\0' || v < min || v > max)
        return -1;
    *out = (int)v;
    return 0;
}

static int set(const char *path, int line, const char *val, int min, int max, int *out)
{
    if (number(val, min, max, out) == -1) {
        fprintf(stderr, "%s:%d: value must be %d-%d\n", path, line, min, max);
        return -1;
    }
    return 0;
}

static int is_off(const char *val)
{
    return strcmp(val, "off") == 0 || strcmp(val, "none") == 0;
}

/* "vlan.3" -> 3 when the key starts with prefix, else 0, -1 for a bad number */
static int port_key(const char *key, const char *prefix)
{
    size_t n = strlen(prefix);
    int id;

    if (strncmp(key, prefix, n) != 0 || key[n] != '.')
        return 0;
    if (number(key + n + 1, 1, CFG_MAX_PORT, &id) == -1)
        return -1;
    return id;
}

static int parse_weights(const char *path, int line, const char *val, int *w)
{
    int a, b, c;
    char extra;

    if (sscanf(val, "%d:%d:%d%c", &a, &b, &c, &extra) != 3 ||
        a < 1 || b < 1 || c < 1 || a > 100 || b > 100 || c > 100) {
        fprintf(stderr, "%s:%d: weights must look like 5:3:1 (each 1-100)\n", path, line);
        return -1;
    }
    w[0] = a;
    w[1] = b;
    w[2] = c;
    return 0;
}

void config_defaults(struct config *c)
{
    c->age = 30;
    c->max_macs = 1024;
    c->per_port = 16;
    c->wait = 5;
    c->rate = 0;
    c->weight[0] = 5;
    c->weight[1] = 3;
    c->weight[2] = 1;
    c->qos_age = 200;
    c->trunk = -1;
    for (int i = 0; i <= CFG_MAX_PORT; i++) {
        c->vlan[i] = 0;
        c->mirror[i] = 0;
        c->port_rate[i] = -1;
    }
}

static int apply_line(const char *path, int line, const char *key, const char *val,
                      struct config *next)
{
    int id;

    if (strcmp(key, "age") == 0)
        return set(path, line, val, 1, 86400, &next->age);
    if (strcmp(key, "max_macs") == 0)
        return set(path, line, val, 0, 32768, &next->max_macs);
    if (strcmp(key, "macs_per_port") == 0)
        return set(path, line, val, 0, 32768, &next->per_port);
    if (strcmp(key, "wait") == 0)
        return set(path, line, val, 0, 3600, &next->wait);
    if (strcmp(key, "rate") == 0)
        return set(path, line, val, 0, 1000000, &next->rate);
    if (strcmp(key, "weights") == 0)
        return parse_weights(path, line, val, next->weight);
    if (strcmp(key, "qos_age") == 0)
        return set(path, line, val, 0, 60000, &next->qos_age);
    if (strcmp(key, "trunk") == 0) {
        if (is_off(val)) {
            next->trunk = 0;
            return 0;
        }
        return set(path, line, val, 1, CFG_MAX_PORT, &next->trunk);
    }

    if ((id = port_key(key, "vlan")) != 0) {
        if (id < 0) {
            fprintf(stderr, "%s:%d: bad port in '%s'\n", path, line, key);
            return -1;
        }
        return set(path, line, val, 1, 4094, &next->vlan[id]);
    }
    if ((id = port_key(key, "rate")) != 0) {
        if (id < 0) {
            fprintf(stderr, "%s:%d: bad port in '%s'\n", path, line, key);
            return -1;
        }
        return set(path, line, val, 0, 1000000, &next->port_rate[id]);
    }
    if ((id = port_key(key, "mirror")) != 0) {
        if (id < 0) {
            fprintf(stderr, "%s:%d: bad port in '%s'\n", path, line, key);
            return -1;
        }
        if (is_off(val)) {
            next->mirror[id] = -1;
            return 0;
        }
        return set(path, line, val, 1, CFG_MAX_PORT, &next->mirror[id]);
    }

    fprintf(stderr, "%s:%d: unknown key '%s'\n", path, line, key);
    return -1;
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
        if (apply_line(path, line, key, val, &next) == -1)
            bad = 1;
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
