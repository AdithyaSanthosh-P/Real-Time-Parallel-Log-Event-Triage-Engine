#include "bp_lib.h"

#include <ctype.h>
#include <errno.h>
#include <regex.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int64_t bp_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* ---- paper §3.2: input processing -------------------------------------- */

/* The paper's ALLOWED_CHARS = "a-zA-Z0-9@.-_?! ". Read as a list of
 * characters, ".-_" is three characters; read as a regex class it would be the
 * range '.'..'_' and admit / : ; < = > [ \ ] ^ as well. We use the list reading;
 * the apostrophe of "What's your name?" is rejected under either. */
static int allowed_char(unsigned char c)
{
    return isalnum(c) || strchr("@.-_?! ", c) != NULL;
}

int bp_normalize_paper(const char *in, char *out, const char **err)
{
    size_t n = strlen(in);

    /* Step 1: length. (There is no empty-input check in the pseudocode.) */
    if (n > BP_MAX_INPUT_LENGTH) {
        *err = "Input exceeds maximum length";
        return -1;
    }
    /* Step 2: character set. Tabs and newlines are not allowed, so the tab and
     * newline replacements of step 3 can never fire; they are kept below only
     * because the pseudocode lists them. */
    for (size_t i = 0; i < n; i++) {
        if (!allowed_char((unsigned char)in[i])) {
            *err = "Input contains invalid characters";
            return -1;
        }
    }

    /* Step 3, in the pseudocode's order: lowercase, trim, normalize_special_chars. */
    char t[BP_MAX_INPUT_LENGTH + 1];
    for (size_t i = 0; i < n; i++)
        t[i] = (char)tolower((unsigned char)in[i]);
    t[n] = '\0';

    size_t b = 0, e = n;
    while (b < e && isspace((unsigned char)t[b]))
        b++;
    while (e > b && isspace((unsigned char)t[e - 1]))
        e--;

    size_t o = 0;
    for (size_t i = b; i < e;) {
        char c = t[i];
        if (c == ' ' || c == '\t') {            /* "  +" -> " ",  "\t+" -> " " */
            size_t j = i;
            while (j < e && (t[j] == ' ' || t[j] == '\t'))
                j++;
            /* a run of two or more spaces, or any tab run, becomes one space; a lone
             * space stays a space. */
            out[o++] = ' ';
            i = j;
        } else if (c == '\n') {                 /* "\n+" -> "" */
            while (i < e && t[i] == '\n')
                i++;
        } else {
            out[o++] = c;
            i++;
        }
    }
    out[o] = '\0';
    return 0;
}

/* ---- paper §3.3: parallel_classify ------------------------------------- */

/* Allowed CPUs of this process (honours taskset). The paper's
 * "i % available_cores" is read as an index into this list so that pinning
 * stays inside the cpuset the experiment was given (RB3). */
static int allowed_cpus(int *list, int cap)
{
    cpu_set_t set;
    int n = 0;
    if (sched_getaffinity(0, sizeof set, &set) != 0)
        return 0;
    for (int c = 0; c < CPU_SETSIZE && n < cap; c++)
        if (CPU_ISSET(c, &set))
            list[n++] = c;
    return n;
}

static void pin_to_cpu(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof set, &set);     /* best effort, like the paper */
}

/* execute_pattern_match(): compile, match, free, all inside the child. */
static BpRecord execute_pattern_match(const char *text, int i)
{
    BpRecord r = { .idx = i };
    regex_t re;
    int64_t t0 = bp_now_ns();
    int rc = regcomp(&re, BP_PATTERNS[i].regex, REG_EXTENDED);
    int64_t t1 = bp_now_ns();
    r.t_regcomp_ns = t1 - t0;
    if (rc != 0)
        return r;                               /* {matched: false, error: "Compilation failed"} */
    rc = regexec(&re, text, 0, NULL, 0);
    r.t_regexec_ns = bp_now_ns() - t1;
    regfree(&re);
    r.matched = (rc == 0);
    return r;
}

/* Read exactly one record; 0 on success, -1 on EOF/error. */
static int receive_result(int fd, BpRecord *r)
{
    char *p = (char *)r;
    size_t got = 0;
    while (got < sizeof *r) {
        ssize_t k = read(fd, p + got, sizeof *r - got);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return -1;
        got += (size_t)k;
    }
    return 0;
}

int bp_classify_fork(const char *text, BpRecordFn on_record, void *arg,
                     unsigned *mask, int64_t *total_ns)
{
    int cpus[CPU_SETSIZE];
    int ncpu = allowed_cpus(cpus, CPU_SETSIZE);
    pid_t child[BP_NPAT];
    int nchild = 0, nrec = 0;
    int fds[2];

    *mask = 0;
    if (pipe(fds) != 0) {
        fprintf(stderr, "bp: pipe failed: %s\n", strerror(errno));
        return 0;
    }
    int64_t t0 = bp_now_ns();
    fflush(stdout);                             /* the child must not replay buffered output */

    for (int i = 0; i < BP_NPAT; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            close(fds[0]);
            if (ncpu > 0)
                pin_to_cpu(cpus[i % ncpu]);
            BpRecord r = execute_pattern_match(text, i);
            ssize_t w = write(fds[1], &r, sizeof r);    /* < PIPE_BUF: atomic */
            _exit(w == (ssize_t)sizeof r ? 0 : 1);
        } else if (pid > 0) {
            child[nchild++] = pid;
        } else {
            fprintf(stderr, "bp: failed to create process for pattern: %s\n",
                    BP_PATTERNS[i].name);
        }
    }
    close(fds[1]);

    /* Collect: one record, then waitpid, per child (paper's loop). */
    for (int k = 0; k < nchild; k++) {
        BpRecord r;
        if (receive_result(fds[0], &r) == 0 && r.idx >= 0 && r.idx < BP_NPAT) {
            nrec++;
            if (r.matched)
                *mask |= 1u << r.idx;
            if (on_record)
                on_record(&r, arg);
        }
        while (waitpid(child[k], NULL, 0) < 0 && errno == EINTR)
            ;
    }
    close(fds[0]);
    if (total_ns)
        *total_ns = bp_now_ns() - t0;
    return nrec;
}
