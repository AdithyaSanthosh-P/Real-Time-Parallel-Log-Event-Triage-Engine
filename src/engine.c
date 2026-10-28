#include "engine.h"

#include <stdio.h>
#include <stdlib.h>

#include "engine_dfa.h"
#include "engine_fork.h"
#include "engine_posix.h"

struct EnginePrepared {
    EngineKind     kind;
    const RuleSet *rs;
    EngineDfa     *dfa;      /* ENGINE_DFA */
};

struct Engine {
    EngineKind       kind;
    const EngineDfa *dfa;    /* shared, read-only */
    EnginePosix     *posix;  /* owned by this engine */
    EngineFork      *fork;   /* owned by this engine */
};

const char *engine_kind_name(EngineKind kind)
{
    switch (kind) {
    case ENGINE_DFA:   return "dfa";
    case ENGINE_POSIX: return "posix";
    case ENGINE_FORK:  return "fork";
    }
    return "?";
}

EnginePrepared *engine_prepare(EngineKind kind, const RuleSet *rs, int max_states,
                               char *err, size_t err_len)
{
    EnginePrepared *p = calloc(1, sizeof *p);
    if (!p) {
        snprintf(err, err_len, "out of memory");
        return NULL;
    }
    p->kind = kind;
    p->rs = rs;
    if (kind == ENGINE_DFA) {
        p->dfa = dfa_engine_build(rs, max_states, err, err_len);
        if (!p->dfa) {
            free(p);
            return NULL;
        }
    }
    return p;
}

void engine_describe(const EnginePrepared *p, char *buf, size_t len)
{
    if (p->kind == ENGINE_DFA)
        snprintf(buf, len, "dfa: %d rules, NFA %d states, %d DFA group%s, %lld DFA states, built in %.1f ms",
                 p->rs->count, p->dfa->nfa_states, p->dfa->dfas.ngroups,
                 p->dfa->dfas.ngroups == 1 ? "" : "s", (long long)p->dfa->dfas.total_states,
                 p->dfa->dfas.build_ms);
    else
        snprintf(buf, len, "%s: %d rules", engine_kind_name(p->kind), p->rs->count);
}

void engine_prepared_free(EnginePrepared *p)
{
    if (!p)
        return;
    dfa_engine_free(p->dfa);
    free(p);
}

Engine *engine_new(const EnginePrepared *p, char *err, size_t err_len)
{
    Engine *e = calloc(1, sizeof *e);
    if (!e) {
        snprintf(err, err_len, "out of memory");
        return NULL;
    }
    e->kind = p->kind;
    if (p->kind == ENGINE_DFA) {
        e->dfa = p->dfa;
    } else if (p->kind == ENGINE_FORK) {
        e->fork = fork_engine_new(p->rs, err, err_len);
        if (!e->fork) {
            free(e);
            return NULL;
        }
    } else {
        e->posix = posix_new(p->rs, err, err_len);
        if (!e->posix) {
            free(e);
            return NULL;
        }
    }
    return e;
}

uint64_t engine_match(Engine *e, const char *line, size_t len)
{
    if (e->kind == ENGINE_DFA)
        return dfa_engine_match(e->dfa, line, len);
    if (e->kind == ENGINE_FORK)
        return fork_engine_match(e->fork, line, len);
    return posix_match(e->posix, line, len);
}

void engine_free(Engine *e)
{
    if (!e)
        return;
    posix_free(e->posix);
    fork_engine_free(e->fork);
    free(e);
}
