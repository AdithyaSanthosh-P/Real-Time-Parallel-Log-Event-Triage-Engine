/* Tests for the base-paper reproduction (Phase R1).
 *
 *   test_bp PATH/TO/bp_fork
 *
 * (a) the four §6 screenshot inputs give the matched sets shown in the paper;
 * (b) the §4.4 input-module table is run through the §3.2 pseudocode and every
 *     row where they disagree is listed (recorded, not fixed);
 * (c) a 10,000-input batch run leaves no zombie processes and exits cleanly. */

#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../test_main.h"
#include "bp_lib.h"

static const char *g_bin;

/* ---- fixture loading --------------------------------------------------- */

#define MAX_ROWS 16
typedef struct {
    char f[3][512];                  /* tab-separated fields */
    int nf;
} Row;

static int load_rows(const char *path, Row *rows)
{
    FILE *f = fopen(path, "r");
    char line[2048];
    int n = 0;
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        tm_failures++;
        return 0;
    }
    while (fgets(line, sizeof line, f) && n < MAX_ROWS) {
        size_t len = strlen(line);
        if (len && line[len - 1] == '\n')
            line[--len] = '\0';
        if (line[0] == '#')
            continue;
        Row *r = &rows[n++];
        r->nf = 0;
        char *p = line;
        while (r->nf < 3) {
            char *tab = r->nf < 2 ? strchr(p, '\t') : NULL;
            size_t l = tab ? (size_t)(tab - p) : strlen(p);
            memcpy(r->f[r->nf], p, l);
            r->f[r->nf][l] = '\0';
            r->nf++;
            if (!tab)
                break;
            p = tab + 1;
        }
    }
    fclose(f);
    return n;
}

static unsigned names_to_mask(const char *names)
{
    unsigned m = 0;
    for (int i = 0; i < BP_NPAT; i++)
        if (strstr(names, BP_PATTERNS[i].name))
            m |= 1u << i;
    return m;
}

/* ---- (a) screenshots ---------------------------------------------------- */

static void count_record(const BpRecord *r, void *arg)
{
    int *seen = arg;
    CHECK(r->idx >= 0 && r->idx < BP_NPAT);
    CHECK(r->t_regcomp_ns > 0);
    seen[r->idx]++;
}

static void test_screenshots_lib(void)
{
    Row rows[MAX_ROWS];
    int n = load_rows("tests/basepaper/screenshots.txt", rows);
    CHECK_EQ_INT(n, 4);
    for (int i = 0; i < n; i++) {
        int seen[BP_NPAT] = { 0 };
        unsigned mask = 0;
        int64_t total = 0;
        int got = bp_classify_fork(rows[i].f[0], count_record, seen, &mask, &total);
        CHECK_EQ_INT(got, BP_NPAT);
        for (int k = 0; k < BP_NPAT; k++)
            CHECK_EQ_INT(seen[k], 1);                  /* each child reports exactly once */
        CHECK_EQ_INT(mask, names_to_mask(rows[i].f[1]));
        CHECK(total > 0);
    }
}

/* Run the binary with `input` as stdin (a file), return its stdout (malloc'd). */
static char *run_capture(const char *args, const char *stdin_path)
{
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "%s %s < %s", g_bin, args, stdin_path);
    FILE *p = popen(cmd, "r");
    char *out = NULL;
    size_t len = 0, cap = 0;
    char buf[4096];
    size_t k;
    if (p == NULL)
        return NULL;
    while ((k = fread(buf, 1, sizeof buf, p)) > 0) {
        if (len + k + 1 > cap) {
            cap = (len + k + 1) * 2;
            out = realloc(out, cap);
        }
        memcpy(out + len, buf, k);
        len += k;
    }
    if (out)
        out[len] = '\0';
    CHECK_EQ_INT(pclose(p), 0);
    return out;
}

/* Interactive mode: after each INPUT TEXT line there are five "Core i (NAME): mark"
 * lines (any order), then ANALYSIS COMPLETE. */
