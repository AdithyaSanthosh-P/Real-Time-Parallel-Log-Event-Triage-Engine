#ifndef DISPATCH_H
#define DISPATCH_H

/* Dispatcher (spec §2.3, plan T4.4): hands filled jobs to the workers' input
 * rings. Runs on the reader thread.
 *
 *   rr     round-robin over the workers (default)
 *   least  the worker whose input ring holds the fewest jobs (ties: the next
 *          one after the previous choice, so equal rings still rotate)
 *
 * Backpressure: when the chosen ring is full the dispatcher waits, which
 * stops the reader, so no line is lost. With `drop` it does not wait: the job
 * is not dispatched, its lines are counted as dropped and the caller reuses
 * the job. */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "cli.h"
#include "pool.h"
#include "spsc.h"

typedef struct {
    SpscRing   **rings;          /* one input ring per worker (not owned) */
    int          n;
    DispatchKind policy;
    bool         drop;
    int          next;           /* rr: next worker; least: where the tie scan starts */
    atomic_uint_fast64_t dropped_lines;   /* written by the dispatcher, read by stats */
    atomic_uint_fast64_t dropped_batches;
    atomic_bool *stop;
} Dispatcher;

typedef enum { DISPATCH_SENT, DISPATCH_DROPPED, DISPATCH_STOPPED } DispatchResult;

void dispatch_init(Dispatcher *d, SpscRing **rings, int n, DispatchKind policy, bool drop,
                   atomic_bool *stop);

/* Choose a worker for the next job (exposed for tests). */
int dispatch_pick(Dispatcher *d);

/* Send one job. DISPATCH_DROPPED (only with drop) means the caller still owns
 * the job; DISPATCH_STOPPED means `stop` was raised while waiting. */
DispatchResult dispatch_job(Dispatcher *d, Job *job);

/* End of input: push `eof` on every worker's ring (waits even in drop mode,
 * so every worker sees it). false if `stop` was raised while waiting. */
bool dispatch_eof(Dispatcher *d, Job *eof);

#endif /* DISPATCH_H */
