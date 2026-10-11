#ifndef DOT_H
#define DOT_H

/* Graphviz export of the automata (spec §3.2 step 7, `--dump-dot FILE`).
 *
 * One digraph with a cluster for the combined NFA and one per DFA group.
 * Edge labels are charsets collapsed into ranges ('a', [0-9a-f], [^\x20]);
 * epsilon edges are labelled "ε", assertion edges "^" and "$". DFA edges
 * with the same source and target are merged into one edge whose label is
 * the union of their bytes. Accepting states are double circles labelled
 * with the rule ids (end-of-line-only accepts are prefixed with "$:").
 * Meant for small rule sets (report figures); large ones produce huge graphs. */

#include <stdio.h>

#include "dfa.h"
#include "nfa.h"
#include "rules.h"

/* Write the NFA and every DFA group of `ds` (ds may be NULL: NFA only).
 * Returns 0, or -1 if a write failed. */
int dot_write(FILE *f, const RuleSet *rs, const Nfa *nfa, const DfaSet *ds);

#endif /* DOT_H */
