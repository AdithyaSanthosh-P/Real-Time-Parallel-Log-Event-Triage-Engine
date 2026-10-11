#ifndef ENGINE_POSIX_H
#define ENGINE_POSIX_H

#include <stddef.h>
#include <stdint.h>

#include "rules.h"

/* The `posix` engine (spec §2.1): one glibc regex_t per rule, tried in turn.
 * It is the sequential baseline and the differential-test oracle for the DFA.
 *
 * An instance owns its compiled regexes and is NOT safe to share between
 * threads (glibc's regexec takes an internal lock on a shared regex_t); each
 * worker creates its own with posix_new(). Matching uses the C locale: bytes
 * are characters, which is what the DFA engine assumes too (the program never
 * calls setlocale). */

typedef struct EnginePosix EnginePosix;

/* Compile every rule (REG_EXTENDED | REG_NOSUB, plus REG_ICASE for rules with
 * the 'i' flag). Returns NULL on failure and writes a message naming the rule
 * into `err`. */
EnginePosix *posix_new(const RuleSet *rs, char *err, size_t err_len);

/* Rule mask for one line of normalized text (bit r set = rule r matches
 * somewhere in it). The line need not be NUL-terminated. */
uint64_t posix_match(EnginePosix *e, const char *line, size_t len);

void posix_free(EnginePosix *e);

#endif /* ENGINE_POSIX_H */
