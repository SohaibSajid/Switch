#ifndef FRAME_H
#define FRAME_H

#include <stddef.h>
#include <stdint.h>

#define FRAME_HDR 17
#define MAX_PAYLOAD 1500
#define MAC_LEN 6
#define MAC_STR_LEN 18

enum { PRI_HIGH = 0, PRI_NORMAL = 1, PRI_LOW = 2 };

struct frame_hdr {
    uint16_t len;
    uint8_t src[MAC_LEN];
    uint8_t dst[MAC_LEN];
    uint16_t vlan;
    uint8_t pri;
};

int frame_pack(const struct frame_hdr *h, uint8_t *buf);
int frame_unpack(const uint8_t *buf, struct frame_hdr *h);

int mac_parse(const char *s, uint8_t *mac);
void mac_format(const uint8_t *mac, char *out);
int mac_is_broadcast(const uint8_t *mac);

void frame_text(const uint8_t *payload, uint16_t len, char *out, size_t size);

#endif
