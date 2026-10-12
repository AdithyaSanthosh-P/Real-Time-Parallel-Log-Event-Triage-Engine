/* bp_bench: the Phase R comparison variants (spec §10.4, plan R2). Same five
 * patterns, same inputs, same output mask; only the execution strategy differs.
 *
 *   bp_bench --variant V -f FILE [-n N] [-t T] [--warmup W] [--no-latency]
 *            [--dump-masks FILE]
 *
 *   fork             the paper: fork per pattern per input, regcomp/regexec/regfree in each child
 *   seq-recompile    one process; per input, 5 x (regcomp, regexec, regfree)
 *   seq-precompiled  compile once; per input, 5 x regexec
 *   threads-pattern  5 persistent pthreads, one pattern each, a barrier pair per input
 *   threads-data     T pthreads, each with its own 5 compiled regex_t; contiguous input chunks
 *   dfa              the 5 patterns in one DFA built by our compiler; T threads, contiguous chunks
 *
 * Input i is line (i mod lines) of FILE, taken from a preloaded pool; N defaults to the
 * number of lines. Timed region: the loop over the inputs only. Thread creation and
 * regex/DFA compilation are excluded (reported as setup_ns), except for the variants
 * that compile per input by design. Workers are pinned to the (j mod n)-th CPU of the
 * allowed set, as the paper pins child i. Prints one JSON object. */

#include <errno.h>
#include <pthread.h>
#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bp_lib.h"
#include "dfa.h"
#include "nfa.h"
#include "rules.h"

typedef enum { V_FORK, V_SEQ_RECOMPILE, V_SEQ_PRECOMPILED, V_THREADS_PATTERN, V_THREADS_DATA, V_DFA } Variant;

static const char *const VARIANT_NAMES[] = {
    "fork", "seq-recompile", "seq-precompiled", "threads-pattern", "threads-data", "dfa",
};

typedef struct {
    const char **text;                 /* text[i], i < n (points into the pool) */
    size_t n;
} Inputs;

typedef struct {
    uint8_t *mask;                     /* mask[i] */
    int64_t *lat;                      /* lat[i] in ns, or NULL */
    int64_t total_ns, setup_ns;
    int64_t sum_fork_total_ns, sum_regcomp_ns, sum_regexec_ns;   /* fork variant */
    size_t incomplete;                 /* fork: inputs where fewer than 5 records arrived */
    int64_t dfa_states;
    int ok;
} RunOut;

static int g_cpus[CPU_SETSIZE];
static int g_ncpu;

static void pin_worker(int j)
{
    if (g_ncpu > 0)
        bp_pin_to_cpu(g_cpus[j % g_ncpu]);
}

static uint8_t match_regexes(regex_t *re, const char *text)
{
    unsigned m = 0;
    for (int k = 0; k < BP_NPAT; k++)
        if (regexec(&re[k], text, 0, NULL, 0) == 0)
            m |= 1u << k;
    return (uint8_t)m;
}

static int compile_all(regex_t *re)
{
    for (int k = 0; k < BP_NPAT; k++)
        if (regcomp(&re[k], BP_PATTERNS[k].regex, REG_EXTENDED) != 0)
            return -1;
    return 0;
}

static void free_all(regex_t *re)
{
    for (int k = 0; k < BP_NPAT; k++)
        regfree(&re[k]);
}

/* ---- fork ----------------------------------------------------------------- */

typedef struct {
    int64_t regcomp_ns, regexec_ns;
} ForkAcc;

static void fork_record(const BpRecord *r, void *arg)
{
    ForkAcc *a = arg;
    a->regcomp_ns += r->t_regcomp_ns;
    a->regexec_ns += r->t_regexec_ns;
}

