#ifndef BATCH_H
#define BATCH_H

#include "types.h"

/* Allocate the line buffers. Returns 0, or -1 on out of memory. */
int  batch_init(Batch *b);
void batch_free(Batch *b);

/* Empty the batch, keeping its buffers. */
void batch_reset(Batch *b);

/* Append one line: copies it into raw and a normalized copy (normalize.h)
 * into norm, growing the buffers as needed. Returns 0, or -1 if the batch is
 * full (BATCH_MAX lines), the offsets would overflow 32 bits, or memory ran
 * out; the batch is unchanged on failure. */
int  batch_add_line(Batch *b, const char *line, size_t len, int64_t ts, uint64_t ingest_ns);

/* Accessors for line i < count. Pointers are not NUL-terminated. */
static inline const char *batch_raw(const Batch *b, uint32_t i)  { return b->raw + b->off[i]; }
static inline size_t batch_raw_len(const Batch *b, uint32_t i)   { return b->off[i + 1] - b->off[i]; }
static inline const char *batch_norm(const Batch *b, uint32_t i) { return b->norm + b->moff[i]; }
static inline size_t batch_norm_len(const Batch *b, uint32_t i)  { return b->moff[i + 1] - b->moff[i]; }

#endif /* BATCH_H */
