#include "reader.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "batch.h"
#include "timestamp.h"

struct Reader {
    int      fd;
    bool     own_fd;
    bool     eof;
    char    *buf;
    size_t   cap;
    size_t   start;      /* first unconsumed byte */
    size_t   end;        /* end of valid data */
    size_t   scan;       /* bytes in [start, scan) are known to hold no '\n' */
    size_t   chunk;
    size_t   max_line;
    int      year;
    uint64_t next_seq;
    int64_t  now_s;      /* ingest time of the latest read */
    uint64_t now_ns;
};

static void stamp_now(Reader *r)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    r->now_s = (int64_t)t.tv_sec;
    clock_gettime(CLOCK_MONOTONIC, &t);
    r->now_ns = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

Reader *reader_open_fd(int fd, bool own_fd, size_t chunk)
{
    Reader *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->chunk = chunk ? chunk : READER_DEFAULT_CHUNK;
    r->cap = r->chunk;
    r->buf = malloc(r->cap);
    if (!r->buf) {
        free(r);
        errno = ENOMEM;
        return NULL;
    }
    r->fd = fd;
    r->own_fd = own_fd;
    r->max_line = READER_DEFAULT_MAX_LINE;
    stamp_now(r);
    r->year = ts_year_of(r->now_s);
    return r;
}

Reader *reader_open_path(const char *path, size_t chunk)
{
    if (path == NULL || strcmp(path, "-") == 0)
        return reader_open_fd(STDIN_FILENO, false, chunk);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    Reader *r = reader_open_fd(fd, true, chunk);
    if (!r) {
        int e = errno;
        close(fd);
        errno = e;
    }
    return r;
}

void reader_set_year(Reader *r, int year)        { r->year = year; }
void reader_set_max_line(Reader *r, size_t max)  { r->max_line = max ? max : 1; }

void reader_close(Reader *r)
{
    if (!r)
        return;
    if (r->own_fd)
        close(r->fd);
    free(r->buf);
    free(r);
}

uint64_t reader_lines_read(const Reader *r) { return r->next_seq; }

/* Read more bytes after `end`. Compacts or grows the buffer first. Returns 1
 * if data arrived, 0 at end of input, -1 on error. */
static int refill(Reader *r)
{
    if (r->start > 0 && r->start == r->end) {
        r->start = r->end = r->scan = 0;
    } else if (r->start > 0 && r->end == r->cap) {
        memmove(r->buf, r->buf + r->start, r->end - r->start);
        r->scan -= r->start;
        r->end -= r->start;
        r->start = 0;
    }
    if (r->end == r->cap) {
        size_t ncap = r->cap * 2;
        char *nb = realloc(r->buf, ncap);
        if (!nb) {
            errno = ENOMEM;
            return -1;
        }
        r->buf = nb;
        r->cap = ncap;
    }
    for (;;) {
        ssize_t got = read(r->fd, r->buf + r->end, r->cap - r->end);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (got == 0) {
            r->eof = true;
            return 0;
        }
        r->end += (size_t)got;
        stamp_now(r);
        return 1;
    }
}

/* Locate the next line without consuming it. On success returns 1 and sets
 * *line, *len (CR removed) and *consume (bytes to advance, newline included).
 * Returns 0 at end of input, -1 on error. The pointer is valid until the next
 * call that reads. */
static int peek_line(Reader *r, const char **line, size_t *len, size_t *consume)
{
    for (;;) {
        const char *base = r->buf + r->start;
        size_t avail = r->end - r->start;
        const char *nl = memchr(r->buf + r->scan, '\n', r->end - r->scan);
        if (nl && (size_t)(nl - base) <= r->max_line) {
            size_t n = (size_t)(nl - base);
            *consume = n + 1;
            if (n > 0 && base[n - 1] == '\r')
                n--;
            *line = base;
            *len = n;
            return 1;
        }
        if (!nl)
            r->scan = r->end;

        if (avail > r->max_line) {               /* over-long line: emit a piece */
            *line = base;
            *len = r->max_line;
            *consume = r->max_line;
            return 1;
        }
        if (r->eof) {
            if (avail == 0)
                return 0;
            size_t n = avail;                    /* last line, no newline */
            if (base[n - 1] == '\r')
                n--;
            *line = base;
            *len = n;
            *consume = avail;
            return 1;
        }
        int rc = refill(r);
        if (rc < 0)
            return -1;
    }
}

static void consume(Reader *r, size_t n)
{
    r->start += n;
    if (r->scan < r->start)
        r->scan = r->start;
}

int reader_next_batch(Reader *r, Batch *b, uint32_t max_lines)
{
    if (max_lines > BATCH_MAX)
        max_lines = BATCH_MAX;
    batch_reset(b);
    b->seq_start = r->next_seq;

    while (b->count < max_lines) {
        const char *line;
        size_t len, adv;
        int rc = peek_line(r, &line, &len, &adv);
        if (rc < 0)
            return -1;
        if (rc == 0)
            break;
        if (b->count > 0 && (size_t)b->off[b->count] + len > READER_BATCH_BYTES_SOFT)
            break;                               /* leave the line for the next batch */

        int64_t ts;
        if (!ts_parse(line, len, r->year, &ts))
            ts = r->now_s;
        if (batch_add_line(b, line, len, ts, r->now_ns) != 0) {
            errno = ENOMEM;
            return -1;
        }
        consume(r, adv);
        r->next_seq++;
    }
    return (int)b->count;
}