static void run_fork(const Inputs *in, int threads, RunOut *o)
{
    (void)threads;
    int64_t t_start = bp_now_ns();
    for (size_t i = 0; i < in->n; i++) {
        ForkAcc acc = { 0, 0 };
        unsigned mask;
        int64_t tot = 0, t0 = bp_now_ns();
        int got = bp_classify_fork(in->text[i], fork_record, &acc, &mask, &tot);
        if (o->lat)
            o->lat[i] = bp_now_ns() - t0;
        if (got != BP_NPAT)
            o->incomplete++;
        o->mask[i] = (uint8_t)mask;
        o->sum_fork_total_ns += tot;
        o->sum_regcomp_ns += acc.regcomp_ns;
        o->sum_regexec_ns += acc.regexec_ns;
    }
    o->total_ns = bp_now_ns() - t_start;
    o->ok = 1;
}

/* ---- sequential ----------------------------------------------------------- */

static void run_seq(const Inputs *in, int recompile, RunOut *o)
{
    regex_t re[BP_NPAT];
    if (!recompile) {
        int64_t s0 = bp_now_ns();
        if (compile_all(re) != 0) {
            fprintf(stderr, "bp_bench: regcomp failed\n");
            return;
        }
        o->setup_ns = bp_now_ns() - s0;
    }
    int64_t t_start = bp_now_ns();
    for (size_t i = 0; i < in->n; i++) {
        int64_t t0 = o->lat ? bp_now_ns() : 0;
        if (recompile) {
            unsigned m = 0;
            for (int k = 0; k < BP_NPAT; k++) {
                regex_t r;
                if (regcomp(&r, BP_PATTERNS[k].regex, REG_EXTENDED) == 0) {
                    if (regexec(&r, in->text[i], 0, NULL, 0) == 0)
                        m |= 1u << k;
                    regfree(&r);
                }
            }
            o->mask[i] = (uint8_t)m;
        } else {
            o->mask[i] = match_regexes(re, in->text[i]);
        }
        if (o->lat)
            o->lat[i] = bp_now_ns() - t0;
    }
    o->total_ns = bp_now_ns() - t_start;
    if (!recompile)
        free_all(re);
    o->ok = 1;
}

/* ---- threads-pattern: one persistent thread per pattern ---------------------- */

typedef struct {
    pthread_barrier_t ready, start, end;
    const char *cur;
    int quit;
    uint8_t res[BP_NPAT];
} PatShared;

typedef struct {
    PatShared *sh;
    int idx;
    int failed;
} PatArg;

static void *pattern_thread(void *p)
{
    PatArg *a = p;
    PatShared *sh = a->sh;
    regex_t re;
    pin_worker(a->idx);
    a->failed = regcomp(&re, BP_PATTERNS[a->idx].regex, REG_EXTENDED) != 0;
    pthread_barrier_wait(&sh->ready);
    for (;;) {
        pthread_barrier_wait(&sh->start);
        if (sh->quit)
            break;
        sh->res[a->idx] = !a->failed && regexec(&re, sh->cur, 0, NULL, 0) == 0;
        pthread_barrier_wait(&sh->end);
    }
    if (!a->failed)
        regfree(&re);
    return NULL;
}

static void run_threads_pattern(const Inputs *in, RunOut *o)
{
    PatShared sh;
    PatArg args[BP_NPAT];
    pthread_t th[BP_NPAT];
    memset(&sh, 0, sizeof sh);
    int64_t s0 = bp_now_ns();
    pthread_barrier_init(&sh.ready, NULL, BP_NPAT + 1);
    pthread_barrier_init(&sh.start, NULL, BP_NPAT + 1);
    pthread_barrier_init(&sh.end, NULL, BP_NPAT + 1);
    for (int k = 0; k < BP_NPAT; k++) {
        args[k] = (PatArg){ &sh, k, 0 };
        pthread_create(&th[k], NULL, pattern_thread, &args[k]);
    }
    pthread_barrier_wait(&sh.ready);
    o->setup_ns = bp_now_ns() - s0;

    int64_t t_start = bp_now_ns();
    for (size_t i = 0; i < in->n; i++) {
        int64_t t0 = o->lat ? bp_now_ns() : 0;
        sh.cur = in->text[i];
        pthread_barrier_wait(&sh.start);
        pthread_barrier_wait(&sh.end);
        unsigned m = 0;
        for (int k = 0; k < BP_NPAT; k++)
            m |= (unsigned)sh.res[k] << k;
        o->mask[i] = (uint8_t)m;
        if (o->lat)
            o->lat[i] = bp_now_ns() - t0;
    }
    o->total_ns = bp_now_ns() - t_start;

    sh.quit = 1;
    pthread_barrier_wait(&sh.start);
    int failed = 0;
    for (int k = 0; k < BP_NPAT; k++) {
        pthread_join(th[k], NULL);
        failed |= args[k].failed;
    }
    pthread_barrier_destroy(&sh.ready);
    pthread_barrier_destroy(&sh.start);
    pthread_barrier_destroy(&sh.end);
    o->ok = !failed;
}

