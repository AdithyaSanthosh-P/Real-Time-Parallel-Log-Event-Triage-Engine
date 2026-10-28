#ifndef ENGINE_H
#define ENGINE_H

/* Uniform handle over the matching engines (spec §2.1), in two layers:
 *
 *   EnginePrepared  built ONCE before the workers start, read-only after:
 *                   for `dfa` the compiled DFA tables (shared by all workers
 *                   without locks); for `posix` just the rule set.
 *   Engine          one per worker thread, created from the prepared state:
 *                   for `posix` it compiles that worker's own regex_t set
 *                   (glibc's regexec locks a shared regex_t); for `dfa` it
 *                   only points at the shared tables. */

#include <stddef.h>
#include <stdint.h>

#include "rules.h"

typedef enum { ENGINE_DFA, ENGINE_POSIX, ENGINE_FORK } EngineKind;

const char *engine_kind_name(EngineKind kind);

typedef struct EnginePrepared EnginePrepared;
typedef struct Engine Engine;

/* Compile the rule set for `kind`. `max_states` is the DFA state cap
 * (<= 0: default). `rs` must outlive the result. NULL + `err` on failure,
 * including for engines that are not implemented yet. */
EnginePrepared *engine_prepare(EngineKind kind, const RuleSet *rs, int max_states,
                               char *err, size_t err_len);

/* One line about what was built, e.g. "dfa: 2 groups, 7593 states, 613.2 ms". */
void engine_describe(const EnginePrepared *p, char *buf, size_t len);

/* Size of what was built, for --bench: all zero for engines without a DFA. */
void engine_dfa_info(const EnginePrepared *p, int *nfa_states, int *groups, long long *dfa_states,
                     double *build_ms);

void engine_prepared_free(EnginePrepared *p);

/* A per-thread matcher. NULL + `err` on failure. */
Engine *engine_new(const EnginePrepared *p, char *err, size_t err_len);

/* Rule mask of one line of normalized text; bit r = rule r matched. */
uint64_t engine_match(Engine *e, const char *line, size_t len);

void engine_free(Engine *e);

#endif /* ENGINE_H */
