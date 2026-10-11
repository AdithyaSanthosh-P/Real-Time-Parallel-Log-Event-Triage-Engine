#include "normalize.h"

/* Value of a hex digit, or -1. */
static int hex_val(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int is_dropped(unsigned char c)
{
    return (c < 0x20 && c != '\t') || c == 0x7f;
}

size_t normalize_line(const char *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; ) {
        unsigned char c = (unsigned char)in[i];
        if (c == '%' && n - i >= 3) {
            /* both following bytes exist: try to decode */
            int hi = hex_val((unsigned char)in[i + 1]);
            int lo = hex_val((unsigned char)in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                c = (unsigned char)(hi * 16 + lo);
                i += 3;
                if (!is_dropped(c))
                    out[o++] = (char)c;
                continue;
            }
        }
        i++;
        if (c == '+')
            c = ' ';
        if (!is_dropped(c))
            out[o++] = (char)c;
    }
    return o;
}
