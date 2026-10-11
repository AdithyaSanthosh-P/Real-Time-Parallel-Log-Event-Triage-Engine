#include "json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_init(StrBuf *sb)
{
    memset(sb, 0, sizeof *sb);
}

void sb_free(StrBuf *sb)
{
    free(sb->data);
    sb_init(sb);
}

void sb_clear(StrBuf *sb)
{
    sb->len = 0;
    sb->oom = false;
}

/* Make room for `extra` more bytes plus a NUL. */
static bool reserve(StrBuf *sb, size_t extra)
{
    if (sb->oom)
        return false;
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap)
        return true;
    size_t cap = sb->cap ? sb->cap : 256;
    while (cap < need)
        cap *= 2;
    char *p = realloc(sb->data, cap);
    if (!p) {
        sb->oom = true;
        return false;
    }
    sb->data = p;
    sb->cap = cap;
    return true;
}

void sb_append(StrBuf *sb, const char *s, size_t n)
{
    if (!reserve(sb, n))
        return;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_puts(StrBuf *sb, const char *s)
{
    sb_append(sb, s, strlen(s));
}

void sb_putc(StrBuf *sb, char c)
{
    sb_append(sb, &c, 1);
}

void sb_printf(StrBuf *sb, const char *fmt, ...)
{
    char small[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) {
        sb->oom = true;
        return;
    }
    if ((size_t)n < sizeof small) {
        sb_append(sb, small, (size_t)n);
        return;
    }
    if (!reserve(sb, (size_t)n))
        return;
    va_start(ap, fmt);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb->len += (size_t)n;
}

/* Length of the well-formed UTF-8 sequence starting at s[0] (s[0] >= 0x80),
 * or 0 if there is none. Follows RFC 3629 / Unicode Table 3-7: no overlong
 * forms, no surrogates (U+D800..DFFF), nothing above U+10FFFF. */
static size_t utf8_seq_len(const unsigned char *s, size_t avail)
{
    unsigned char c = s[0];
    if (c >= 0xC2 && c <= 0xDF)
        return (avail >= 2 && (s[1] & 0xC0) == 0x80) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        if (avail < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80)
            return 0;
        if (c == 0xE0 && s[1] < 0xA0)       /* overlong */
            return 0;
        if (c == 0xED && s[1] > 0x9F)       /* surrogate */
            return 0;
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (avail < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 ||
            (s[3] & 0xC0) != 0x80)
            return 0;
        if (c == 0xF0 && s[1] < 0x90)       /* overlong */
            return 0;
        if (c == 0xF4 && s[1] > 0x8F)       /* above U+10FFFF */
            return 0;
        return 4;
    }
    return 0;
}

void json_put_string(StrBuf *sb, const char *str, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *s = (const unsigned char *)str;

    sb_putc(sb, '"');
    for (size_t i = 0; i < n; ) {
        unsigned char c = s[i];
        if (c >= 0x80) {
            size_t k = utf8_seq_len(s + i, n - i);
            if (k == 0) {
                sb_puts(sb, "\\ufffd");
                i++;
            } else {
                sb_append(sb, str + i, k);
                i += k;
            }
            continue;
        }
        i++;
        switch (c) {
        case '"':  sb_puts(sb, "\\\""); break;
        case '\\': sb_puts(sb, "\\\\"); break;
        case '\b': sb_puts(sb, "\\b");  break;
        case '\f': sb_puts(sb, "\\f");  break;
        case '\n': sb_puts(sb, "\\n");  break;
        case '\r': sb_puts(sb, "\\r");  break;
        case '\t': sb_puts(sb, "\\t");  break;
        default:
            if (c < 0x20 || c == 0x7f) {
                char esc[7] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15], '\0' };
                sb_append(sb, esc, 6);
            } else {
                sb_putc(sb, (char)c);
            }
        }
    }
    sb_putc(sb, '"');
}
