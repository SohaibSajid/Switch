#ifndef CONFIG_H
#define CONFIG_H

struct config {
    int age;
    int max_macs;
    int per_port;
    int wait;
};

int config_load(const char *path, struct config *c);

#endif
