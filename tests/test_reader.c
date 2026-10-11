#include <errno.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "test_main.h"
#include "batch.h"
#include "reader.h"

typedef struct {
    char  **line;
    size_t *len;
    size_t  n, cap;
} Lines;

static void lines_add(Lines *L, const char *p, size_t len)
{
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 64;
        L->line = realloc(L->line, L->cap * sizeof *L->line);
        L->len = realloc(L->len, L->cap * sizeof *L->len);
    }
    L->line[L->n] = malloc(len + 1);
    memcpy(L->line[L->n], p, len);
    L->line[L->n][len] = '\0';
    L->len[L->n++] = len;
}

static void lines_free(Lines *L)
{
    for (size_t i = 0; i < L->n; i++)
        free(L->line[i]);
    free(L->line);
    free(L->len);
    memset(L, 0, sizeof *L);
}

/* Write `data` to an anonymous temp file and return a reader over it. */
static Reader *reader_over(const void *data, size_t len, size_t chunk, FILE **keep)
{
    FILE *f = tmpfile();
    if (!f) {
        perror("tmpfile");
        exit(1);
    }
    if (len && fwrite(data, 1, len, f) != len) {
        perror("fwrite");
        exit(1);
    }
    fflush(f);
    lseek(fileno(f), 0, SEEK_SET);
    *keep = f;
    Reader *r = reader_open_fd(fileno(f), false, chunk);
    if (!r) {
        perror("reader_open_fd");
        exit(1);
    }
    return r;
}

/* Read everything through reader_next_batch() with batches of `bs` lines. */
static void read_all(Reader *r, uint32_t bs, Lines *out)
{
    Batch b;
    if (batch_init(&b) != 0)
        exit(1);
    uint64_t expect_seq = 0;
    int n;
    while ((n = reader_next_batch(r, &b, bs)) > 0) {
        CHECK_EQ_INT(b.seq_start, expect_seq);
        CHECK_EQ_INT(b.count, n);
        CHECK(n <= (int)bs);
        for (uint32_t i = 0; i < b.count; i++)
            lines_add(out, batch_raw(&b, i), batch_raw_len(&b, i));
        expect_seq += (uint64_t)n;
    }
    CHECK_EQ_INT(n, 0);
    CHECK_EQ_INT(reader_lines_read(r), expect_seq);
    batch_free(&b);
}

static const size_t CHUNKS[] = { 1, 2, 3, 7, 64, 4096, 1u << 20 };

/* Split `data` through the reader at every chunk size and compare to `want`. */
static void expect_lines(const char *data, size_t len, const char *const *want, size_t nwant,
                         int src_line)
{
    for (size_t c = 0; c < sizeof CHUNKS / sizeof CHUNKS[0]; c++) {
        FILE *f;
        Reader *r = reader_over(data, len, CHUNKS[c], &f);
        Lines L = {0};
        read_all(r, 3, &L);
        int good = L.n == nwant;
        for (size_t i = 0; good && i < nwant; i++)
            good = L.len[i] == strlen(want[i]) && memcmp(L.line[i], want[i], L.len[i]) == 0;
        tm_checks++;
        if (!good) {
            tm_failures++;
            fprintf(stderr, "%s:%d: chunk %zu: got %zu lines, want %zu\n", __FILE__, src_line,
                    CHUNKS[c], L.n, nwant);
            for (size_t i = 0; i < L.n && i < 6; i++)
                fprintf(stderr, "    got[%zu] = \"%s\"\n", i, L.line[i]);
        }
        lines_free(&L);
        reader_close(r);
        fclose(f);
    }
}

#define LINES(data, ...)                                                     \
    do {                                                                     \
        static const char *const w_[] = { __VA_ARGS__ };                     \
        expect_lines((data), sizeof(data) - 1, w_, sizeof w_ / sizeof w_[0], __LINE__); \
    } while (0)

static void test_line_splitting(void)
{
    LINES("a\nb\nc\n", "a", "b", "c");
    LINES("a\r\nb\r\n", "a", "b");                    /* CRLF */
    LINES("a\r\r\nb\n", "a\r", "b");                  /* only one CR is removed */
    LINES("a\rb\n", "a\rb");                          /* lone CR inside a line stays */
    LINES("a\nb", "a", "b");                          /* no trailing newline */
    LINES("a\r\nb\r", "a", "b");                      /* last line ends in CR only */
    LINES("a\n\nb\n", "a", "", "b");                  /* empty line in the middle */
    LINES("\n", "");                                  /* one empty line */
    LINES("\n\n", "", "");
    LINES("\r\n", "");
    LINES("x", "x");
    LINES("a\n\n", "a", "");                          /* trailing blank line counts */
    expect_lines("", 0, NULL, 0, __LINE__);           /* empty file: no lines */
}

