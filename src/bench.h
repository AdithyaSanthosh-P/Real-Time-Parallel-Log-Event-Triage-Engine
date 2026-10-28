#ifndef BENCH_H
#define BENCH_H

/* `--bench` support (spec §8, §10.2): preloading the input into memory, a
 * log-scale latency histogram, and the one-line JSON result. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Read all of `path` ("-" or NULL = stdin) into an anonymous in-memory file
 * and return a descriptor positioned at its start (the Reader reads it like
 * any file), so the timed run never touches the disk. `*bytes` is the input
 * size. -1 with errno set on failure. */
int bench_preload(const char *path, size_t *bytes);

/* Latency histogram with 8 sub-buckets per power of two (about 9 % relative
 * resolution) from 1 ns up to ~18 s; larger values land in the last bucket.
 * One writer thread; read after that thread has been joined. */
#define BENCH_HIST_BUCKETS 512

typedef struct {
    uint64_t count;
    uint64_t max_ns;
    uint64_t bucket[BENCH_HIST_BUCKETS];
} BenchHist;

void     bench_hist_add(BenchHist *h, uint64_t ns);

/* Upper edge, in ns, of the bucket holding the q-quantile (0 < q <= 1); 0 when empty. */
uint64_t bench_hist_quantile(const BenchHist *h, double q);

typedef struct {
    const char *engine;
    int         workers, batch;
    uint64_t    lines, bytes;
    double      seconds;
    int         nfa_states, dfa_groups;      /* 0 for engines without a DFA */
    long long   dfa_states;
    double      build_ms;
    const BenchHist *latency;                /* may be NULL */
} BenchResult;

/* One JSON object, one line:
 * {"engine":"dfa","workers":4,"batch":256,"lines":N,"bytes":N,"seconds":S,
 *  "lines_per_s":X,"mb_per_s":X,"latency_us":{"p50":..,"p90":..,"p99":..,"p99.9":..,"max":..},
 *  "nfa_states":N,"dfa_groups":N,"dfa_states":N,"build_ms":X} */
void bench_print_json(FILE *out, const BenchResult *r);

#endif /* BENCH_H */
