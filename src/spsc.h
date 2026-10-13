#ifndef SPSC_H
#define SPSC_H

/* Bounded single-producer / single-consumer ring of pointers (spec §2.3).
 * One ring connects the dispatcher to each worker and each worker to the
 * aggregator; the items are Batch* / result-batch pointers.
 *
 * Exactly one thread may push and exactly one thread may pop on a given ring.
 * `head` is the consumer's index (next slot to pop), `tail` the producer's
 * (next slot to fill); both only grow and are reduced modulo the capacity, so
 * occupancy is tail - head (unsigned arithmetic stays correct across
 * wraparound). The producer fills a slot and then publishes it with a release
 * store of `tail`; the consumer's acquire load of `tail` therefore also sees
 * everything the producer wrote into the item before pushing it. Symmetrically
 * the consumer releases `head` after reading a slot, so the producer never
 * overwrites a slot that is still being read. No locks, no read-modify-write
 * atomics: each index has a single writer.
 *
 * head and tail sit on separate 64-byte cache lines (no false sharing between
 * producer and consumer); the read-only capacity/slots pair has its own line. */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#define SPSC_CACHE_LINE 64

typedef struct {
    _Alignas(SPSC_CACHE_LINE) atomic_size_t head;   /* written by the consumer only */
    _Alignas(SPSC_CACHE_LINE) atomic_size_t tail;   /* written by the producer only */
    _Alignas(SPSC_CACHE_LINE) size_t capacity;      /* power of two */
    void **slots;
} SpscRing;

/* Allocate a ring (cache-line aligned) with room for `capacity` items,
 * rounded up to a power of two (minimum 1). Returns NULL on out of memory or
 * if capacity is 0 or too large. Free with spsc_free (NULL is fine). */
SpscRing *spsc_new(size_t capacity);
void      spsc_free(SpscRing *r);

/* Non-blocking. try_push returns false if the ring is full (item not stored);
 * try_pop returns false if it is empty (*out untouched). Any pointer value,
 * NULL included, can be an item. */
bool spsc_try_push(SpscRing *r, void *item);
bool spsc_try_pop(SpscRing *r, void **out);

/* Blocking variants: retry with a short busy spin, then sched_yield, then
 * nanosleep with a growing pause (capped at SPSC_MAX_SLEEP_NS) until the
 * operation succeeds. If `stop` is not NULL and becomes true while waiting,
 * they give up and return false (used to unblock threads when another stage
 * fails); otherwise they return true. */
#define SPSC_MAX_SLEEP_NS 50000L
bool spsc_push(SpscRing *r, void *item, const atomic_bool *stop);
bool spsc_pop(SpscRing *r, void **out, const atomic_bool *stop);

/* Number of items in the ring. Exact when called by the producer or the
 * consumer for its own side's decisions; from any other thread (dispatch
 * policy `least` reads worker rings it does not consume, the stats line reads
 * everything) it is a snapshot that may be stale by the time it is used. */
size_t spsc_size(const SpscRing *r);
static inline size_t spsc_capacity(const SpscRing *r) { return r->capacity; }

#endif /* SPSC_H */
