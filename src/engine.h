#ifndef ENGINE_H
#define ENGINE_H

/* Uniform handle over the three matching engines (spec §2.1), so the
 * pipeline does not care which one it runs. An Engine belongs to one thread:
 * each worker creates its own (the DFA tables behind `dfa` will be shared
 * read-only; the `posix` engine compiles its own regex_t set). */

#include <stddef.h>
#include <stdint.h>

#include "rules.h"

typedef enum { ENGINE_DFA, ENGINE_POSIX, ENGINE_FORK } EngineKind;

const char *engine_kind_name(EngineKind kind);

typedef struct Engine Engine;

/* Build an engine for `rs`. Returns NULL with a message in `err` on failure,
 * including for engines that are not implemented yet. */
Engine *engine_new(EngineKind kind, const RuleSet *rs, char *err, size_t err_len);

/* Rule mask of one line of normalized text; bit r = rule r matched. */
uint64_t engine_match(Engine *e, const char *line, size_t len);

void engine_free(Engine *e);

#endif /* ENGINE_H */