static void test_long_lines(void)
{
    /* 1 MB line, with and without terminator, read in 4 KB chunks. */
    size_t big = 1u << 20;
    char *data = malloc(big + 1);
    memset(data, 'x', big);
    data[big] = '\n';

    for (int terminated = 0; terminated < 2; terminated++) {
        size_t total = big + (terminated ? 1 : 0);
        FILE *f;
        Reader *r = reader_over(data, total, 4096, &f);
        Lines L = {0};
        read_all(r, 8, &L);
        CHECK_EQ_INT(L.n, 1);
        if (L.n == 1) {
            CHECK_EQ_INT(L.len[0], big);
            CHECK(memcmp(L.line[0], data, big) == 0);
        }
        lines_free(&L);
        reader_close(r);
        fclose(f);
    }

    /* big line between small ones, tiny chunk (exercises buffer growth). */
    size_t mid = 100000;
    char *mixed = malloc(mid + 16);
    size_t k = 0;
    memcpy(mixed + k, "head\n", 5); k += 5;
    memset(mixed + k, 'y', mid); k += mid;
    memcpy(mixed + k, "\r\ntail\n", 7); k += 7;
    FILE *f;
    Reader *r = reader_over(mixed, k, 13, &f);
    Lines L = {0};
    read_all(r, 2, &L);
    CHECK_EQ_INT(L.n, 3);
    if (L.n == 3) {
        CHECK_STR_EQ(L.line[0], "head");
        CHECK_EQ_INT(L.len[1], mid);
        CHECK_STR_EQ(L.line[2], "tail");
    }
    lines_free(&L);
    reader_close(r);
    fclose(f);
    free(mixed);
    free(data);
}

static void test_max_line_split(void)
{
    /* Lines longer than max_line are cut into pieces of max_line bytes. */
    static const char data[] = "xxxxxxxxxxxxxxxxxxxxxxxxx\nabc\n";   /* 25 x, then abc */
    for (size_t c = 0; c < sizeof CHUNKS / sizeof CHUNKS[0]; c++) {
        FILE *f;
        Reader *r = reader_over(data, sizeof data - 1, CHUNKS[c], &f);
        reader_set_max_line(r, 10);
        Lines L = {0};
        read_all(r, 4, &L);
        CHECK_EQ_INT(L.n, 4);
        if (L.n == 4) {
            CHECK_EQ_INT(L.len[0], 10);
            CHECK_EQ_INT(L.len[1], 10);
            CHECK_EQ_INT(L.len[2], 5);
            CHECK_STR_EQ(L.line[3], "abc");
        }
        lines_free(&L);
        reader_close(r);
        fclose(f);
    }

    /* A line of exactly max_line bytes is not split (no empty line follows). */
    static const char exact[] = "0123456789\nz\n";
    for (size_t c = 0; c < sizeof CHUNKS / sizeof CHUNKS[0]; c++) {
        FILE *f;
        Reader *r = reader_over(exact, sizeof exact - 1, CHUNKS[c], &f);
        reader_set_max_line(r, 10);
        Lines L = {0};
        read_all(r, 4, &L);
        CHECK_EQ_INT(L.n, 2);
        if (L.n == 2) {
            CHECK_STR_EQ(L.line[0], "0123456789");
            CHECK_STR_EQ(L.line[1], "z");
        }
        lines_free(&L);
        reader_close(r);
        fclose(f);
    }
}

static void test_batches_and_seq(void)
{
    char data[256];
    size_t len = 0;
    for (int i = 0; i < 10; i++)
        len += (size_t)snprintf(data + len, sizeof data - len, "line %d\n", i);

    FILE *f;
    Reader *r = reader_over(data, len, 16, &f);
    Batch b;
    CHECK_EQ_INT(batch_init(&b), 0);

    static const int sizes[] = { 4, 4, 2, 0 };
    uint64_t seq[] = { 0, 4, 8, 10 };
    for (int i = 0; i < 4; i++) {
        int n = reader_next_batch(r, &b, 4);
        CHECK_EQ_INT(n, sizes[i]);
        CHECK_EQ_INT(b.seq_start, seq[i]);
        CHECK_EQ_INT(b.count, sizes[i]);
    }
    CHECK_EQ_INT(reader_next_batch(r, &b, 4), 0);     /* stays at EOF */

    /* max_lines above BATCH_MAX is clamped */
    reader_close(r);
    fclose(f);
    char *many = malloc(2 * (BATCH_MAX + 10));
    size_t mlen = 0;
    for (int i = 0; i < BATCH_MAX + 10; i++) {
        many[mlen++] = 'a';
        many[mlen++] = '\n';
    }
    r = reader_over(many, mlen, 4096, &f);
    CHECK_EQ_INT(reader_next_batch(r, &b, 100000), BATCH_MAX);
    CHECK_EQ_INT(reader_next_batch(r, &b, 100000), 10);
    CHECK_EQ_INT(b.seq_start, BATCH_MAX);
    free(many);
    reader_close(r);
    fclose(f);
    batch_free(&b);
}

