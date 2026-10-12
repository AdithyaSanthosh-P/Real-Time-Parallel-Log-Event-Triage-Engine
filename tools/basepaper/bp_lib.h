#ifndef BP_LIB_H
#define BP_LIB_H

/* Faithful reproduction of the base paper's program (docs/base_paper.md §3.2,
 * §3.3), shared by bp_fork (R1) and bp_bench (R2).
 *
 * "Faithful, not fixed" (CLAUDE.md): per input this forks one child per
 * pattern, and every child runs regcomp -> regexec -> regfree. Nothing here
 * is optimised on purpose. */

#include <stdint.h>

#include "patterns.h"

/* What a child sends back through the pipe. The timings are instrumentation
 * we added; the paper's record has {category, matched, pattern, timestamp}. */
typedef struct {
    int32_t idx;              /* pattern index = "core" in the paper's screenshots */
    int32_t matched;          /* 1 if regexec returned 0; 0 otherwise (also on regcomp failure) */
    int64_t t_regcomp_ns;
    int64_t t_regexec_ns;
} BpRecord;

/* Called once per record as the parent receives it (arrival order). */
typedef void (*BpRecordFn)(const BpRecord *rec, void *arg);

/* parallel_classify() of paper §3.3. `text` is NUL-terminated. Records are
 * delivered in arrival order through `on_record` (may be NULL) and the match
 * mask (bit i set = pattern i matched) is returned in *mask. *total_ns, if
 * not NULL, receives the parent's time from the first fork to the last
 * waitpid. Returns the number of records received (BP_NPAT when nothing
 * failed; fewer if a fork failed or a child died without reporting). */
int bp_classify_fork(const char *text, BpRecordFn on_record, void *arg,
                     unsigned *mask, int64_t *total_ns);

/* Paper §3.2 process_input() steps 1-3 exactly as written. Writes the
 * normalised text to `out` (capacity >= BP_MAX_INPUT_LENGTH + 1) and returns 0,
 * or returns -1 and sets *err to the paper's error message. */
#define BP_MAX_INPUT_LENGTH 256
int bp_normalize_paper(const char *in, char *out, const char **err);

/* CPUs this process may run on (honours taskset), ascending; returns how many
 * were stored (at most `cap`). bp_pin_to_cpu() pins the calling thread/process
 * to one CPU (best effort). The variants pin "worker i" to list[i % n]. */
int  bp_allowed_cpus(int *list, int cap);
void bp_pin_to_cpu(int cpu);

/* Monotonic clock in nanoseconds. */
int64_t bp_now_ns(void);

#endif /* BP_LIB_H */
