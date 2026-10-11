#ifndef DFA_H
#define DFA_H

/* Combined multi-pattern DFA built by subset construction (spec §3.2 steps
 * 4 and 6) and matched with the loop of §3.3.
 *
 * Tables of one DFA (read-only after build, shared by all workers):
 *   next[s * 256 + byte]   next state
 *   accept_mask[s]         rules that have matched once state s is reached
 *   accept_end_mask[s]     rules that match if the line ENDS in state s
 *                          (they need '$', or would match the empty line)
 * State 0 is the start state. Masks use the global rule numbering, so the
 * masks of several group DFAs can simply be OR-ed together. */

#include <stddef.h>
#include <stdint.h>

#include "nfa.h"

#define DFA_DEFAULT_MAX_STATES 20000

typedef struct {
    int32_t   nstates;
    int32_t  *next;
    uint64_t *accept_mask;
    uint64_t *accept_end_mask;
    uint64_t  rules;            /* rule bits this DFA covers */
} Dfa;

/* All rules, possibly split into groups when one DFA would exceed the cap. */
typedef struct {
    int     ngroups;
    Dfa    *groups;
    int64_t total_states;
    double  build_ms;
} DfaSet;

/* Build one DFA for the rules in `rule_mask` of `nfa`.
 * Returns 0; DFA_TOO_BIG if it would need more than `max_states` states
 * (nothing is kept); -1 on out of memory. */
#define DFA_TOO_BIG 1
int  dfa_build(const Nfa *nfa, uint64_t rule_mask, int max_states, Dfa *out);
void dfa_free(Dfa *d);

/* Build DFAs for all rules of `nfa`. If a group exceeds `max_states`
 * (<= 0 means DFA_DEFAULT_MAX_STATES) it is halved and each half built
 * separately, recursively. Fails only if a single rule alone exceeds the
 * cap or memory runs out; then returns -1 with a message in `err`. */
int  dfaset_build(const Nfa *nfa, int max_states, DfaSet *out, char *err, size_t err_len);
void dfaset_free(DfaSet *ds);

/* §3.3 match loop for one DFA. */
uint64_t dfa_match(const Dfa *d, const char *s, uint32_t n);

/* OR of dfa_match over every group. */
uint64_t dfaset_match(const DfaSet *ds, const char *s, size_t n);

#endif /* DFA_H */