static void test_screenshots_binary(void)
{
    Row rows[MAX_ROWS];
    int n = load_rows("tests/basepaper/screenshots.txt", rows);
    char path[] = "/tmp/bp_test_in_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    for (int i = 0; i < n; i++) {
        dprintf(fd, "%s\n", rows[i].f[0]);
    }
    close(fd);

    char *out = run_capture("", path);
    CHECK(out != NULL);
    if (out) {
        CHECK(strstr(out, " PARALLEL TEXT CLASSIFIER SYSTEM") != NULL);
        CHECK(strstr(out, "Enter text to classify:") != NULL);
        char *p = out;
        for (int i = 0; i < n; i++) {
            char want[600];
            snprintf(want, sizeof want, "INPUT TEXT: \"%.500s\"\n", rows[i].f[0]);
            char *at = strstr(p, want);
            CHECK(at != NULL);
            if (at == NULL)
                break;
            char *done = strstr(at, "ANALYSIS COMPLETE");
            CHECK(done != NULL);
            if (done == NULL)
                break;
            CHECK(strstr(at, "ANALYZING TEXT PATTERNS IN PARALLEL...") != NULL);
            unsigned want_mask = names_to_mask(rows[i].f[1]), got_mask = 0;
            int lines = 0;
            for (int k = 0; k < BP_NPAT; k++) {
                char pre[64];
                snprintf(pre, sizeof pre, "Core %d (%s): ", k, BP_PATTERNS[k].name);
                char *l = strstr(at, pre);
                if (l == NULL || l > done) {
                    CHECK(0);
                    continue;
                }
                lines++;
                l += strlen(pre);
                if (strncmp(l, "\xE2\x9C\x85", 3) == 0)
                    got_mask |= 1u << k;
                else
                    CHECK(strncmp(l, "\xE2\x9D\x8C", 3) == 0);
            }
            CHECK_EQ_INT(lines, BP_NPAT);
            CHECK_EQ_INT(got_mask, want_mask);
            p = done;
        }
        free(out);
    }

    unlink(path);
}

static void test_batch_masks(void)
{
    Row rows[MAX_ROWS];
    int n = load_rows("tests/basepaper/screenshots.txt", rows);
    char path[] = "/tmp/bp_test_batch_XXXXXX";
    int fd = mkstemp(path);
    for (int i = 0; i < n; i++)
        dprintf(fd, "%s\n", rows[i].f[0]);
    close(fd);

    char args[256], cmd[512];
    snprintf(args, sizeof args, "--quiet -f %s", path);
    snprintf(cmd, sizeof cmd, "%s %s", g_bin, args);
    FILE *p = popen(cmd, "r");
    CHECK(p != NULL);
    for (int i = 0; p && i < n; i++) {
        char line[64];
        CHECK(fgets(line, sizeof line, p) != NULL);
        unsigned m = 0;
        for (int k = 0; k < BP_NPAT; k++)
            if (line[k] == '1')
                m |= 1u << k;
        CHECK_EQ_INT(strlen(line), BP_NPAT + 1);
        CHECK_EQ_INT(m, names_to_mask(rows[i].f[1]));
    }
    if (p)
        CHECK_EQ_INT(pclose(p), 0);
    unlink(path);
}

/* ---- (b) §4.4 input-module table --------------------------------------- */

typedef enum { AGREES, CONTRADICTS, MESSAGE_DIFFERS } Verdict;

