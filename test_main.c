#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "frame.h"
#include "mac_table.h"
#include "pkt.h"
#include "queue.h"

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
    CHECK(mac_table_flush_port(&table, 3) == 1);
    CHECK(mac_table_lookup(&table, a) == -1);
    CHECK(mac_table_lookup(&table, b) == 2);
    CHECK(mac_table_count(&table) == 1);
    CHECK(mac_table_insert(&table, a, 4) == MAC_NEW);
    CHECK(mac_table_clear(&table) == 2);
    CHECK(mac_table_count(&table) == 0);
    CHECK(mac_table_lookup(&table, b) == -1);

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
        mac_table_insert(&table, mac, i % 2 ? 40 : 41);
    return NULL;
}

static void *watcher(void *arg)
{
    const uint8_t *mac = arg;
    intptr_t bad = 0;
    for (int i = 0; i < 5000; i++) {
        int p = mac_table_lookup(&table, mac);
        if (p != 40 && p != 41)
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
    CHECK(mac_table_insert(&table, roam, 40) == MAC_NEW);

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
    CHECK(mac_table_lookup(&table, roam) == ((MOVES - 1) % 2 ? 40 : 41));

    printf("mac thread tests done: %d threads, %zu entries\n",
           THREADS + 2, mac_table_count(&table));
}

static void test_mac_limits(void)
{
    static struct mac_table t;
    uint8_t a[MAC_LEN], b[MAC_LEN], c[MAC_LEN], d[MAC_LEN];

    mac_parse("02:00:00:00:01:0A", a);
    mac_parse("02:00:00:00:01:0B", b);
    mac_parse("02:00:00:00:01:0C", c);
    mac_parse("02:00:00:00:01:0D", d);

    CHECK(mac_table_init(&t, 3, 0) == 0);
    CHECK(mac_table_insert(&t, a, 1) == MAC_NEW);
    CHECK(mac_table_insert(&t, b, 2) == MAC_NEW);
    CHECK(mac_table_insert(&t, c, 3) == MAC_NEW);
    CHECK(mac_table_lookup(&t, a) == 1);
    CHECK(mac_table_insert(&t, d, 4) == MAC_NEW);
    CHECK(mac_table_count(&t) == 3);
    CHECK(mac_table_lookup(&t, b) == -1);
    CHECK(mac_table_lookup(&t, a) == 1);
    CHECK(mac_table_lookup(&t, d) == 4);
    mac_table_destroy(&t);

    CHECK(mac_table_init(&t, 0, 2) == 0);
    CHECK(mac_table_insert(&t, a, 5) == MAC_NEW);
    CHECK(mac_table_insert(&t, b, 5) == MAC_NEW);
    CHECK(mac_table_insert(&t, c, 5) == MAC_LIMIT);
    CHECK(mac_table_lookup(&t, c) == -1);
    CHECK(mac_table_insert(&t, c, 6) == MAC_NEW);
    CHECK(mac_table_insert(&t, c, 5) == MAC_LIMIT);
    CHECK(mac_table_flush_port(&t, 5) == 2);
    CHECK(mac_table_insert(&t, c, 5) == MAC_MOVED);
    CHECK(mac_table_age(&t, 60) == 0);
    CHECK(mac_table_age(&t, 0) == 1);
    CHECK(mac_table_count(&t) == 0);
    mac_table_destroy(&t);

    printf("mac limit tests done\n");
}

static void test_mac_trunk(void)
{
    static struct mac_table t;
    uint8_t mac[MAC_LEN];

    CHECK(mac_table_init(&t, 0, 2) == 0);
    mac_table_set_trunk(&t, 9);
    for (int n = 0; n < 6; n++) {
        make_mac(3, n, mac);
        CHECK(mac_table_insert(&t, mac, 9) == MAC_NEW);
    }
    make_mac(3, 100, mac);
    CHECK(mac_table_insert(&t, mac, 4) == MAC_NEW);
    make_mac(3, 101, mac);
    CHECK(mac_table_insert(&t, mac, 4) == MAC_NEW);
    make_mac(3, 102, mac);
    CHECK(mac_table_insert(&t, mac, 4) == MAC_LIMIT);

    /* when the port stops being the trunk the limit applies to it again */
    mac_table_set_trunk(&t, 0);
    make_mac(3, 103, mac);
    CHECK(mac_table_insert(&t, mac, 9) == MAC_LIMIT);
    mac_table_destroy(&t);

    printf("mac trunk tests done\n");
}

static void test_mac_shm(void)
{
    const char *name = "/vswitch_test_mactable";
    uint8_t mac[MAC_LEN];
    int fixed = -1;

    mac_table_shm_unlink(name);
    struct mac_table *t1 = mac_table_shm_create(name, 100, 10);
    CHECK(t1 != NULL);
    if (!t1)
        return;
    for (int n = 0; n < 3; n++) {
        make_mac(7, n, mac);
        CHECK(mac_table_insert(t1, mac, n + 1) == MAC_NEW);
    }

    /* a second mapping sees the same entries */
    struct mac_table *t2 = mac_table_shm_attach(name, &fixed);
    CHECK(t2 != NULL);
    CHECK(fixed == 0);
    if (t2) {
        make_mac(7, 1, mac);
        CHECK(mac_table_lookup(t2, mac) == 2);
        CHECK(mac_table_count(t2) == 3);
        mac_table_shm_close(t2);
    }

    /* damage it the way a crash in the middle of an update could */
    for (int b = 0; b < MAC_BUCKETS; b++)
        if (t1->bucket[b].head != -1)
            t1->pool[t1->bucket[b].head].next = MAC_POOL + 5;
    atomic_store(&t1->count, 99);

    t2 = mac_table_shm_attach(name, &fixed);
    CHECK(t2 != NULL);
    CHECK(fixed >= 1);
    if (t2) {
        CHECK(mac_table_count(t2) == 3);
        CHECK(mac_table_port_count(t2, 1) + mac_table_port_count(t2, 2) +
              mac_table_port_count(t2, 3) == 3);
        make_mac(7, 9, mac);
        CHECK(mac_table_insert(t2, mac, 5) == MAC_NEW);
        mac_table_shm_close(t2);
    }

    mac_table_shm_close(t1);
    mac_table_shm_unlink(name);
    printf("mac shared memory tests done\n");
}

static struct pkt *mk(int pri, int id)
{
    struct frame_hdr h;
    uint8_t body[1];

    memset(&h, 0, sizeof h);
    h.len = 1;
    h.vlan = 1;
    h.pri = (uint8_t)pri;
    h.src[5] = 1;
    h.dst[5] = 2;
    body[0] = (uint8_t)id;
    return pkt_new(&h, body);
}

static void ms_sleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1)
        ;
}

