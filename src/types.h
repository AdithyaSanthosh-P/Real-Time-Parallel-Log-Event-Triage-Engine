#ifndef TYPES_H
#define TYPES_H

/* Shared types from spec §2.4. Rule bit r in every uint64_t mask is the
 * index of rule r in the loaded RuleSet. */

#include <stddef.h>
#include <stdint.h>

typedef enum {
    CAT_AUTH = 1 << 0,
    CAT_SEC  = 1 << 1,
    CAT_ERR  = 1 << 2,
    CAT_PERF = 1 << 3,
    CAT_PII  = 1 << 4
} Category;

typedef enum {
    SEV_INFO,
    SEV_LOW,
    SEV_MEDIUM,
    SEV_HIGH,
    SEV_CRITICAL
} Severity;

/* Most lines a Batch can hold; `-b` selects the actual batch size (default 256). */
#define BATCH_MAX 1024

/* A batch of lines in one shared buffer (spec §2.4). Line i is
 * raw[off[i] .. off[i+1]-1] and its match-normalized copy is
 * norm[moff[i] .. moff[i+1]-1]; there are no separators and no NULs between
 * lines. raw_cap/norm_cap are capacities (implementation detail added to the
 * spec's struct; the buffers grow so a line of any length fits). */
typedef struct {
    uint64_t seq_start;
    uint32_t count;
    uint32_t off[BATCH_MAX + 1];
    uint32_t moff[BATCH_MAX + 1];
    int64_t  ts[BATCH_MAX];           /* parsed log timestamp (epoch s) or ingest time */
    uint64_t ingest_ns[BATCH_MAX];    /* CLOCK_MONOTONIC, for latency */
    char    *raw;                     /* original bytes (for output) */
    char    *norm;                    /* URL-decoded / control-stripped (for matching) */
    size_t   raw_cap;
    size_t   norm_cap;
} Batch;

#endif /* TYPES_H */
