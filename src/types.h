#ifndef TYPES_H
#define TYPES_H

/* Shared enums from spec §2.4. Rule bit r in every uint64_t mask is the
 * index of rule r in the loaded RuleSet. */

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

#endif /* TYPES_H */