static int elapsed_ms(const struct timespec *a, const struct timespec *b)
{
    return (int)((b->tv_sec - a->tv_sec) * 1000 + (b->tv_nsec - a->tv_nsec) / 1000000);
}

static void test_queue_wrr(void)
{
    struct queue q;
    int w[3] = { 5, 3, 1 };
    char got[64];
    int n = 0;

    CHECK(queue_init(&q, 64) == 0);
    queue_set_qos(&q, w, 0);
    queue_open(&q);

    for (int i = 0; i < 10; i++)
        CHECK(queue_push(&q, mk(PRI_LOW, i)) == QUEUE_OK);
    for (int i = 0; i < 10; i++)
        CHECK(queue_push(&q, mk(PRI_NORMAL, i)) == QUEUE_OK);
    for (int i = 0; i < 10; i++)
        CHECK(queue_push(&q, mk(PRI_HIGH, i)) == QUEUE_OK);
    CHECK(queue_len(&q) == 30);

    while (queue_len(&q) > 0) {
        struct pkt *k = queue_pop(&q);
        got[n++] = "HNL"[k->hdr.pri];
        pkt_unref(k);
    }
    got[n] = '\0';
    CHECK(n == 30);
    CHECK(strncmp(got, "HHHHHNNNLHHHHHNNNL", 18) == 0);
    /* nothing is lost and nothing is starved */
    int c[3] = { 0, 0, 0 };
    for (int i = 0; i < n; i++)
        c[got[i] == 'H' ? 0 : got[i] == 'N' ? 1 : 2]++;
    CHECK(c[0] == 10 && c[1] == 10 && c[2] == 10);

    /* an empty class does not hold the others up */
    for (int i = 0; i < 4; i++)
        CHECK(queue_push(&q, mk(PRI_LOW, i)) == QUEUE_OK);
    for (int i = 0; i < 4; i++) {
        struct pkt *k = queue_pop(&q);
        CHECK(k && k->hdr.pri == PRI_LOW);
        pkt_unref(k);
    }

    /* full queue: drop and count, per class */
    for (int i = 0; i < 64; i++)
        CHECK(queue_push(&q, mk(PRI_NORMAL, i)) == QUEUE_OK);
    struct pkt *extra = mk(PRI_NORMAL, 99);
    CHECK(queue_push(&q, extra) == QUEUE_FULL);
    pkt_unref(extra);
    struct pkt *high = mk(PRI_HIGH, 1);
    CHECK(queue_push(&q, high) == QUEUE_OK);
    CHECK(queue_flush(&q) == 65);

    queue_close(&q);
    struct pkt *late = mk(PRI_HIGH, 2);
    CHECK(queue_push(&q, late) == QUEUE_CLOSED);
    pkt_unref(late);
    queue_destroy(&q);

    printf("queue scheduling tests done\n");
}

