#ifndef EXTRACT_H
#define EXTRACT_H

/* Post-match steps of spec §3.4, run only on lines whose rule mask is not 0:
 *   - validators on the rule's matched span(s) -> MatchResult.valid_mask
 *   - entity extraction (source IPv4, username) for rules flagged `ip` / `user`
 *   - context keywords -> MatchResult.ctx_mask
 *
 * The DFA reports which rules matched but not where, so spans come from a
 * second, per-rule regexec with match offsets on the same normalized line
 * (cheap: only matched lines, only rules that need a validator or extraction).
 * An Extractor owns its own compiled regex_t set, so each worker creates its
 * own (glibc's regexec locks a shared regex_t). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rules.h"
#include "types.h"

typedef struct {
    uint32_t so;                /* offset of the first byte of the span in the line */
    uint32_t eo;                /* offset one past the last byte */
} Span;

#define MAX_SPANS 16            /* spans collected per rule per line */

typedef struct Extractor Extractor;

/* Compile the capture regexes of every rule that has a validator or an
 * extract flag. `rs` must outlive the result. NULL + `err` on failure. */
Extractor *extractor_new(const RuleSet *rs, char *err, size_t err_len);
void       extractor_free(Extractor *x);

/* Spans of rule `rule` in line[0..len), at most `max`, in order of position.
 * The search for the next span restarts one byte after the start of the
 * previous one, so spans may overlap by their boundary characters (two
 * numbers separated by one space are both found). Returns the number found. */
int extractor_spans(Extractor *x, int rule, const char *line, size_t len, Span *out, int max);

/* Fill valid_mask, src_ip and user of `out` for a line with the given rule
 * mask (also copies the mask and leaves seq and ctx_mask untouched). */
void extract_line(Extractor *x, const char *line, size_t len, uint64_t rule_mask, MatchResult *out);

/* Strict dotted quad: four decimal octets 0-255 separated by '.', nothing
 * else. Host byte order result (1.2.3.4 -> 0x01020304). */
bool parse_ipv4(const char *s, size_t n, uint32_t *ip);

/* Username of an authentication line, given the span the rule matched
 * (see extract.c for the forms recognised). Writes a NUL-terminated name of
 * at most 31 bytes into `out`; "" if there is none. */
void extract_user(const char *line, size_t len, Span span, char out[32]);

/* Source IP: the last IPv4 token inside the span (rules end in `from <ip>`).
 * Returns false if the span holds none. */
bool extract_ip(const char *line, Span span, uint32_t *ip);

/* Case-insensitive substring test per rule: bit r is set when rule r (in
 * rule_mask) has a context keyword occurring anywhere in line[0..len). */
uint64_t context_mask(const RuleSet *rs, const char *line, size_t len, uint64_t rule_mask);

#endif /* EXTRACT_H */