static void test_batch_contents(void)
{
    static const char data[] =
        "Oct 11 10:14:22 srv1 id=1%27+OR+1\n"
        "2026-10-11T10:14:22Z second\n"
        "no timestamp here %zz\n";
    FILE *f;
    Reader *r = reader_over(data, sizeof data - 1, 8, &f);
    reader_set_year(r, 2026);
    Batch b;
    CHECK_EQ_INT(batch_init(&b), 0);

    time_t before = time(NULL);
    CHECK_EQ_INT(reader_next_batch(r, &b, 10), 3);
    time_t after = time(NULL);

    /* raw copy is untouched, normalized copy is decoded */
    CHECK(memcmp(batch_raw(&b, 0), "Oct 11 10:14:22 srv1 id=1%27+OR+1", batch_raw_len(&b, 0)) == 0);
    CHECK_EQ_INT(batch_raw_len(&b, 0), strlen("Oct 11 10:14:22 srv1 id=1%27+OR+1"));
    CHECK_EQ_INT(batch_norm_len(&b, 0), strlen("Oct 11 10:14:22 srv1 id=1' OR 1"));
    CHECK(memcmp(batch_norm(&b, 0), "Oct 11 10:14:22 srv1 id=1' OR 1", batch_norm_len(&b, 0)) == 0);
    CHECK_EQ_INT(batch_norm_len(&b, 2), strlen("no timestamp here %zz"));

    /* timestamps: syslog (given year), ISO, and ingest-time fallback */
    CHECK_EQ_INT(b.ts[0], 1791713662LL);
    CHECK_EQ_INT(b.ts[1], 1791713662LL);
    CHECK(b.ts[2] >= (int64_t)before && b.ts[2] <= (int64_t)after);
    CHECK(b.ingest_ns[0] > 0 && b.ingest_ns[2] >= b.ingest_ns[0]);

    batch_free(&b);
    reader_close(r);
    fclose(f);
}

static void test_pipe_and_open_errors(void)
{
    int p[2];
    CHECK_EQ_INT(pipe(p), 0);
    static const char data[] = "one\r\ntwo\nthree";
    CHECK_EQ_INT(write(p[1], data, sizeof data - 1), sizeof data - 1);
    close(p[1]);
    Reader *r = reader_open_fd(p[0], true, 5);
    CHECK(r != NULL);
    Lines L = {0};
    read_all(r, 8, &L);
    CHECK_EQ_INT(L.n, 3);
    if (L.n == 3)
        CHECK_STR_EQ(L.line[2], "three");
    lines_free(&L);
    reader_close(r);                                  /* closes p[0] */

    errno = 0;
    CHECK(reader_open_path("tests/corpus/no_such_file.log", 0) == NULL);
    CHECK_EQ_INT(errno, ENOENT);
}

/* Independent splitter over a whole file: expected line list. */
static void split_whole(const char *data, size_t len, Lines *out)
{
    size_t pos = 0;
    while (pos < len) {
        const char *nl = memchr(data + pos, '\n', len - pos);
        size_t end = nl ? (size_t)(nl - data) : len;
        size_t n = end - pos;
        if (n > 0 && data[pos + n - 1] == '\r')
            n--;
        lines_add(out, data + pos, n);
        pos = end + 1;
    }
}

static void test_corpus_files(void)
{
    glob_t g;
    int rc = glob("tests/corpus/*.log", 0, NULL, &g);
    CHECK_EQ_INT(rc, 0);
    if (rc != 0)
        return;
    CHECK(g.gl_pathc >= 6);                            /* 5 Loghub samples + synthetic_small */

    static const size_t chunks[] = { 7, 4096, 1u << 20 };
    for (size_t i = 0; i < g.gl_pathc; i++) {
        const char *path = g.gl_pathv[i];
        FILE *f = fopen(path, "rb");
        CHECK(f != NULL);
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *data = malloc((size_t)size + 1);
        CHECK_EQ_INT(fread(data, 1, (size_t)size, f), size);
        fclose(f);

        /* wc -l, +1 if the last line has no newline */
        size_t newlines = 0;
        for (long k = 0; k < size; k++)
            newlines += data[k] == '\n';
        size_t expected_count = newlines + (size > 0 && data[size - 1] != '\n');

        Lines want = {0};
        split_whole(data, (size_t)size, &want);
        CHECK_EQ_INT(want.n, expected_count);

        for (size_t c = 0; c < sizeof chunks / sizeof chunks[0]; c++) {
            Reader *r = reader_open_path(path, chunks[c]);
            CHECK(r != NULL);
            if (!r)
                continue;
            Lines got = {0};
            read_all(r, 256, &got);
            int same = got.n == want.n;
            for (size_t k = 0; same && k < got.n; k++)
                same = got.len[k] == want.len[k] && memcmp(got.line[k], want.line[k], got.len[k]) == 0;
            tm_checks++;
            if (!same) {
                tm_failures++;
                fprintf(stderr, "%s: chunk %zu: reader gave %zu lines, expected %zu\n", path,
                        chunks[c], got.n, want.n);
            }
            lines_free(&got);
            reader_close(r);
        }
        lines_free(&want);
        free(data);
    }
    globfree(&g);
}

int main(void)
{
    test_line_splitting();
    test_long_lines();
    test_max_line_split();
    test_batches_and_seq();
    test_batch_contents();
    test_pipe_and_open_errors();
    test_corpus_files();
    return TEST_DONE();
}
