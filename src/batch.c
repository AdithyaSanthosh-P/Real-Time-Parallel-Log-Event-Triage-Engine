#include "batch.h"

#include <stdlib.h>
#include <string.h>

#include "normalize.h"

#define INITIAL_CAP (64u * 1024u)

int batch_init(Batch *b)
{
    memset(b, 0, sizeof *b);
    b->raw = malloc(INITIAL_CAP);
    b->norm = malloc(INITIAL_CAP);
    if (!b->raw || !b->norm) {
        batch_free(b);
        return -1;
    }
    b->raw_cap = b->norm_cap = INITIAL_CAP;
    return 0;
}

void batch_free(Batch *b)
{
    free(b->raw);
    free(b->norm);
    b->raw = b->norm = NULL;
    b->raw_cap = b->norm_cap = 0;
    b->count = 0;
}

void batch_reset(Batch *b)
{
    b->count = 0;
    b->off[0] = 0;
    b->moff[0] = 0;
}

/* Grow *buf so that `need` bytes fit (doubling). 0 on success. */
static int ensure(char **buf, size_t *cap, size_t need)
{
    if (need <= *cap)
        return 0;
    size_t n = *cap ? *cap : INITIAL_CAP;
    while (n < need)
        n *= 2;
    char *p = realloc(*buf, n);
    if (!p)
        return -1;
    *buf = p;
    *cap = n;
    return 0;
}

int batch_add_line(Batch *b, const char *line, size_t len, int64_t ts, uint64_t ingest_ns)
{
    if (b->count >= BATCH_MAX)
        return -1;
    size_t raw_len = b->off[b->count];
    size_t norm_len = b->moff[b->count];
    if (len > UINT32_MAX - raw_len)
        return -1;
    /* A failed grow leaves the old buffers valid and the batch unchanged. */
    if (ensure(&b->raw, &b->raw_cap, raw_len + len) != 0 ||
        ensure(&b->norm, &b->norm_cap, norm_len + len) != 0)
        return -1;

    memcpy(b->raw + raw_len, line, len);
    size_t nlen = normalize_line(line, len, b->norm + norm_len);

    uint32_t i = b->count++;
    b->off[i + 1] = (uint32_t)(raw_len + len);
    b->moff[i + 1] = (uint32_t)(norm_len + nlen);
    b->ts[i] = ts;
    b->ingest_ns[i] = ingest_ns;
    return 0;
}
