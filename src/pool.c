#include "pool.h"

#include <stdlib.h>

#include "batch.h"

/* Slack beyond workers x depth: one job in the reader, one per worker being
 * processed, one in the aggregator, one in the router, and a few in transit
 * on the result rings, so the reader rarely waits for a free job while the
 * worker rings have room. */
#define POOL_SLACK 4

/* Initial room for a job's masked text (only PII lines add to it), so the
 * buffer does not have to grow from empty in every job. */
#define MASKED_INITIAL (64u * 1024u)

size_t pool_size_for(int workers, size_t depth)
{
    return (size_t)workers * depth + (size_t)workers + POOL_SLACK;
}

int pool_init(Pool *p, size_t n)
{
    p->n = 0;
    p->jobs = calloc(n, sizeof *p->jobs);
    p->free_ring = spsc_new(n);
    if (!p->jobs || !p->free_ring) {
        pool_free(p);
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        if (batch_init(&p->jobs[i].batch) != 0) {
            pool_free(p);
            return -1;
        }
        sb_init(&p->jobs[i].res.masked);
        p->n = i + 1;
        if (!sb_reserve(&p->jobs[i].res.masked, MASKED_INITIAL)) {
            pool_free(p);
            return -1;
        }
        /* the ring's capacity is >= n, so this cannot fail */
        spsc_try_push(p->free_ring, &p->jobs[i]);
    }
    return 0;
}

void pool_free(Pool *p)
{
    for (size_t i = 0; i < p->n; i++) {
        batch_free(&p->jobs[i].batch);
        sb_free(&p->jobs[i].res.masked);
    }
    free(p->jobs);
    spsc_free(p->free_ring);
    p->jobs = NULL;
    p->free_ring = NULL;
    p->n = 0;
}