/* ---- threads-data and dfa: chunks of the input split across threads ------------ */

typedef struct {
    Variant v;
    const Inputs *in;
    RunOut *o;
    const DfaSet *ds;
    pthread_barrier_t *ready;          /* T workers + main */
    size_t lo, hi;
    int idx;
    int failed;
} DataArg;

static void *data_thread(void *p)
{
    DataArg *a = p;
    regex_t re[BP_NPAT];
    pin_worker(a->idx);
    if (a->v == V_THREADS_DATA)
        a->failed = compile_all(re) != 0;      /* each thread owns its regex_t set */
    pthread_barrier_wait(a->ready);
    if (a->failed)
        return NULL;
    const Inputs *in = a->in;
    RunOut *o = a->o;
    for (size_t i = a->lo; i < a->hi; i++) {
        int64_t t0 = o->lat ? bp_now_ns() : 0;
        if (a->v == V_DFA)
            o->mask[i] = (uint8_t)dfaset_match(a->ds, in->text[i], strlen(in->text[i]));
        else
            o->mask[i] = match_regexes(re, in->text[i]);
        if (o->lat)
            o->lat[i] = bp_now_ns() - t0;
    }
    if (a->v == V_THREADS_DATA)
        free_all(re);
    return NULL;
}

/* The five patterns as a rule set for our compiler. Built through the ordinary rule
 * loader from an in-memory TSV (category/severity are placeholders; the rule format is
 * unchanged), so rule i = pattern i and mask bit i = pattern i. */
static int build_dfa(DfaSet *ds, Nfa *nfa, RuleSet *rs, char *err, size_t err_len)
{
    char text[2048];
    size_t n = 0;
    for (int k = 0; k < BP_NPAT; k++)
        n += (size_t)snprintf(text + n, sizeof text - n, "%s\tAUTH\tLOW\t0.5\t-\t-\t-\t-\t%s\n",
                              BP_PATTERNS[k].name, BP_PATTERNS[k].regex);
    if (rules_parse(text, n, "bp_patterns", rs, err, err_len) != 0)
        return -1;
    if (nfa_build_rules(nfa, rs, err, err_len) != 0) {
        rules_free(rs);
        return -1;
    }
    if (dfaset_build(nfa, 0, ds, err, err_len) != 0) {
        nfa_free(nfa);
        rules_free(rs);
        return -1;
    }
    return 0;
}

static void run_data(Variant v, const Inputs *in, int threads, RunOut *o)
{
    DfaSet ds;
    Nfa nfa;
    RuleSet rs;
    char err[256];
    int have_dfa = 0;
    int64_t s0 = bp_now_ns();

    if (v == V_DFA) {
        if (build_dfa(&ds, &nfa, &rs, err, sizeof err) != 0) {
            fprintf(stderr, "bp_bench: %s\n", err);
            return;
        }
        have_dfa = 1;
        o->dfa_states = ds.total_states;
    }
    if ((size_t)threads > in->n)
        threads = (int)in->n;
    DataArg *args = calloc((size_t)threads, sizeof *args);
    pthread_t *th = calloc((size_t)threads, sizeof *th);
    pthread_barrier_t ready;
    pthread_barrier_init(&ready, NULL, (unsigned)threads + 1);
    for (int j = 0; j < threads; j++) {
        args[j] = (DataArg){ v, in, o, have_dfa ? &ds : NULL, &ready,
                             in->n * (size_t)j / (size_t)threads,
                             in->n * (size_t)(j + 1) / (size_t)threads, j, 0 };
        pthread_create(&th[j], NULL, data_thread, &args[j]);
    }
    pthread_barrier_wait(&ready);
    int64_t t_start = bp_now_ns();
    o->setup_ns = t_start - s0;
    int failed = 0;
    for (int j = 0; j < threads; j++) {
        pthread_join(th[j], NULL);
        failed |= args[j].failed;
    }
    o->total_ns = bp_now_ns() - t_start;
    o->ok = !failed;
    pthread_barrier_destroy(&ready);
    free(args);
    free(th);
    if (have_dfa) {
        dfaset_free(&ds);
        nfa_free(&nfa);
        rules_free(&rs);
    }
}

