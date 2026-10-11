#include "engine_posix.h"

#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct EnginePosix {
    int     count;
    regex_t re[RULES_MAX];
};

EnginePosix *posix_new(const RuleSet *rs, char *err, size_t err_len)
{
    EnginePosix *e = calloc(1, sizeof *e);
    if (!e) {
        snprintf(err, err_len, "out of memory");
        return NULL;
    }
    for (int i = 0; i < rs->count; i++) {
        const Rule *r = &rs->rules[i];
        int flags = REG_EXTENDED | REG_NOSUB | (r->icase ? REG_ICASE : 0);
        int rc = regcomp(&e->re[i], r->regex, flags);
        if (rc != 0) {
            char msg[128];
            regerror(rc, &e->re[i], msg, sizeof msg);
            snprintf(err, err_len, "rule %s: regcomp failed: %s", r->id, msg);
            posix_free(e);
            return NULL;
        }
        e->count = i + 1;
    }
    return e;
}

uint64_t posix_match(EnginePosix *e, const char *line, size_t len)
{
    uint64_t mask = 0;
    for (int i = 0; i < e->count; i++) {
        /* REG_STARTEND: match in [rm_so, rm_eo) of `line`, so no NUL
         * terminator and no copy are needed; ^ and $ bind to those ends. */
        regmatch_t range;
        range.rm_so = 0;
        range.rm_eo = (regoff_t)len;
        if (regexec(&e->re[i], line, 0, &range, REG_STARTEND) == 0)
            mask |= (uint64_t)1 << i;
    }
    return mask;
}

void posix_free(EnginePosix *e)
{
    if (!e)
        return;
    for (int i = 0; i < e->count; i++)
        regfree(&e->re[i]);
    free(e);
}
