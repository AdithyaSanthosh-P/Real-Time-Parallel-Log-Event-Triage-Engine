#ifndef WORKER_H
#define WORKER_H

/* Workers (spec §2.2, plan T4.3). Each worker is one pthread with its own
 * matcher state and two SPSC rings:
 *
 *   in   dispatcher -> worker   Jobs whose Batch is filled
 *   out  worker -> aggregator   the same Jobs with per-line results added
 *
 * Per line the worker runs the engine (DFA: shared read-only tables; POSIX:
 * this worker's own regex_t set) and, on matched lines only, the post-match
 * steps of §3.4: validators and entity extraction, context keywords,
 * subsumption, and PII masking of the output text (§7). Classification
 * (§5) is left to the aggregator thread.
 *
 * The end of input is a sentinel: the dispatcher pushes WORKER_EOF on every
 * worker's in-ring; the worker forwards it on its out-ring and exits. */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "engine.h"
#include "extract.h"
#include "json.h"
#include "pool.h"
#include "rules.h"
#include "spsc.h"

/* The end-of-input marker (never dereferenced). */
extern Job *const WORKER_EOF;

typedef struct {
    int            id;
    int            cpu;              /* CPU to pin to, -1 = not pinned */
    const RuleSet *rs;
    Engine        *engine;
    Extractor     *extractor;
    StrBuf         scratch;          /* mask_pii output before it is copied into the job */
    SpscRing      *in, *out;
    atomic_bool   *stop;             /* set by any stage that fails: give up waiting */
    pthread_t      thread;
    bool           started;
    bool           pin_failed;       /* written by the worker, read after join */
} Worker;

/* Create the per-worker state (own engine instance and capture regexes) and
 * rings with `depth` slots. NULL-safe free. 0, or -1 with `err` filled. */
int  worker_init(Worker *w, int id, const EnginePrepared *ep, const RuleSet *rs,
                 size_t depth, atomic_bool *stop, char *err, size_t err_len);
void worker_destroy(Worker *w);

/* Start the thread (pinned to w->cpu if >= 0). 0, or an errno value. */
int  worker_start(Worker *w);
void worker_join(Worker *w);

/* What the thread does with one job; exposed for tests. */
void worker_process(Worker *w, Job *job);

/* CPUs to pin workers to, in preference order: the allowed CPUs
 * (sched_getaffinity), first one logical CPU of each physical core, then the
 * remaining SMT siblings, so N workers use N cores before sharing any.
 * Without topology information the allowed CPUs are used in numeric order.
 * Returns the number of CPUs written to `cpus` (at most `max`), 0 on failure. */
int  worker_cpu_order(int *cpus, int max);

#endif /* WORKER_H */
