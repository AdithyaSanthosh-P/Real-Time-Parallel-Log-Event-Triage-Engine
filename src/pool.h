#ifndef POOL_H
#define POOL_H

/* Job pool for the parallel pipeline (spec §2.3, plan T4.2).
 *
 * A Job is the unit that travels through the pipeline: a Batch of input lines
 * (spec §2.4) together with everything later stages compute for those lines.
 * One object flows reader -> worker -> aggregator -> router and is then handed
 * back to the reader on the pool's free ring (router -> reader, SPSC), so in
 * steady state nothing is allocated: the jobs are created once, and their
 * growable buffers (line text, masked text) stop growing once they have seen
 * the largest batch of the input.
 *
 * Ownership is exclusive: at any moment a job belongs to exactly one stage,
 * and moving it through an SpscRing is what transfers it (the ring's
 * release/acquire pairing makes the previous owner's writes visible). */

#include <stddef.h>
#include <stdint.h>

#include "json.h"
#include "spsc.h"
#include "types.h"

/* Per-line results, filled by the worker (m, subsumed, masked text) and the
 * aggregator (c). mask_len[i] == MASK_NONE means line i is written as is. */
#define MASK_NONE UINT32_MAX

typedef struct {
    MatchResult m[BATCH_MAX];
    uint64_t    subsumed[BATCH_MAX];     /* rules to drop from the explanation (§5.3) */
    Classified  c[BATCH_MAX];
    uint32_t    mask_off[BATCH_MAX];     /* masked line i = masked.data[mask_off .. +mask_len) */
    uint32_t    mask_len[BATCH_MAX];
    StrBuf      masked;
} ResultBatch;

typedef struct {
    Batch       batch;
    ResultBatch res;
} Job;

typedef struct {
    Job      *jobs;
    size_t    n;
    SpscRing *free_ring;     /* producer: router; consumer: reader */
} Pool;

/* Allocate n jobs and put all of them on the free ring. 0, or -1 on out of
 * memory (nothing left allocated). Call before the pipeline threads start. */
int  pool_init(Pool *p, size_t n);
void pool_free(Pool *p);

/* Pool size for `workers` workers with rings of `depth` items: enough to keep
 * every worker ring full while one job is in each other stage and a few are
 * in the result rings. */
size_t pool_size_for(int workers, size_t depth);

#endif /* POOL_H */
