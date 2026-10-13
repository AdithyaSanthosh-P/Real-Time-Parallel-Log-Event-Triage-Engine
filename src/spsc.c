#include "spsc.h"

#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

SpscRing *spsc_new(size_t capacity)
{
    if (capacity == 0 || capacity > (SIZE_MAX / sizeof(void *)) / 2)
        return NULL;
    size_t cap = 1;
    while (cap < capacity)
        cap <<= 1;

    /* aligned_alloc needs a size that is a multiple of the alignment;
     * sizeof(SpscRing) is, because of its _Alignas members. */
    SpscRing *r = aligned_alloc(SPSC_CACHE_LINE, sizeof *r);
    if (!r)
        return NULL;
    r->slots = calloc(cap, sizeof *r->slots);
    if (!r->slots) {
        free(r);
        return NULL;
    }
    r->capacity = cap;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    return r;
}

void spsc_free(SpscRing *r)
{
    if (!r)
        return;
    free(r->slots);
    free(r);
}

bool spsc_try_push(SpscRing *r, void *item)
{
    /* Our own index: only this thread writes it, so relaxed is enough. */
    size_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    /* Acquire pairs with the consumer's release of head: the slot we are about
     * to reuse has been read completely. */
    size_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    if (t - h == r->capacity)
        return false;                                   /* full */
    r->slots[t & (r->capacity - 1)] = item;
    /* Release publishes the slot (and whatever `item` points to). */
    atomic_store_explicit(&r->tail, t + 1, memory_order_release);
    return true;
}

bool spsc_try_pop(SpscRing *r, void **out)
{
    size_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    /* Acquire pairs with the producer's release of tail. */
    size_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (h == t)
        return false;                                   /* empty */
    *out = r->slots[h & (r->capacity - 1)];
    /* Release hands the slot back to the producer. */
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
    return true;
}

size_t spsc_size(const SpscRing *r)
{
    /* Read head before tail: tail never decreases, so tail - head >= 0. From a
     * third thread the consumer may have advanced head after we read it, which
     * can make the difference exceed the capacity; clamp. */
    size_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    size_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    size_t n = t - h;
    return n > r->capacity ? r->capacity : n;
}

/* ---- waiting: spin, then yield, then sleep with exponential growth ---- */

#define SPIN_ROUNDS  64
#define YIELD_ROUNDS 16

typedef struct {
    unsigned rounds;
    long     sleep_ns;
} Backoff;

static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

static void backoff_wait(Backoff *b)
{
    if (b->rounds < SPIN_ROUNDS) {
        cpu_relax();
    } else if (b->rounds < SPIN_ROUNDS + YIELD_ROUNDS) {
        sched_yield();
    } else {
        b->sleep_ns = b->sleep_ns ? b->sleep_ns * 2 : 1000;
        if (b->sleep_ns > SPSC_MAX_SLEEP_NS)
            b->sleep_ns = SPSC_MAX_SLEEP_NS;
        struct timespec ts = { 0, b->sleep_ns };
        nanosleep(&ts, NULL);
    }
    b->rounds++;
}

static inline bool stopped(const atomic_bool *stop)
{
    return stop && atomic_load_explicit(stop, memory_order_acquire);
}

bool spsc_push(SpscRing *r, void *item, const atomic_bool *stop)
{
    Backoff b = { 0, 0 };
    while (!spsc_try_push(r, item)) {
        if (stopped(stop))
            return false;
        backoff_wait(&b);
    }
    return true;
}

bool spsc_pop(SpscRing *r, void **out, const atomic_bool *stop)
{
    Backoff b = { 0, 0 };
    while (!spsc_try_pop(r, out)) {
        if (stopped(stop))
            return false;
        backoff_wait(&b);
    }
    return true;
}
