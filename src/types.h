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

/* Classified.flags bits (spec §6): set by the correlator when it escalates. */
#define FLAG_BRUTE_FORCE       (1u << 0)
#define FLAG_PASSWORD_SPRAY    (1u << 1)
#define FLAG_FAIL_THEN_SUCCESS (1u << 2)
#define FLAG_ERROR_STORM       (1u << 3)

/* Aggregator output for one line (spec §2.4). `primary` is a category INDEX
 * (0 = AUTH ... 4 = PII, see aggregate.h), or PRIMARY_NONE when the line has
 * no label. */
#define PRIMARY_NONE 0xFF

typedef struct {
    uint64_t seq;
    uint32_t label_mask;             /* CAT_* bits, multi-label */
    uint8_t  primary;                /* CAT index */
    uint8_t  severity;               /* SEV_* */
    float    confidence;             /* of primary */
    uint64_t rule_mask;              /* for explainability */
    uint32_t flags;                  /* e.g. ESCALATED_BRUTE_FORCE */
} Classified;

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
