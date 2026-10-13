#ifndef STATS_H
#define STATS_H

/* Pipeline counters and the 1 Hz stats line on stderr (spec §7, plan T4.5):
 *
 *   triage: 812345 lines/s | queues 3 0 1 2 | AUTH 10 SEC 2 ERR 5 PERF 0 PII 3 | dropped 0
 *
 * lines/s is the router's output rate over the last interval; `queues` the
 * jobs waiting in each worker's input ring; the category counts are lines
 * labelled with that category so far; `dropped` the lines discarded by
 * --drop so far. Every counter has one writing thread (relaxed atomics; the
 * reporter only needs eventually-consistent snapshots). */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "aggregate.h"
#include "json.h"
#include "spsc.h"

typedef struct {
    atomic_uint_fast64_t lines_out;             /* router */
    atomic_uint_fast64_t cat[N_CATEGORIES];     /* router: lines with that label */
} Stats;

void stats_init(Stats *s);

/* Router side: one line written with label mask `label_mask` (CAT_* bits). */
void stats_count_line(Stats *s, uint32_t label_mask);

/* A snapshot of everything the line shows. */
typedef struct {
    uint64_t lines_out;
    uint64_t cat[N_CATEGORIES];
    uint64_t dropped;
    int      nqueues;
    size_t   queue[256];
} StatsSnapshot;

/* Format one stats line (no newline) from two snapshots `secs` apart. */
void stats_format(const StatsSnapshot *prev, const StatsSnapshot *now, double secs, StrBuf *out);

/* Background thread printing the line every `interval_ms` until stopped. */
typedef struct {
    Stats                *stats;
    SpscRing *const      *queues;   /* worker input rings (read-only use: spsc_size) */
    int                   nqueues;
    atomic_uint_fast64_t *dropped;  /* the dispatcher's dropped-lines counter */
    FILE                 *out;
    long                  interval_ms;
    atomic_bool           done;
    pthread_t             thread;
    bool                  started;
} StatsReporter;

int  stats_reporter_start(StatsReporter *r);    /* 0 or an errno value */
void stats_reporter_stop(StatsReporter *r);     /* NULL-safe; joins */

#endif /* STATS_H */