static void run_variant(Variant v, const Inputs *in, int threads, RunOut *o)
{
    switch (v) {
    case V_FORK:             run_fork(in, threads, o); break;
    case V_SEQ_RECOMPILE:    run_seq(in, 1, o); break;
    case V_SEQ_PRECOMPILED:  run_seq(in, 0, o); break;
    case V_THREADS_PATTERN:  run_threads_pattern(in, o); break;
    case V_THREADS_DATA:
    case V_DFA:              run_data(v, in, threads, o); break;
    }
}

/* ---- statistics, input loading, JSON ------------------------------------------- */

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

/* Nearest-rank percentile of a sorted array. */
static int64_t percentile(const int64_t *sorted, size_t n, double p)
{
    size_t rank = (size_t)(p / 100.0 * (double)n + 0.999999999);
    if (rank < 1)
        rank = 1;
    if (rank > n)
        rank = n;
    return sorted[rank - 1];
}

static char **load_lines(const char *path, size_t *count)
{
    FILE *f = fopen(path, "r");
    char *line = NULL;
    size_t cap = 0, n = 0, vcap = 0;
    ssize_t len;
    char **v = NULL;
    if (f == NULL) {
        perror(path);
        return NULL;
    }
    while ((len = getline(&line, &cap, f)) >= 0) {
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';
        if (n == vcap) {
            vcap = vcap ? vcap * 2 : 1024;
            v = realloc(v, vcap * sizeof *v);
        }
        v[n++] = strdup(line);
    }
    free(line);
    fclose(f);
    *count = n;
    return v;
}

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static void usage(FILE *f)
{
    fprintf(f, "usage: bp_bench --variant fork|seq-recompile|seq-precompiled|threads-pattern|"
               "threads-data|dfa -f FILE\n"
               "                [-n N] [-t T] [--warmup W] [--no-latency] [--dump-masks FILE]\n");
}

