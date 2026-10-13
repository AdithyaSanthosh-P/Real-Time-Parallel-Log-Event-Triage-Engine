/* T4.1: SPSC ring (spec §2.3). Single-threaded semantics, index wraparound,
 * the stop flag of the blocking calls, and multi-threaded stress tests that
 * check order and a checksum. Run under `make SAN=thread test` to have TSan
 * check the release/acquire handoff, including the non-atomic payload behind
 * each pushed pointer. */

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "spsc.h"
#include "test_main.h"

#define STRESS_ITEMS 10000000ULL

static void *num(uint64_t v) { return (void *)(uintptr_t)v; }

/* ---- single-threaded ---- */

static void test_capacity(void)
{
    CHECK(spsc_new(0) == NULL);
    CHECK(spsc_new(SIZE_MAX) == NULL);
    size_t want[][2] = { {1, 1}, {2, 2}, {3, 4}, {5, 8}, {64, 64}, {65, 128} };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        SpscRing *r = spsc_new(want[i][0]);
        CHECK(r != NULL);
        if (!r)
            continue;
        CHECK_EQ_INT(spsc_capacity(r), want[i][1]);
        CHECK(((uintptr_t)r % SPSC_CACHE_LINE) == 0);
        spsc_free(r);
    }
    spsc_free(NULL);
    /* head, tail and the read-only fields are on different cache lines */
    SpscRing probe;
    uintptr_t h = (uintptr_t)&probe.head, t = (uintptr_t)&probe.tail;
    uintptr_t c = (uintptr_t)&probe.capacity;
    CHECK(h / SPSC_CACHE_LINE != t / SPSC_CACHE_LINE);
    CHECK(t / SPSC_CACHE_LINE != c / SPSC_CACHE_LINE);
}

static void test_fifo_full_empty(void)
{
    SpscRing *r = spsc_new(4);
    void *out = num(999);
    CHECK(!spsc_try_pop(r, &out));
    CHECK(out == num(999));                     /* untouched on failure */
    CHECK_EQ_INT(spsc_size(r), 0);

    for (uint64_t i = 1; i <= 4; i++)
        CHECK(spsc_try_push(r, num(i)));
    CHECK_EQ_INT(spsc_size(r), 4);
    CHECK(!spsc_try_push(r, num(5)));           /* full */
    CHECK_EQ_INT(spsc_size(r), 4);

    CHECK(spsc_try_pop(r, &out));
    CHECK(out == num(1));
    CHECK(spsc_try_push(r, num(5)));            /* one slot freed */
    for (uint64_t i = 2; i <= 5; i++) {
        CHECK(spsc_try_pop(r, &out));
        CHECK(out == num(i));
    }
    CHECK(!spsc_try_pop(r, &out));

    CHECK(spsc_try_push(r, NULL));              /* NULL is a valid item */
    out = num(7);
    CHECK(spsc_try_pop(r, &out));
    CHECK(out == NULL);
    spsc_free(r);

    /* capacity 1: alternate full and empty */
    r = spsc_new(1);
    for (uint64_t i = 0; i < 100; i++) {
        CHECK(spsc_try_push(r, num(i)));
        CHECK(!spsc_try_push(r, num(i)));
        CHECK(spsc_try_pop(r, &out));
        CHECK(out == num(i));
        CHECK(!spsc_try_pop(r, &out));
    }
    spsc_free(r);
}

/* The indices only grow; they must keep working when they wrap past SIZE_MAX. */
static void test_index_wraparound(void)
{
    SpscRing *r = spsc_new(8);
    atomic_store(&r->head, SIZE_MAX - 5);
    atomic_store(&r->tail, SIZE_MAX - 5);
    CHECK_EQ_INT(spsc_size(r), 0);
    void *out;
    uint64_t next_out = 0;
    for (uint64_t i = 0; i < 40; i++) {
        CHECK(spsc_try_push(r, num(i)));
        if (i % 3 == 2) {                        /* keep 1..8 items in flight */
            while (spsc_size(r) > 1) {
                CHECK(spsc_try_pop(r, &out));
                CHECK(out == num(next_out));
                next_out++;
            }
        }
        CHECK(spsc_size(r) <= 8);
    }
    while (spsc_try_pop(r, &out)) {
        CHECK(out == num(next_out));
        next_out++;
    }
    CHECK_EQ_INT(next_out, 40);
    CHECK(atomic_load(&r->tail) < 100);           /* really wrapped */
    spsc_free(r);
}