static void test_queue_aging(void)
{
    struct queue q;
    int w[3] = { 5, 3, 1 };

    CHECK(queue_init(&q, 16) == 0);
    queue_set_qos(&q, w, 50);
    queue_open(&q);

    CHECK(queue_push(&q, mk(PRI_LOW, 1)) == QUEUE_OK);
    ms_sleep(120);
    for (int i = 0; i < 3; i++)
        CHECK(queue_push(&q, mk(PRI_HIGH, i)) == QUEUE_OK);

    struct pkt *k = queue_pop(&q);
    CHECK(k && k->hdr.pri == PRI_LOW);
    pkt_unref(k);
    k = queue_pop(&q);
    CHECK(k && k->hdr.pri == PRI_HIGH);
    pkt_unref(k);

    /* two aged frames: the high one gets in between */
    CHECK(queue_push(&q, mk(PRI_LOW, 2)) == QUEUE_OK);
    CHECK(queue_push(&q, mk(PRI_LOW, 3)) == QUEUE_OK);
    ms_sleep(120);
    k = queue_pop(&q);
    CHECK(k && k->hdr.pri == PRI_LOW);
    pkt_unref(k);
    k = queue_pop(&q);
    CHECK(k && k->hdr.pri == PRI_HIGH);
    pkt_unref(k);

    queue_close(&q);
    queue_destroy(&q);
    printf("queue aging tests done\n");
}

static atomic_int tick_stop;

static void *ticker(void *arg)
{
    struct queue *q = arg;
    while (!atomic_load(&tick_stop)) {
        ms_sleep(10);
        queue_refill(q, 10);
    }
    return NULL;
}

static void *popper(void *arg)
{
    struct pkt *k = queue_pop(arg);
    if (k)
        pkt_unref(k);
    return k;
}

static void test_queue_rate(void)
{
    struct queue q;
    int w[3] = { 5, 3, 1 };
    pthread_t th;
    struct timespec t0, t1;

    CHECK(queue_init(&q, 64) == 0);
    queue_set_qos(&q, w, 0);
    queue_set_rate(&q, 100);
    queue_open(&q);
    atomic_store(&tick_stop, 0);
    CHECK(pthread_create(&th, NULL, ticker, &q) == 0);

    for (int i = 0; i < 30; i++)
        CHECK(queue_push(&q, mk(PRI_NORMAL, i)) == QUEUE_OK);

    /* 100 frames/s, bucket of 10: 30 frames need about 200 ms after the burst */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 30; i++) {
        struct pkt *k = queue_pop(&q);
        CHECK(k != NULL);
        if (k)
            pkt_unref(k);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    int took = elapsed_ms(&t0, &t1);
    CHECK(took >= 150 && took <= 450);
    if (took < 150 || took > 450)
        fprintf(stderr, "rate test took %d ms\n", took);

    atomic_store(&tick_stop, 1);
    CHECK(pthread_join(th, NULL) == 0);
    queue_close(&q);
    queue_destroy(&q);
    printf("queue rate limit tests done (%d ms)\n", took);
}

static void test_queue_close(void)
{
    struct queue q;
    pthread_t th;
    void *res = &q;

    CHECK(queue_init(&q, 8) == 0);
    queue_open(&q);
    CHECK(pthread_create(&th, NULL, popper, &q) == 0);
    ms_sleep(50);
    queue_close(&q);
    CHECK(pthread_join(th, &res) == 0);
    CHECK(res == NULL);

    /* the same for a sender that is waiting for tokens */
    queue_open(&q);
    queue_set_rate(&q, 1);
    CHECK(queue_push(&q, mk(PRI_HIGH, 1)) == QUEUE_OK);
    CHECK(queue_push(&q, mk(PRI_HIGH, 2)) == QUEUE_OK);
    struct pkt *k = queue_pop(&q);
    CHECK(k != NULL);
    if (k)
        pkt_unref(k);
    CHECK(pthread_create(&th, NULL, popper, &q) == 0);
    ms_sleep(50);
    queue_close(&q);
    CHECK(pthread_join(th, &res) == 0);
    CHECK(res == NULL);
    queue_destroy(&q);
    printf("queue close tests done\n");
}

int main(void)
{
    int err = mac_table_init(&table, 0, 0);
    if (err) {
        fprintf(stderr, "mac_table_init: %s\n", strerror(err));
        return 1;
    }

    test_frame();
    test_mac_basic();
    test_mac_threads();
    test_mac_limits();
    test_mac_trunk();
    test_mac_shm();
    test_queue_wrr();
    test_queue_aging();
    test_queue_rate();
    test_queue_close();

    mac_table_destroy(&table);

    if (fails) {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
