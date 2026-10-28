#include "engine_fork.h"

#include <errno.h>
#include <regex.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct EngineFork {
    const RuleSet *rs;
    int            cpus[CPU_SETSIZE];
    int            ncpu;
    unsigned long  failures;
};

/* What a child reports: the rule index and whether it matched. Smaller than
 * PIPE_BUF, so each write is atomic and records of different children never
 * interleave. */
typedef struct {
    int idx;
    int matched;
} ForkRecord;

EngineFork *fork_engine_new(const RuleSet *rs, char *err, size_t err_len)
{
    EngineFork *e = calloc(1, sizeof *e);
    if (!e) {
        snprintf(err, err_len, "out of memory");
        return NULL;
    }
    e->rs = rs;
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0)
        for (int c = 0; c < CPU_SETSIZE; c++)
            if (CPU_ISSET(c, &set))
                e->cpus[e->ncpu++] = c;
    return e;
}

/* The child's work: pin to a core (the paper pins each child), compile the
 * rule, match, report. Never returns. */
static void child_main(const EngineFork *e, int rule, const char *line, size_t len, int fd)
{
    if (e->ncpu > 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(e->cpus[rule % e->ncpu], &set);
        sched_setaffinity(0, sizeof set, &set);
    }
    const Rule *r = &e->rs->rules[rule];
    ForkRecord rec = { rule, 0 };
    regex_t re;
    int flags = REG_EXTENDED | REG_NOSUB | (r->icase ? REG_ICASE : 0);
    if (regcomp(&re, r->regex, flags) == 0) {
        regmatch_t range = { .rm_so = 0, .rm_eo = (regoff_t)len };
        rec.matched = regexec(&re, line, 0, &range, REG_STARTEND) == 0;
        regfree(&re);
    }
    ssize_t w = write(fd, &rec, sizeof rec);
    _exit(w == (ssize_t)sizeof rec ? 0 : 1);
}

static int read_record(int fd, ForkRecord *rec)
{
    char *p = (char *)rec;
    size_t got = 0;
    while (got < sizeof *rec) {
        ssize_t k = read(fd, p + got, sizeof *rec - got);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return -1;
        got += (size_t)k;
    }
    return 0;
}

uint64_t fork_engine_match(EngineFork *e, const char *line, size_t len)
{
    pid_t   child[RULES_MAX];
    int     nchild = 0, fds[2];
    uint64_t mask = 0;
    int     n = e->rs->count;

    if (pipe(fds) != 0) {
        e->failures += (unsigned long)n;
        return 0;
    }
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            close(fds[0]);
            child_main(e, i, line, len, fds[1]);
        } else if (pid > 0) {
            child[nchild++] = pid;
        } else {
            e->failures++;
        }
    }
    close(fds[1]);

    /* One record, then one waitpid, per child (the paper's collection loop). */
    for (int k = 0; k < nchild; k++) {
        ForkRecord rec;
        if (read_record(fds[0], &rec) == 0 && rec.idx >= 0 && rec.idx < n) {
            if (rec.matched)
                mask |= (uint64_t)1 << rec.idx;
        } else {
            e->failures++;
        }
        while (waitpid(child[k], NULL, 0) < 0 && errno == EINTR)
            ;
    }
    close(fds[0]);
    return mask;
}

unsigned long fork_engine_failures(const EngineFork *e)
{
    return e->failures;
}

void fork_engine_free(EngineFork *e)
{
    free(e);
}
