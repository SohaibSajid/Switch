#ifndef CONFIG_H
#define CONFIG_H

#define CFG_MAX_PORT 66

/* Values that are not in the file stay "unset", so a reload only touches
 * what the file talks about. */
struct config {
    int age;
    int max_macs;
    int per_port;
    int wait;
    int rate;                       /* frames per second per port, 0 = off */
    int weight[3];                  /* high:normal:low */
    int qos_age;                    /* ms before a waiting frame jumps ahead, 0 = off */
    int trunk;                      /* -1 unset, 0 none, else port */
    int vlan[CFG_MAX_PORT + 1];     /* 0 unset */
    int mirror[CFG_MAX_PORT + 1];   /* 0 unset, -1 off, else sniffer port */
    int port_rate[CFG_MAX_PORT + 1];/* -1 unset */
};

void config_defaults(struct config *c);
int config_load(const char *path, struct config *c);

#endif
