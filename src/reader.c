#include "reader.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "batch.h"
#include "timestamp.h"

struct Reader {
    int      fd;
    bool     own_fd;
    bool     eof;
    bool     can_wait;   /* not a regular file: a read may block (pipe, tty, socket) */
    long     flush_ms;   /* batch delay, < 0 = never return a partial batch early */
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
    r->flush_ms = -1;
    struct stat st;
    r->can_wait = !(fstat(fd, &st) == 0 && S_ISREG(st.st_mode));
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
void reader_set_flush_ms(Reader *r, long ms)     { r->flush_ms = ms; }

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

static uint64_t mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* With a deadline (CLOCK_MONOTONIC ns, 0 = none) on an input that can block:
 * wait for data until then. Returns 1 if a read will not block, 0 on timeout,
 * -1 on error. */
static int wait_readable(Reader *r, uint64_t deadline_ns)
{
    if (deadline_ns == 0 || !r->can_wait)
        return 1;
    for (;;) {
        uint64_t now = mono_ns();
        uint64_t left = deadline_ns > now ? deadline_ns - now : 0;
        struct timespec ts = { (time_t)(left / 1000000000ull), (long)(left % 1000000000ull) };
        struct pollfd pfd = { .fd = r->fd, .events = POLLIN };
        int rc = ppoll(&pfd, 1, &ts, NULL);
        if (rc > 0)
            return 1;               /* data, EOF or an error condition: read() reports it */
        if (rc == 0)
            return 0;
        if (errno != EINTR)
            return -1;
    }
}

/* Read more bytes after `end`. Compacts or grows the buffer first. Returns 1
 * if data arrived, 0 at end of input, 2 if `deadline_ns` (see wait_readable)
 * passed with no data, -1 on error. */
static int refill(Reader *r, uint64_t deadline_ns)
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
    int w = wait_readable(r, deadline_ns);
    if (w <= 0)
        return w < 0 ? -1 : 2;
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
 * Returns 0 at end of input, 2 if no complete line arrived before
 * `deadline_ns` (0 = wait as long as needed), -1 on error. The pointer is
 * valid until the next call that reads. */
static int peek_line(Reader *r, uint64_t deadline_ns, const char **line, size_t *len, size_t *consume)
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
        int rc = refill(r, deadline_ns);
        if (rc < 0 || rc == 2)
            return rc;
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

    uint64_t deadline = 0;
    while (b->count < max_lines) {
        const char *line = NULL;
        size_t len = 0, adv = 0;
        /* Batch delay (spec §2.3): once the batch has a line, wait at most
         * flush_ms after that line arrived for the rest. */
        if (b->count == 1 && r->flush_ms >= 0)
            deadline = b->ingest_ns[0] + (uint64_t)r->flush_ms * 1000000ull;
        int rc = peek_line(r, deadline, &line, &len, &adv);
        if (rc < 0)
            return -1;
        if (rc == 0 || rc == 2)
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
