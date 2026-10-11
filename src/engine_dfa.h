#ifndef ENGINE_DFA_H
#define ENGINE_DFA_H

/* The `dfa` engine (spec §2.1, §3.3): the rule set compiled by our own
 * compiler (regex_parse -> nfa -> dfa) into one or more combined DFAs.
 * The tables are built once and are read-only afterwards, so every worker
 * can match against the same EngineDfa without locks. */

#include <stddef.h>
#include <stdint.h>

#include "dfa.h"
#include "rules.h"

typedef struct {
    DfaSet dfas;
    int    nfa_states;      /* size of the combined NFA it was built from */
} EngineDfa;

/* Compile `rs`. max_states <= 0 means DFA_DEFAULT_MAX_STATES. Returns NULL
 * with a message in `err` on failure (unsupported regex syntax, a rule too
 * big for the cap, out of memory). */
EngineDfa *dfa_engine_build(const RuleSet *rs, int max_states, char *err, size_t err_len);

/* Rule mask of one normalized line: the §3.3 loop over every group. */
uint64_t dfa_engine_match(const EngineDfa *e, const char *line, size_t len);

void dfa_engine_free(EngineDfa *e);

#endif /* ENGINE_DFA_H */