/* ---- the stop flag of the blocking calls ---- */

typedef struct {
    SpscRing   *r;
    atomic_bool stop;
    bool        result;
} StopArg;

static void *blocked_pop(void *p)
{
    StopArg *a = p;
    void *out;
    a->result = spsc_pop(a->r, &out, &a->stop);
    return NULL;
}

static void test_stop(void)
{
    SpscRing *r = spsc_new(2);
    atomic_bool stop;
    atomic_init(&stop, true);
    void *out;
    CHECK(!spsc_pop(r, &out, &stop));             /* empty + stop: give up */
    CHECK(spsc_push(r, num(1), &stop));           /* room: succeeds despite stop */
    CHECK(spsc_push(r, num(2), &stop));
    CHECK(!spsc_push(r, num(3), &stop));          /* full + stop: give up */
    CHECK(spsc_pop(r, &out, &stop));
    CHECK(out == num(1));
    CHECK(spsc_pop(r, &out, NULL));
    CHECK(out == num(2));

    /* a thread waiting on an empty ring wakes up when stop is raised */
    StopArg a = { .r = r, .result = true };
    atomic_init(&a.stop, false);
    pthread_t th;
    CHECK(pthread_create(&th, NULL, blocked_pop, &a) == 0);
    struct timespec ts = { 0, 20 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    atomic_store(&a.stop, true);
    pthread_join(th, NULL);
    CHECK(!a.result);
    spsc_free(r);
}

/* ---- stress: numbers in order, checksum ---- */

typedef struct {
    SpscRing *r;
    uint64_t  n;
    uint64_t  sum;          /* consumer: sum of received values */
    uint64_t  errors;       /* consumer: values out of order */
    bool      use_try;      /* exercise try_* in a spin loop instead of the blocking calls */
} NumArg;

static uint64_t value_of(uint64_t i) { return i * 2654435761u + 1; }

static void *num_producer(void *p)
{
    NumArg *a = p;
    for (uint64_t i = 0; i < a->n; i++) {
        if (a->use_try)
            while (!spsc_try_push(a->r, num(value_of(i))))
                ;
        else
            spsc_push(a->r, num(value_of(i)), NULL);
    }
    return NULL;
}

static void *num_consumer(void *p)
{
    NumArg *a = p;
    for (uint64_t i = 0; i < a->n; i++) {
        void *out;
        if (a->use_try)
            while (!spsc_try_pop(a->r, &out))
                ;
        else
            spsc_pop(a->r, &out, NULL);
        uint64_t v = (uint64_t)(uintptr_t)out;
        if (v != value_of(i))
            a->errors++;
        a->sum += v;
    }
    return NULL;
}

static void run_numbers(size_t capacity, uint64_t n, bool use_try)
{
    SpscRing *r = spsc_new(capacity);
    NumArg a = { .r = r, .n = n, .use_try = use_try };
    pthread_t prod, cons;
    CHECK(pthread_create(&cons, NULL, num_consumer, &a) == 0);
    CHECK(pthread_create(&prod, NULL, num_producer, &a) == 0);
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    uint64_t want = 0;
    for (uint64_t i = 0; i < n; i++)
        want += value_of(i);
    CHECK_EQ_INT(a.errors, 0);
    CHECK(a.sum == want);
    CHECK_EQ_INT(spsc_size(r), 0);
    spsc_free(r);
}

/* ---- stress: pointers to non-atomic payloads, recycled through a return ring.
 * This is the batch-pool pattern of T4.2: the producer fills an object and
 * pushes it, the consumer reads it and hands it back on a second ring. If the
 * release/acquire pairing were wrong, TSan reports a race on Item fields. */

typedef struct {
    uint64_t seq;
    uint64_t payload[7];
} Item;

typedef struct {
    SpscRing *work, *ret;
    uint64_t  n;
    uint64_t  sum, errors;
} PoolArg;

static void *pool_producer(void *p)
{
    PoolArg *a = p;
    for (uint64_t i = 0; i < a->n; i++) {
        void *v;
        spsc_pop(a->ret, &v, NULL);             /* a free item */
        Item *it = v;
        it->seq = i;
        for (int k = 0; k < 7; k++)
            it->payload[k] = value_of(i) + (uint64_t)k;
        spsc_push(a->work, it, NULL);
    }
    return NULL;
}

static void *pool_consumer(void *p)
{
    PoolArg *a = p;
    for (uint64_t i = 0; i < a->n; i++) {
        void *v;
        spsc_pop(a->work, &v, NULL);
        Item *it = v;
        if (it->seq != i)
            a->errors++;
        for (int k = 0; k < 7; k++) {
            if (it->payload[k] != value_of(i) + (uint64_t)k)
                a->errors++;
            a->sum += it->payload[k];
        }
        it->seq = UINT64_MAX;                   /* consumer writes too */
        spsc_push(a->ret, it, NULL);
    }
    return NULL;
}

static void test_payload_pool(uint64_t n)
{
    enum { POOL = 8 };
    Item items[POOL];
    PoolArg a = { .work = spsc_new(4), .ret = spsc_new(POOL), .n = n };
    for (int i = 0; i < POOL; i++)
        CHECK(spsc_try_push(a.ret, &items[i]));
    pthread_t prod, cons;
    CHECK(pthread_create(&cons, NULL, pool_consumer, &a) == 0);
    CHECK(pthread_create(&prod, NULL, pool_producer, &a) == 0);
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    uint64_t want = 0;
    for (uint64_t i = 0; i < n; i++)
        for (int k = 0; k < 7; k++)
            want += value_of(i) + (uint64_t)k;
    CHECK_EQ_INT(a.errors, 0);
    CHECK(a.sum == want);
    CHECK_EQ_INT(spsc_size(a.ret), POOL);         /* every item came back */
    spsc_free(a.work);
    spsc_free(a.ret);
}

/* ---- a third thread reading the size while the ring is busy ---- */

typedef struct {
    SpscRing   *r;
    atomic_bool done;
    uint64_t    bad;
} ObsArg;

static void *observer(void *p)
{
    ObsArg *o = p;
    while (!atomic_load(&o->done))
        if (spsc_size(o->r) > spsc_capacity(o->r))
            o->bad++;
    return NULL;
}

static void test_observer(void)
{
    SpscRing *r = spsc_new(16);
    ObsArg o = { .r = r };
    atomic_init(&o.done, false);
    pthread_t obs;
    CHECK(pthread_create(&obs, NULL, observer, &o) == 0);
    NumArg a = { .r = r, .n = 1000000 };
    pthread_t prod, cons;
    CHECK(pthread_create(&cons, NULL, num_consumer, &a) == 0);
    CHECK(pthread_create(&prod, NULL, num_producer, &a) == 0);
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);
    atomic_store(&o.done, true);
    pthread_join(obs, NULL);
    CHECK_EQ_INT(a.errors, 0);
    CHECK_EQ_INT(o.bad, 0);
    spsc_free(r);
}

int main(void)
{
    test_capacity();
    test_fifo_full_empty();
    test_index_wraparound();
    test_stop();
    run_numbers(64, STRESS_ITEMS, false);       /* the plan's 10M-item stress */
    run_numbers(1, 200000, false);              /* every op hits full or empty */
    run_numbers(256, 2000000, true);            /* non-blocking calls, spinning */
    test_payload_pool(2000000);
    test_observer();
    return TEST_DONE();
}
