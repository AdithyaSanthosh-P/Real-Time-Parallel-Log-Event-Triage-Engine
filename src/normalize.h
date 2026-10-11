#ifndef NORMALIZE_H
#define NORMALIZE_H

#include <stddef.h>

/* Build the match copy of a log line (spec §2, §4.2 notes):
 *   - "%xx" (two hex digits, either case) is replaced by that byte; an invalid
 *     or truncated escape ("%zz", "%", "%4" at the end) is copied as-is;
 *   - a literal '+' becomes a space;
 *   - control characters (bytes < 0x20 except '\t', and 0x7f) are dropped,
 *     whether they appeared literally or came from a "%xx" escape (so "%00"
 *     and "%0a" cannot be used to split a keyword);
 *   - no lowercasing: case-insensitivity is a per-rule flag.
 * Decoding is a single pass: "%2b" yields '+', not a space, and "%2541" yields
 * "%41", not "A".
 *
 * `out` must have room for `n` bytes; the result is never longer than the
 * input. `out` may equal `in` (the write position never passes the read
 * position). The result is not NUL-terminated. Returns the output length. */
size_t normalize_line(const char *in, size_t n, char *out);

#endif /* NORMALIZE_H */