int main(int argc, char **argv)
{
    const char *vname = NULL, *file = NULL, *dump = NULL;
    long n_opt = 0, threads = 1, warmup = 0;
    int want_lat = 1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (strcmp(a, "--variant") == 0 && more)         vname = argv[++i];
        else if (strcmp(a, "-f") == 0 && more)           file = argv[++i];
        else if (strcmp(a, "-n") == 0 && more)           n_opt = atol(argv[++i]);
        else if (strcmp(a, "-t") == 0 && more)           threads = atol(argv[++i]);
        else if (strcmp(a, "--warmup") == 0 && more)     warmup = atol(argv[++i]);
        else if (strcmp(a, "--dump-masks") == 0 && more) dump = argv[++i];
        else if (strcmp(a, "--no-latency") == 0)         want_lat = 0;
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(stdout); return 0; }
        else { fprintf(stderr, "bp_bench: unknown or incomplete option '%s'\n", a); usage(stderr); return 2; }
    }
    int vi = -1;
    for (int k = 0; vname && k < 6; k++)
        if (strcmp(vname, VARIANT_NAMES[k]) == 0)
            vi = k;
    if (vi < 0 || file == NULL || threads < 1 || threads > 256 || n_opt < 0 || warmup < 0) {
        usage(stderr);
        return 2;
    }
    Variant v = (Variant)vi;

    size_t pool_n = 0;
    char **pool = load_lines(file, &pool_n);
    if (pool == NULL || pool_n == 0) {
        fprintf(stderr, "bp_bench: no inputs in %s\n", file);
        return 1;
    }
    size_t n = n_opt > 0 ? (size_t)n_opt : pool_n;
    Inputs in = { malloc(n * sizeof(char *)), n };
    for (size_t i = 0; i < n; i++)
        in.text[i] = pool[i % pool_n];

    g_ncpu = bp_allowed_cpus(g_cpus, CPU_SETSIZE);

    if (warmup > 0) {                                  /* untimed pass, results discarded */
        Inputs w = { in.text, (size_t)warmup < n ? (size_t)warmup : n };
        RunOut wo = { .mask = calloc(w.n, 1) };
        run_variant(v, &w, (int)threads, &wo);
        free(wo.mask);
    }

    RunOut o = { .mask = calloc(n, 1), .lat = want_lat ? calloc(n, sizeof(int64_t)) : NULL };
    run_variant(v, &in, (int)threads, &o);
    if (!o.ok) {
        fprintf(stderr, "bp_bench: variant %s failed\n", VARIANT_NAMES[vi]);
        return 1;
    }

    if (dump) {
        FILE *d = fopen(dump, "w");
        if (d == NULL) {
            perror(dump);
            return 1;
        }
        for (size_t i = 0; i < n; i++) {
            for (int k = 0; k < BP_NPAT; k++)
                fputc((o.mask[i] >> k) & 1 ? '1' : '0', d);
            fputc('\n', d);
        }
        fclose(d);
    }

    double secs = (double)o.total_ns / 1e9;
    int nthreads = v == V_THREADS_PATTERN ? BP_NPAT
                 : (v == V_THREADS_DATA || v == V_DFA) ? (int)threads : 1;
    if (nthreads > (int)n && (v == V_THREADS_DATA || v == V_DFA))
        nthreads = (int)n;

    printf("{\"variant\":\"%s\",\"inputs\":%zu,\"pool\":%zu,\"threads\":%d,\"warmup\":%ld,\"cpus\":[",
           VARIANT_NAMES[vi], n, pool_n, nthreads, warmup);
    for (int k = 0; k < g_ncpu; k++)
        printf("%s%d", k ? "," : "", g_cpus[k]);
    printf("],\"total_seconds\":%.9f,\"inputs_per_s\":%.2f,\"setup_ns\":%lld,\"mask_hash\":\"%016llx\"",
           secs, secs > 0 ? (double)n / secs : 0.0, (long long)o.setup_ns,
           (unsigned long long)fnv1a(o.mask, n));
    if (o.lat) {
        qsort(o.lat, n, sizeof(int64_t), cmp_i64);
        printf(",\"latency_ns\":{\"p50\":%lld,\"p90\":%lld,\"p99\":%lld,\"min\":%lld,\"max\":%lld}",
               (long long)percentile(o.lat, n, 50), (long long)percentile(o.lat, n, 90),
               (long long)percentile(o.lat, n, 99), (long long)o.lat[0], (long long)o.lat[n - 1]);
    }
    if (v == V_FORK)
        printf(",\"fork\":{\"mean_fork_to_collect_ns\":%.0f,\"mean_child_regcomp_ns\":%.0f,"
               "\"mean_child_regexec_ns\":%.0f,\"incomplete_inputs\":%zu}",
               (double)o.sum_fork_total_ns / (double)n, (double)o.sum_regcomp_ns / (double)(n * BP_NPAT),
               (double)o.sum_regexec_ns / (double)(n * BP_NPAT), o.incomplete);
    if (v == V_DFA)
        printf(",\"dfa_states\":%lld", (long long)o.dfa_states);
    printf("}\n");

    for (size_t i = 0; i < pool_n; i++)
        free(pool[i]);
    free(pool);
    free(in.text);
    free(o.mask);
    free(o.lat);
    return 0;
}
