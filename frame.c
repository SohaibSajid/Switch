#define _POSIX_C_SOURCE 200809L

#include "frame.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

enum { OFF_LEN = 0, OFF_SRC = 2, OFF_DST = 8, OFF_VLAN = 14, OFF_PRI = 16 };

static int hdr_ok(const struct frame_hdr *h)
{
    return h->len <= MAX_PAYLOAD && h->pri <= PRI_LOW;
}

int frame_pack(const struct frame_hdr *h, uint8_t *buf)
{
    if (!hdr_ok(h))
        return -1;

    uint16_t len = htons(h->len);
    uint16_t vlan = htons(h->vlan);

    memcpy(buf + OFF_LEN, &len, 2);
    memcpy(buf + OFF_SRC, h->src, MAC_LEN);
    memcpy(buf + OFF_DST, h->dst, MAC_LEN);
    memcpy(buf + OFF_VLAN, &vlan, 2);
    buf[OFF_PRI] = h->pri;
    return 0;
}

int frame_unpack(const uint8_t *buf, struct frame_hdr *h)
{
    uint16_t len;
    uint16_t vlan;

    memcpy(&len, buf + OFF_LEN, 2);
    memcpy(h->src, buf + OFF_SRC, MAC_LEN);
    memcpy(h->dst, buf + OFF_DST, MAC_LEN);
    memcpy(&vlan, buf + OFF_VLAN, 2);
    h->len = ntohs(len);
    h->vlan = ntohs(vlan);
    h->pri = buf[OFF_PRI];

    return hdr_ok(h) ? 0 : -1;
}

int mac_parse(const char *s, uint8_t *mac)
{
    unsigned int b[MAC_LEN];
    char extra;

    int n = sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &extra);
    if (n != MAC_LEN)
        return -1;

    for (int i = 0; i < MAC_LEN; i++)
        mac[i] = (uint8_t)b[i];
    return 0;
}

void mac_format(const uint8_t *mac, char *out)
{
    snprintf(out, MAC_STR_LEN, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int mac_is_broadcast(const uint8_t *mac)
{
    for (int i = 0; i < MAC_LEN; i++)
        if (mac[i] != 0xFF)
            return 0;
    return 1;
}

void frame_text(const uint8_t *payload, uint16_t len, char *out, size_t size)
{
    size_t n = 0;
    while (n < len && n + 1 < size) {
        uint8_t c = payload[n];
        out[n] = (c >= 32 && c < 127) ? (char)c : '.';
        n++;
    }
    out[n] = '\0';
}
