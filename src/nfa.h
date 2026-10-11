#ifndef NFA_H
#define NFA_H

/* Thompson NFA (spec §3.2 steps 2-3).
 *
 * Every state has either one charset edge, or at most two epsilon edges, or
 * one assertion edge, or is an accept state:
 *   NFA_CHAR    consume one byte in `set`, go to out1
 *   NFA_EPS     epsilon edges to out1 and/or out2 (-1 = none)
 *   NFA_BOL     epsilon edge to out1, usable only at offset 0      ('^')
 *   NFA_EOL     epsilon edge to out1, usable only at the line end  ('$')
 *   NFA_ACCEPT  accept state of rule `rule`
 *
 * Each rule gets its own fragment ending in an NFA_ACCEPT tagged with the
 * rule's index; the combined NFA has a fresh start state with epsilon edges
 * to every rule's start (step 3). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ast.h"
#include "rules.h"

typedef enum { NFA_CHAR, NFA_EPS, NFA_BOL, NFA_EOL, NFA_ACCEPT } NfaKind;

typedef struct {
    uint8_t  kind;      /* NfaKind */
    int32_t  out1;
    int32_t  out2;
    int32_t  rule;      /* NFA_ACCEPT: rule index */
    CharSet  set;       /* NFA_CHAR */
} NfaState;

typedef struct {
    NfaState *st;
    int       n, cap;
    int       start;                        /* combined start (after nfa_finish) */
    int       nrules;
    int       rule_start[RULES_MAX];
    int       rule_accept[RULES_MAX];
    bool      rule_anchored[RULES_MAX];     /* every match must begin at offset 0 */
} Nfa;

void nfa_init(Nfa *nfa);
void nfa_free(Nfa *nfa);

/* Thompson-construct `ast` as rule number `rule` (0..RULES_MAX-1, in order).
 * Returns 0, or -1 on out of memory. */
int  nfa_add_rule(Nfa *nfa, const AstNode *ast, int rule);

/* Add the combined start state with epsilon edges to every rule's start. */
int  nfa_finish(Nfa *nfa);

/* Parse every rule of `rs` (regex_parse) and build the combined NFA.
 * Returns 0, or -1 with "rule ID: offset N: message" in `err`. */
int  nfa_build_rules(Nfa *nfa, const RuleSet *rs, char *err, size_t err_len);

/* Reference matcher: set-of-states simulation of the combined NFA over
 * line[0..len), with the same unanchored semantics as the DFA (§3.2 step 4):
 * every rule may start a match at every offset, '^' only holds at offset 0
 * and '$' only at `len`. Returns the mask of rules that match somewhere. */
uint64_t nfa_simulate(const Nfa *nfa, const char *line, size_t len);

#endif /* NFA_H */
