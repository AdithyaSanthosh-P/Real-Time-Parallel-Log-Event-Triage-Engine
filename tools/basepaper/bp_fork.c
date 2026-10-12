/* bp_fork: the base paper's program (docs/base_paper.md §3.2, §3.3, §6),
 * reproduced as described. One fork per pattern per input, regcomp/regexec/
 * regfree inside each child, results over a pipe, waitpid in the parent.
 *
 *   bp_fork                      interactive, like the paper's §6 screenshots
 *   bp_fork -f FILE [--quiet]    batch: one input per line
 *   --normalize=none|paper       none (default): text goes to the regexes as typed,
 *                                which is what the §6 screenshots imply;
 *                                paper: the §3.2 process_input() pseudocode.
 *
 * Batch output, one line per input:
 *   MASK<TAB>NAMES<TAB>TEXT     MASK is 5 characters (pattern 0 first), '1' = match
 *   ERR<TAB>MESSAGE<TAB>TEXT    input rejected by --normalize=paper
 * With --quiet only the first field is printed. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bp_lib.h"

typedef struct {
    int paper_normalize;
    int quiet;
    const char *file;
} Opts;

static void usage(FILE *f)
{
    fprintf(f, "usage: bp_fork [-f FILE] [--quiet] [--normalize=none|paper]\n"
               "  no -f: interactive prompt on stdin (paper §6); -f: one input per line\n");
}

/* Interactive: one ✅/❌ line per core, in the order the children report. The
 * transcribed screenshots do not show the exact wording of these lines. */
static void print_core(const BpRecord *r, void *arg)
{
    (void)arg;
    printf("Core %d (%s): %s\n", r->idx, BP_PATTERNS[r->idx].name,
           r->matched ? "\xE2\x9C\x85" : "\xE2\x9D\x8C");
}

static void format_mask(unsigned mask, char out[BP_NPAT + 1])
{
    for (int i = 0; i < BP_NPAT; i++)
        out[i] = (mask >> i) & 1 ? '1' : '0';
    out[BP_NPAT] = '\0';
}

static void format_names(unsigned mask, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = '\0';
    for (int i = 0; i < BP_NPAT; i++) {
        if (!((mask >> i) & 1))
            continue;
        n += (size_t)snprintf(out + n, cap - n, "%s%s", n ? "," : "", BP_PATTERNS[i].name);
    }
    if (n == 0)
        snprintf(out, cap, "-");
}

/* Returns the text to match, or NULL after printing nothing and setting *err. */
static const char *prepare(const Opts *o, const char *raw, char *buf, const char **err)
{
    if (!o->paper_normalize)
        return raw;
    return bp_normalize_paper(raw, buf, err) == 0 ? buf : NULL;
}

static void run_batch(const Opts *o, FILE *in)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    char norm[BP_MAX_INPUT_LENGTH + 1];

    while ((len = getline(&line, &cap, in)) >= 0) {
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';
        const char *err = NULL;
        const char *text = prepare(o, line, norm, &err);
        if (text == NULL) {
            if (o->quiet)
                puts("ERR");
            else
                printf("ERR\t%s\t%s\n", err, line);
            continue;
        }
        unsigned mask;
        bp_classify_fork(text, NULL, NULL, &mask, NULL);
        char m[BP_NPAT + 1], names[128];
        format_mask(mask, m);
        if (o->quiet) {
            puts(m);
        } else {
            format_names(mask, names, sizeof names);
            printf("%s\t%s\t%s\n", m, names, line);
        }
    }
    free(line);
}

static void run_interactive(const Opts *o, FILE *in)
{
    char buf[4096];
    char norm[BP_MAX_INPUT_LENGTH + 1];

    puts("================================\n"
         " PARALLEL TEXT CLASSIFIER SYSTEM\n"
         "================================");
    for (;;) {
        printf("Enter text to classify: ");
        fflush(stdout);
        if (fgets(buf, sizeof buf, in) == NULL)
            break;
        size_t n = strlen(buf);
        if (n > 0 && buf[n - 1] == '\n')
            buf[n - 1] = '\0';
        const char *err = NULL;
        const char *text = prepare(o, buf, norm, &err);
        if (text == NULL) {
            printf("ERROR: %s\n", err);
            continue;
        }
        printf("INPUT TEXT: \"%s\"\nANALYZING TEXT PATTERNS IN PARALLEL...\n", text);
        unsigned mask;
        bp_classify_fork(text, print_core, NULL, &mask, NULL);
        puts("ANALYSIS COMPLETE");
    }
    putchar('\n');
}

int main(int argc, char **argv)
{
    Opts o = { 0, 0, NULL };
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-f") == 0 && i + 1 < argc) {
            o.file = argv[++i];
        } else if (strcmp(a, "--quiet") == 0) {
            o.quiet = 1;
        } else if (strcmp(a, "--normalize=paper") == 0) {
            o.paper_normalize = 1;
        } else if (strcmp(a, "--normalize=none") == 0) {
            o.paper_normalize = 0;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "bp_fork: unknown or incomplete option '%s'\n", a);
            usage(stderr);
            return 2;
        }
    }
    if (o.file) {
        FILE *f = fopen(o.file, "r");
        if (f == NULL) {
            perror(o.file);
            return 1;
        }
        run_batch(&o, f);
        fclose(f);
    } else {
        run_interactive(&o, stdin);
    }
    return 0;
}
