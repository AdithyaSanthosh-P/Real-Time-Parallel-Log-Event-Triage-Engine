#include "engine_dfa.h"

#include <stdio.h>
#include <stdlib.h>

#include "nfa.h"

EngineDfa *dfa_engine_build(const RuleSet *rs, int max_states, char *err, size_t err_len)
{
    EngineDfa *e = calloc(1, sizeof *e);
    if (!e) {
        snprintf(err, err_len, "out of memory");
        return NULL;
    }
    Nfa nfa;
    if (nfa_build_rules(&nfa, rs, err, err_len) != 0) {
        free(e);
        return NULL;
    }
    e->nfa_states = nfa.n;
    int rc = dfaset_build(&nfa, max_states, &e->dfas, err, err_len);
    nfa_free(&nfa);                     /* the DFA tables are self-contained */
    if (rc != 0) {
        free(e);
        return NULL;
    }
    return e;
}

uint64_t dfa_engine_match(const EngineDfa *e, const char *line, size_t len)
{
    return dfaset_match(&e->dfas, line, len);
}

void dfa_engine_free(EngineDfa *e)
{
    if (!e)
        return;
    dfaset_free(&e->dfas);
    free(e);
}
