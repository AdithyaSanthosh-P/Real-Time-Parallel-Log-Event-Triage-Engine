#ifndef ENGINE_FORK_H
#define ENGINE_FORK_H

#include <stddef.h>
#include <stdint.h>

#include "rules.h"

/* The `fork` engine (spec §2.1): the base paper's design applied to our rules.
 * For every line, one child process is forked per rule; the child compiles its
 * rule (regcomp), matches (regexec), writes one record to a shared pipe and
 * exits; the parent reads one record and waitpid()s per child. Deliberately
 * slow (process creation and regcomp per line per rule): it exists as the
 * experiment baseline, like the reproduction in tools/basepaper/. Single
 * threaded: the pipeline runs it with one worker. */

typedef struct EngineFork EngineFork;

EngineFork *fork_engine_new(const RuleSet *rs, char *err, size_t err_len);

/* Rule mask of one line of normalized text, as posix_match() would give it.
 * A rule whose child could not be created or reported no record counts as not
 * matched (the paper's behaviour); the number of such rules is added to
 * fork_engine_failures(). */
uint64_t fork_engine_match(EngineFork *e, const char *line, size_t len);

unsigned long fork_engine_failures(const EngineFork *e);

void fork_engine_free(EngineFork *e);

#endif /* ENGINE_FORK_H */
