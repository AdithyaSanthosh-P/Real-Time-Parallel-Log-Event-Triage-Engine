#include "engine.h"

#include <stdio.h>
#include <stdlib.h>

#include "engine_posix.h"

struct Engine {
    EngineKind   kind;
    EnginePosix *posix;
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

Engine *engine_new(EngineKind kind, const RuleSet *rs, char *err, size_t err_len)
{
    if (kind != ENGINE_POSIX) {
        snprintf(err, err_len, "engine '%s' is not implemented yet", engine_kind_name(kind));
        return NULL;
    }
    Engine *e = calloc(1, sizeof *e);
    if (!e) {
        snprintf(err, err_len, "out of memory");
        return NULL;
    }
    e->kind = kind;
    e->posix = posix_new(rs, err, err_len);
    if (!e->posix) {
        free(e);
        return NULL;
    }
    return e;
}

uint64_t engine_match(Engine *e, const char *line, size_t len)
{
    return posix_match(e->posix, line, len);
}

void engine_free(Engine *e)
{
    if (!e)
        return;
    posix_free(e->posix);
    free(e);
}