static void test_input_module(void)
{
    Row rows[MAX_ROWS];
    int n = load_rows("tests/basepaper/input_module.txt", rows);
    CHECK_EQ_INT(n, 5);
    /* What the §3.2 pseudocode does with each row, worked out from the pseudocode:
     *  1 "  Hello   World  "  -> "hello world"                      agrees
     *  2 "TEST@EXAMPLE.COM"   -> "test@example.com"                 agrees
     *  3 "What's your name?"  -> error: ' is not in ALLOWED_CHARS   CONTRADICTS (paper: PASS)
     *  4 ""                   -> accepted, "" (no empty-input check) CONTRADICTS (paper: Empty input error)
     *  5 300 characters       -> error, but the message is
     *                            "Input exceeds maximum length"    message differs from "Input too long" */
    static const Verdict expect[5] = { AGREES, AGREES, CONTRADICTS, CONTRADICTS, MESSAGE_DIFFERS };

    printf("\n§4.4 Input Processing Module vs the §3.2 pseudocode (--normalize=paper)\n");
    for (int i = 0; i < n && i < 5; i++) {
        char out[BP_MAX_INPUT_LENGTH + 1];
        const char *err = NULL;
        int rc = bp_normalize_paper(rows[i].f[0], out, &err);
        int paper_err = strncmp(rows[i].f[1], "Error: ", 7) == 0;
        Verdict v;
        char got[300];
        if (rc != 0) {
            snprintf(got, sizeof got, "Error: %s", err);
            v = paper_err ? MESSAGE_DIFFERS : CONTRADICTS;
        } else {
            snprintf(got, sizeof got, "\"%s\"", out);
            if (paper_err)
                v = CONTRADICTS;
            else {
                char q[300];
                snprintf(q, sizeof q, "%s", rows[i].f[1]);
                v = strcmp(q, out) == 0 ? AGREES : CONTRADICTS;
            }
        }
        if (v == MESSAGE_DIFFERS && strcmp(got + 7, rows[i].f[1] + 7) == 0)
            v = AGREES;
        char shown[40];
        snprintf(shown, sizeof shown, "%.30s%s", rows[i].f[0], strlen(rows[i].f[0]) > 30 ? "..." : "");
        printf("  [%d] input %-36s paper: %-24s pseudocode: %-36s %s\n", i + 1, shown,
               rows[i].f[1], got,
               v == AGREES ? "AGREES" : v == CONTRADICTS ? "CONTRADICTS" : "MESSAGE DIFFERS");
        CHECK_EQ_INT(v, expect[i]);
    }

    /* pseudocode details beyond the table */
    char out[BP_MAX_INPUT_LENGTH + 1];
    const char *err = NULL;
    CHECK_EQ_INT(bp_normalize_paper("a\tb", out, &err), -1);          /* tabs are rejected first, so   */
    CHECK_STR_EQ(err, "Input contains invalid characters");           /* the tab rule is unreachable   */
    CHECK_EQ_INT(bp_normalize_paper("a/b", out, &err), -1);           /* '/' is not in the list reading */
    CHECK_EQ_INT(bp_normalize_paper("Karuna Raj", out, &err), 0);
    CHECK_STR_EQ(out, "karuna raj");      /* lowercasing makes the NAME pattern ^[A-Z][a-z]+ ... unmatchable */
    char big[BP_MAX_INPUT_LENGTH + 2];
    memset(big, 'a', BP_MAX_INPUT_LENGTH);
    big[BP_MAX_INPUT_LENGTH] = '\0';
    CHECK_EQ_INT(bp_normalize_paper(big, out, &err), 0);              /* exactly 256 is allowed */
    big[BP_MAX_INPUT_LENGTH] = 'a';
    big[BP_MAX_INPUT_LENGTH + 1] = '\0';
    CHECK_EQ_INT(bp_normalize_paper(big, out, &err), -1);
}

/* ---- (c) zombies --------------------------------------------------------- */

/* Zombies among the children of `parent` (via /proc). A child is a zombie for a moment
 * between _exit() and the parent's waitpid(), so one sighting proves nothing; a leak is a
 * pid that is still a zombie on the next poll. `prev`/`nprev` hold the previous poll's
 * zombies and are replaced; the return value is the number seen in both polls. */
