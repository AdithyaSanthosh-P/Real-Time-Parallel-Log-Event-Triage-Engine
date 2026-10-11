#ifndef ANNOT_H
#define ANNOT_H

/* Loader for annotated corpora such as tests/corpus/synthetic_small.log.
 *
 * "#@ rules: A,B -- note" annotates the NEXT line with the exact rule set
 * that must match its normalized text ("none" = empty set). Other lines
 * starting with '#' are comments. Comment and annotation lines are not
 * returned as entries (they are still stream lines: `seq` counts them, so
 * `seq` is the line's position in the file as the reader numbers it).
 * Header-only (static inline) so any test can include it. */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "batch.h"
#include "reader.h"
#include "rules.h"

typedef struct {
    uint64_t seq;        /* reader sequence id (0-based line index in the file) */
    size_t   file_line;  /* 1-based line number, for messages */
    char    *text;       /* raw line, NUL-terminated copy */
    size_t   len;
    uint64_t expect;     /* expected rule mask over the normalized text */
} AnnotEntry;

typedef struct {
    AnnotEntry *e;
    size_t      n;
    size_t      total_lines;   /* every stream line, annotations included */
    uint64_t    covered;       /* union of all expected masks */
} AnnotCorpus;

static inline void annot_free(AnnotCorpus *c)
{
    for (size_t i = 0; i < c->n; i++)
        free(c->e[i].text);
    free(c->e);
    memset(c, 0, sizeof *c);
}

/* Parse the id list of one annotation. Returns 0 and sets *mask, or -1. */
static inline int annot_parse_ids(const RuleSet *rs, const char *s, size_t n, uint64_t *mask)
{
    char buf[512];
    const char *dash = NULL;
    for (size_t i = 0; i + 3 < n; i++) {
        if (memcmp(s + i, " -- ", 4) == 0) {
            dash = s + i;
            break;
        }
    }
    if (dash)
        n = (size_t)(dash - s);
    while (n > 0 && isspace((unsigned char)*s)) { s++; n--; }
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    if (n == 0 || n >= sizeof buf)
        return -1;
    memcpy(buf, s, n);
    buf[n] = '\0';

    *mask = 0;
    if (strcmp(buf, "none") == 0)
        return 0;
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        int idx = rules_find(rs, tok);
        if (idx < 0)
            return -1;
        *mask |= (uint64_t)1 << idx;
    }
    return 0;
}

/* Returns 0 on success; -1 with a message on stderr otherwise. */
static inline int annot_load(const char *path, const RuleSet *rs, AnnotCorpus *out)
{
    memset(out, 0, sizeof *out);
    Reader *r = reader_open_path(path, 0);
    Batch b;
    if (!r || batch_init(&b) != 0) {
        fprintf(stderr, "annot: cannot open %s\n", path);
        reader_close(r);
        return -1;
    }

    size_t cap = 0, file_line = 0;
    int have_expect = 0, rc = 0;
    uint64_t expect = 0;
    int n;
    while (rc == 0 && (n = reader_next_batch(r, &b, 256)) > 0) {
        for (uint32_t i = 0; i < b.count && rc == 0; i++) {
            const char *s = batch_raw(&b, i);
            size_t len = batch_raw_len(&b, i);
            file_line++;
            if (len >= 9 && memcmp(s, "#@ rules:", 9) == 0) {
                if (have_expect || annot_parse_ids(rs, s + 9, len - 9, &expect) != 0) {
                    fprintf(stderr, "%s:%zu: bad or dangling annotation\n", path, file_line);
                    rc = -1;
                }
                have_expect = 1;
                continue;
            }
            if (len > 0 && s[0] == '#')
                continue;
            if (!have_expect) {
                fprintf(stderr, "%s:%zu: line without annotation\n", path, file_line);
                rc = -1;
                break;
            }
            if (out->n == cap) {
                cap = cap ? cap * 2 : 128;
                out->e = realloc(out->e, cap * sizeof *out->e);
            }
            AnnotEntry *e = &out->e[out->n++];
            e->seq = b.seq_start + i;
            e->file_line = file_line;
            e->text = malloc(len + 1);
            memcpy(e->text, s, len);
            e->text[len] = '\0';
            e->len = len;
            e->expect = expect;
            out->covered |= expect;
            have_expect = 0;
        }
    }
    if (rc == 0 && have_expect) {
        fprintf(stderr, "%s: annotation at end of file\n", path);
        rc = -1;
    }
    out->total_lines = file_line;
    batch_free(&b);
    reader_close(r);
    if (rc != 0)
        annot_free(out);
    return rc;
}

#endif /* ANNOT_H */
