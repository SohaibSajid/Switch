#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "frame.h"
#include "mac_table.h"

#define THREADS 8
#define PER_THREAD 2000
#define MOVES 6

static int fails;

#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                                   \
        }                                                              \
    } while (0)

static struct mac_table table;

static void make_mac(int id, int n, uint8_t *mac)
{
    mac[0] = 0x02;
    mac[1] = (uint8_t)id;
    mac[2] = (uint8_t)(n >> 8);
    mac[3] = (uint8_t)n;
    mac[4] = (uint8_t)(n * 7);
    mac[5] = (uint8_t)(id * 31 + n);
}

static void test_frame(void)
{
    struct frame_hdr h = {0};
    struct frame_hdr out;
    uint8_t buf[FRAME_HDR];

    CHECK(mac_parse("02:00:00:00:00:0A", h.src) == 0);
    CHECK(mac_parse("ff:ff:ff:ff:ff:ff", h.dst) == 0);
    h.len = 1500;
    h.vlan = 4094;
    h.pri = PRI_LOW;

    CHECK(frame_pack(&h, buf) == 0);
    CHECK(buf[0] == 0x05 && buf[1] == 0xDC);
    CHECK(memcmp(buf + 2, h.src, MAC_LEN) == 0);
    CHECK(memcmp(buf + 8, h.dst, MAC_LEN) == 0);
    CHECK(buf[14] == 0x0F && buf[15] == 0xFE);
    CHECK(buf[16] == PRI_LOW);

    CHECK(frame_unpack(buf, &out) == 0);
    CHECK(out.len == 1500 && out.vlan == 4094 && out.pri == PRI_LOW);
    CHECK(memcmp(out.src, h.src, MAC_LEN) == 0);
    CHECK(memcmp(out.dst, h.dst, MAC_LEN) == 0);
    CHECK(mac_is_broadcast(out.dst));
    CHECK(!mac_is_broadcast(out.src));

    uint8_t raw[FRAME_HDR] = {
        0x00, 0x05,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x0B,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x0C,
        0x00, 0x0A,
        0x01,
    };
    CHECK(frame_unpack(raw, &out) == 0);
    CHECK(out.len == 5 && out.vlan == 10 && out.pri == PRI_NORMAL);
    CHECK(out.src[5] == 0x0B && out.dst[5] == 0x0C);

    raw[0] = 0x05;
    raw[1] = 0xDD;
    CHECK(frame_unpack(raw, &out) == -1);
    raw[0] = 0x00;
    raw[16] = 3;
    CHECK(frame_unpack(raw, &out) == -1);

    h.len = 1501;
    CHECK(frame_pack(&h, buf) == -1);

    char s[MAC_STR_LEN];
    mac_format(h.src, s);
    CHECK(strcmp(s, "02:00:00:00:00:0A") == 0);
    CHECK(mac_parse("02:00:00:00:0A", h.src) == -1);
    CHECK(mac_parse("02:00:00:00:00:0A:FF", h.src) == -1);
    CHECK(mac_parse("zz:00:00:00:00:0A", h.src) == -1);

    printf("frame tests done\n");
}

static void test_mac_basic(void)
{
    uint8_t a[MAC_LEN];
    uint8_t b[MAC_LEN];

    CHECK(mac_parse("02:00:00:00:00:0A", a) == 0);
    CHECK(mac_parse("02:00:00:00:00:0B", b) == 0);

    CHECK(mac_table_lookup(&table, a) == -1);
    CHECK(mac_table_insert(&table, a, 1) == MAC_NEW);
    CHECK(mac_table_insert(&table, b, 2) == MAC_NEW);
    CHECK(mac_table_lookup(&table, a) == 1);
    CHECK(mac_table_lookup(&table, b) == 2);
    CHECK(mac_table_insert(&table, a, 1) == MAC_SAME);
    CHECK(mac_table_insert(&table, a, 3) == MAC_MOVED);
    CHECK(mac_table_lookup(&table, a) == 3);
    CHECK(mac_table_count(&table) == 2);

    printf("mac basic tests done\n");
}

static void *learner(void *arg)
{
    int id = (int)(intptr_t)arg;
    int port = id + 1;
    intptr_t bad = 0;
    uint8_t mac[MAC_LEN];

    for (int n = 0; n < PER_THREAD; n++) {
        make_mac(id, n, mac);
        if (mac_table_insert(&table, mac, port) != MAC_NEW)
            bad++;
        if (mac_table_lookup(&table, mac) != port)
            bad++;

        int other = (id + 1) % THREADS;
        make_mac(other, n, mac);
        int p = mac_table_lookup(&table, mac);
        if (p != -1 && p != other + 1)
            bad++;
    }
    return (void *)bad;
}

static void *mover(void *arg)
{
    const uint8_t *mac = arg;
    for (int i = 0; i < MOVES; i++)
        mac_table_insert(&table, mac, i % 2 ? 100 : 101);
    return NULL;
}

static void *watcher(void *arg)
{
    const uint8_t *mac = arg;
    intptr_t bad = 0;
    for (int i = 0; i < 5000; i++) {
        int p = mac_table_lookup(&table, mac);
        if (p != 100 && p != 101)
            bad++;
    }
    return (void *)bad;
}

static int join(pthread_t th)
{
    void *res;
    int err = pthread_join(th, &res);
    if (err) {
        fprintf(stderr, "pthread_join: %s\n", strerror(err));
        return 1;
    }
    return (int)(intptr_t)res;
}

static void test_mac_threads(void)
{
    pthread_t th[THREADS + 2];
    uint8_t roam[MAC_LEN];
    size_t start = mac_table_count(&table);
    int err;

    CHECK(mac_parse("02:00:00:00:00:FE", roam) == 0);
    CHECK(mac_table_insert(&table, roam, 100) == MAC_NEW);

    for (int i = 0; i < THREADS; i++) {
        err = pthread_create(&th[i], NULL, learner, (void *)(intptr_t)i);
        if (err) {
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
            fails++;
            return;
        }
    }
    err = pthread_create(&th[THREADS], NULL, mover, roam);
    if (!err)
        err = pthread_create(&th[THREADS + 1], NULL, watcher, roam);
    if (err) {
        fprintf(stderr, "pthread_create: %s\n", strerror(err));
        fails++;
        return;
    }

    int bad = 0;
    for (int i = 0; i < THREADS + 2; i++)
        bad += join(th[i]);
    CHECK(bad == 0);

    CHECK(mac_table_count(&table) == start + 1 + THREADS * PER_THREAD);

    uint8_t mac[MAC_LEN];
    for (int id = 0; id < THREADS; id++)
        for (int n = 0; n < PER_THREAD; n++) {
            make_mac(id, n, mac);
            if (mac_table_lookup(&table, mac) != id + 1)
                bad++;
        }
    CHECK(bad == 0);
    CHECK(mac_table_lookup(&table, roam) == ((MOVES - 1) % 2 ? 100 : 101));

    printf("mac thread tests done: %d threads, %zu entries\n",
           THREADS + 2, mac_table_count(&table));
}

int main(void)
{
    int err = mac_table_init(&table);
    if (err) {
        fprintf(stderr, "mac_table_init: %s\n", strerror(err));
        return 1;
    }

    test_frame();
    test_mac_basic();
    test_mac_threads();

    mac_table_destroy(&table);

    if (fails) {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
