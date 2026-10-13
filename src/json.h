#ifndef JSON_H
#define JSON_H

#include <stdbool.h>
#include <stddef.h>

/* Growable string buffer. Appends never fail loudly: once an allocation
 * fails `oom` is set and further appends are ignored, so a record can be
 * built with plain calls and checked once at the end. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    bool   oom;
} StrBuf;

void sb_init(StrBuf *sb);
void sb_free(StrBuf *sb);
void sb_clear(StrBuf *sb);                       /* keep the allocation */
bool sb_reserve(StrBuf *sb, size_t extra);       /* room for `extra` more bytes; false on OOM */
void sb_append(StrBuf *sb, const char *s, size_t n);
void sb_puts(StrBuf *sb, const char *s);
void sb_putc(StrBuf *sb, char c);
void sb_printf(StrBuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Append `s[0..n)` as a JSON string literal, quotes included.
 *   - `"` and `\` are backslash-escaped; \b \f \n \r \t use their short forms;
 *     every other byte below 0x20, and 0x7f, becomes \u00XX;
 *   - well-formed UTF-8 sequences are copied unchanged;
 *   - any byte that is not part of a well-formed UTF-8 sequence (stray
 *     continuation bytes, truncated or overlong sequences, surrogates,
 *     values above U+10FFFF) becomes �, so the output is always valid
 *     JSON text even when the log line is not valid UTF-8. */
void json_put_string(StrBuf *sb, const char *s, size_t n);

#endif /* JSON_H */