#define MAX_Z 64
static int count_zombies(pid_t parent, int *children, int *readable, int *prev, int *nprev, int *seen)
{
    int cur[MAX_Z], ncur = 0;
    FILE *f;
    char path[64], buf[512];
    int z = 0, c = 0;
    char dirs[64];
    snprintf(dirs, sizeof dirs, "/proc/%d/task/%d/children", (int)parent, (int)parent);
    f = fopen(dirs, "r");
    if (f == NULL)
        return 0;
    *readable = 1;
    int cpid;
    while (fscanf(f, "%d", &cpid) == 1) {
        c++;
        snprintf(path, sizeof path, "/proc/%d/stat", cpid);
        FILE *s = fopen(path, "r");
        if (s == NULL)
            continue;
        if (fgets(buf, sizeof buf, s)) {
            char *rp = strrchr(buf, ')');
            if (rp && rp[1] == ' ' && rp[2] == 'Z') {
                (*seen)++;
                if (ncur < MAX_Z)
                    cur[ncur++] = cpid;
                for (int k = 0; k < *nprev; k++)
                    if (prev[k] == cpid)
                        z++;
            }
        }
        fclose(s);
    }
    fclose(f);
    memcpy(prev, cur, sizeof(int) * (size_t)ncur);
    *nprev = ncur;
    *children = c;
    return z;
}

#define BATCH_INPUTS 10000

static void test_no_zombies(void)
{
    static const char *const pool[] = {
        "john@example.com", "9876543210", "What is your name?", "hello everyone",
        "John Smith", "hello john@test.com", "invalid-email", "1234567890",
    };
    char path[] = "/tmp/bp_test_10k_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    for (int i = 0; i < BATCH_INPUTS; i++)
        dprintf(fd, "%s\n", pool[i % 8]);
    close(fd);

    int pfd[2];
    CHECK_EQ_INT(pipe(pfd), 0);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pfd[1], 1);
        close(pfd[0]);
        close(pfd[1]);
        execl(g_bin, g_bin, "--quiet", "-f", path, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);

    long lines = 0;
    int zombies = 0, polls = 0, max_children = 0, readable = 0, transient = 0;
    int prevz[MAX_Z], nprevz = 0;
    char buf[8192];
    unsigned mask_counts[1u << BP_NPAT] = { 0 };
    char cur[16];
    size_t curlen = 0;
    for (;;) {
        struct pollfd p = { pfd[0], POLLIN, 0 };
        int r = poll(&p, 1, 5);
        int c = 0;
        zombies += count_zombies(pid, &c, &readable, prevz, &nprevz, &transient);
        if (c > max_children)
            max_children = c;
        polls++;
        if (r > 0) {
            ssize_t k = read(pfd[0], buf, sizeof buf);
            if (k <= 0)
                break;
            for (ssize_t i = 0; i < k; i++) {
                if (buf[i] == '\n') {
                    cur[curlen] = '\0';
                    unsigned m = 0;
                    for (int b = 0; b < BP_NPAT && (size_t)b < curlen; b++)
                        if (cur[b] == '1')
                            m |= 1u << b;
                    mask_counts[m]++;
                    lines++;
                    curlen = 0;
                } else if (curlen < sizeof cur - 1) {
                    cur[curlen++] = buf[i];
                }
            }
        }
    }
    close(pfd[0]);
    int status = 0;
    CHECK_EQ_INT(waitpid(pid, &status, 0), pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK_EQ_INT(lines, BATCH_INPUTS);
    CHECK(readable);                    /* /proc/PID/task/PID/children was usable */
    CHECK_EQ_INT(zombies, 0);
    /* every child was reaped: none of ours is left over either */
    CHECK(waitpid(-1, NULL, WNOHANG) < 0);
    /* inputs cycle through 8 strings, so each distinct mask count is a multiple of 1250 */
    for (unsigned m = 0; m < (1u << BP_NPAT); m++)
        CHECK_EQ_INT(mask_counts[m] % (BATCH_INPUTS / 8), 0);
    printf("\n10,000-input batch: %ld lines, %d /proc polls, max live children seen %d, "
           "momentary zombies %d, zombies that persisted across polls %d\n",
           lines, polls, max_children, transient, zombies);
    unlink(path);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: test_bp PATH/TO/bp_fork\n");
        return 2;
    }
    g_bin = argv[1];
    signal(SIGPIPE, SIG_DFL);
    test_screenshots_lib();
    test_screenshots_binary();
    test_batch_masks();
    test_input_module();
    test_no_zombies();
    return TEST_DONE();
}
