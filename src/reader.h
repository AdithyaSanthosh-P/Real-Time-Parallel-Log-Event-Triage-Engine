#ifndef READER_H
#define READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"

/* Line reader (spec §2): reads a file or stdin in large chunks, splits on
 * '\n', assigns sequence ids, and fills Batches with raw + normalized copies.
 *
 * Line rules:
 *   - a line ends at '\n'; one '\r' directly before it (CRLF) is removed;
 *   - a final line without '\n' is still a line (a trailing '\r' is removed);
 *   - empty lines are lines;
 *   - a line may be longer than the chunk size (the buffer grows); lines
 *     longer than the max-line limit (default 16 MiB) are split into pieces
 *     of that size, each its own line, so memory stays bounded;
 *   - so the number of lines of a file is the number of '\n' bytes, plus one
 *     if the file is non-empty and does not end in '\n'.
 *
 * Timestamps (spec §6): taken from the start of the line when it has a
 * syslog or ISO-8601 timestamp (timestamp.h), otherwise the ingest time. */

#define READER_DEFAULT_CHUNK     (1u << 20)
#define READER_DEFAULT_MAX_LINE  ((size_t)16 << 20)
/* A batch is closed early, before BATCH_MAX lines, once it holds this many bytes. */
#define READER_BATCH_BYTES_SOFT  ((size_t)16 << 20)

typedef struct Reader Reader;

/* Open `path` ("-" or NULL means stdin). `chunk` is the read size in bytes
 * (0 = default). Returns NULL on failure with errno set. */
Reader *reader_open_path(const char *path, size_t chunk);

/* Read from an open descriptor. If `own_fd`, reader_close() closes it. */
Reader *reader_open_fd(int fd, bool own_fd, size_t chunk);

/* Syslog lines carry no year; default is the current UTC year. */
void reader_set_year(Reader *r, int year);
void reader_set_max_line(Reader *r, size_t max_line);

/* Fill `b` (reset first) with up to `max_lines` (<= BATCH_MAX) lines.
 * b->seq_start is the sequence id of its first line; ids are consecutive from
 * 0 across the whole input. Returns the number of lines, 0 at end of input,
 * or -1 on a read error (errno set). A short batch is returned when the input
 * ends; the next call then returns 0. */
int reader_next_batch(Reader *r, Batch *b, uint32_t max_lines);

/* Total lines handed out so far. */
uint64_t reader_lines_read(const Reader *r);

void reader_close(Reader *r);

#endif /* READER_H */
