/* Phase 4 runtime pieces in isolation: job pool (T4.2), dispatcher (T4.4),
 * worker processing and thread hand-off (T4.3), CPU order for pinning, and
 * the stats line (T4.5). The whole pipeline is checked end to end by
 * tools/compare_runs.sh (T4.6) and tools/run_e2e.sh. */

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_main.h"
#include "aggregate.h"
#include "batch.h"
#include "correlate.h"
#include "dispatch.h"
#include "engine.h"
#include "pool.h"
#include "reader.h"
#include "route.h"
#include "rules.h"
#include "stats.h"
#include "worker.h"

/* ---- pool ---- */

static void test_pool(void)
{
    CHECK_EQ_INT(pool_size_for(1, 4), 1 * 4 + 1 + 4);
    CHECK_EQ_INT(pool_size_for(8, 4), 8 * 4 + 8 + 4);

    Pool p;
    CHECK_EQ_INT(pool_init(&p, 13), 0);
    CHECK_EQ_INT(p.n, 13);
    CHECK_EQ_INT(spsc_size(p.free_ring), 13);
    Job *seen[13];
    for (int i = 0; i < 13; i++) {
        void *item = NULL;
        CHECK(spsc_try_pop(p.free_ring, &item));
        seen[i] = item;
        CHECK(seen[i] >= p.jobs && seen[i] < p.jobs + p.n);
        CHECK(seen[i]->batch.raw != NULL && seen[i]->res.masked.cap > 0);
        for (int j = 0; j < i; j++)
            CHECK(seen[j] != seen[i]);
    }
    CHECK_EQ_INT(spsc_size(p.free_ring), 0);
    for (int i = 0; i < 13; i++)                      /* recycling never overflows */
        CHECK(spsc_try_push(p.free_ring, seen[i]));
    pool_free(&p);
    pool_free(&p);                                    /* idempotent */
}

/* ---- dispatcher ---- */

static Job *fake_job(Pool *p, uint32_t lines)
{
    void *item = NULL;
    spsc_try_pop(p->free_ring, &item);
    Job *j = item;
    j->batch.count = lines;
    return j;
}

static void drain(SpscRing *r)
{
    void *item;
    while (spsc_try_pop(r, &item))
        ;
}

static void test_dispatch(void)
{
    SpscRing *rings[3] = { spsc_new(4), spsc_new(4), spsc_new(4) };
    atomic_bool stop;
    atomic_init(&stop, false);
    Pool p;
    CHECK_EQ_INT(pool_init(&p, 32), 0);
    Dispatcher d;

    /* rr: 0, 1, 2, 0, ... */
    dispatch_init(&d, rings, 3, DISPATCH_RR, false, &stop);
    for (int k = 0; k < 7; k++)
        CHECK_EQ_INT(dispatch_pick(&d), k % 3);

    /* least: fewest queued jobs; ties go to the first ring after the last pick */
    dispatch_init(&d, rings, 3, DISPATCH_LEAST, false, &stop);
    spsc_try_push(rings[0], NULL);
    spsc_try_push(rings[0], NULL);
    spsc_try_push(rings[2], NULL);                   /* sizes 2 0 1 */
    CHECK_EQ_INT(dispatch_pick(&d), 1);
    spsc_try_push(rings[1], NULL);                   /* sizes 2 1 1: tie, scan starts at 2 */
    CHECK_EQ_INT(dispatch_pick(&d), 2);
    spsc_try_push(rings[2], NULL);                   /* sizes 2 1 2 */
    CHECK_EQ_INT(dispatch_pick(&d), 1);
    drain(rings[0]); drain(rings[1]); drain(rings[2]);
    for (int k = 0; k < 6; k++)                      /* all empty: rotates on from the last pick */
        CHECK_EQ_INT(dispatch_pick(&d), (2 + k) % 3);

    /* blocking dispatch fills the rings in rr order */
    dispatch_init(&d, rings, 3, DISPATCH_RR, false, &stop);
    for (int k = 0; k < 12; k++)
        CHECK_EQ_INT(dispatch_job(&d, fake_job(&p, 10)), DISPATCH_SENT);
    for (int i = 0; i < 3; i++)
        CHECK_EQ_INT(spsc_size(rings[i]), 4);
    /* full + stop raised: gives up instead of waiting forever */
    atomic_store(&stop, true);
    Job *extra = fake_job(&p, 10);
    CHECK_EQ_INT(dispatch_job(&d, extra), DISPATCH_STOPPED);
    CHECK(!dispatch_eof(&d, WORKER_EOF));
    atomic_store(&stop, false);

    /* drop: a full ring drops the job and counts its lines */
    dispatch_init(&d, rings, 3, DISPATCH_RR, true, &stop);
    CHECK_EQ_INT(dispatch_job(&d, extra), DISPATCH_DROPPED);
    extra->batch.count = 7;
    CHECK_EQ_INT(dispatch_job(&d, extra), DISPATCH_DROPPED);
    CHECK_EQ_INT(atomic_load(&d.dropped_lines), 17);
    CHECK_EQ_INT(atomic_load(&d.dropped_batches), 2);
    drain(rings[2]);
    d.next = 2;
    CHECK_EQ_INT(dispatch_job(&d, extra), DISPATCH_SENT);
    CHECK_EQ_INT(atomic_load(&d.dropped_lines), 17);

    /* EOF goes to every ring (after the jobs already queued) */
    drain(rings[0]); drain(rings[1]); drain(rings[2]);
    CHECK(dispatch_eof(&d, WORKER_EOF));
    for (int i = 0; i < 3; i++) {
        void *item = NULL;
        CHECK(spsc_try_pop(rings[i], &item));
        CHECK(item == WORKER_EOF);
    }
    for (int i = 0; i < 3; i++)
        spsc_free(rings[i]);
    pool_free(&p);
}

/* ---- worker ---- */

typedef struct {
    int      primary;            /* category index, -1 = none */
    uint32_t labels;
} Expect;

static int label_index(const char *name)
{
    for (int c = 0; c < N_CATEGORIES; c++)
        if (strcmp(category_label(c), name) == 0)
            return c;
    return -2;
}

/* tests/expected/synthetic_small.expected: seq, primary, labels */
static int load_expected(Expect *e, int max)
{
    FILE *f = fopen("tests/expected/synthetic_small.expected", "r");
    if (!f)
        return -1;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        char primary[64], labels[256];
        int seq;
        if (sscanf(line, "%d\t%63s\t%255s", &seq, primary, labels) != 3 || seq != n || n >= max)
            break;
        e[n].primary = strcmp(primary, "-") == 0 ? -1 : label_index(primary);
        e[n].labels = 0;
        if (strcmp(labels, "-") != 0)
            for (char *t = strtok(labels, ","); t; t = strtok(NULL, ","))
                e[n].labels |= (uint32_t)cat_from_index(label_index(t));
        n++;
    }
    fclose(f);
    return n;
}

/* worker_process + classification + correlator reproduce the expected file,
 * for both engines and odd batch sizes (results must not depend on the
 * batching). */
static void test_worker_process(EngineKind kind, uint32_t batch_lines)
{
    char err[256];
    RuleSet rs;
    CHECK_EQ_INT(rules_load("rules/default.rules", &rs, err, sizeof err), 0);
    Expect exp[512];
    int nexp = load_expected(exp, 512);
    CHECK(nexp > 150);
    Aggregator agg;
    aggregator_init(&agg, &rs, 0.5f);
    EnginePrepared *ep = engine_prepare(kind, &rs, 0, err, sizeof err);
    CHECK(ep != NULL);
    atomic_bool stop;
    atomic_init(&stop, false);
    Worker w;
    CHECK_EQ_INT(worker_init(&w, 0, ep, &rs, 4, &stop, err, sizeof err), 0);
    Pool p;
    CHECK_EQ_INT(pool_init(&p, 1), 0);
    Job *job = p.jobs;
    CorrelatorConfig cfg;
    correlator_default_config(&cfg);
    Correlator *corr = correlator_new(&cfg, &rs);
    int escalated = 0;

    Reader *r = reader_open_path("tests/corpus/synthetic_small.log", 0);
    int seen = 0, wrong = 0, masked = 0;
    int n;
    while ((n = reader_next_batch(r, &job->batch, batch_lines)) > 0) {
        worker_process(&w, job);
        for (uint32_t i = 0; i < job->batch.count; i++) {
            const MatchResult *m = &job->res.m[i];
            CHECK_EQ_INT(m->seq, job->batch.seq_start + i);
            Classified c;
            Evidence ev;
            aggregator_evidence(&agg, m, &ev);
            aggregator_decide(&agg, &ev, m->seq, ev.kept & ~job->res.subsumed[i], 0, &c);
            uint32_t flags = correlator_observe(corr, job->batch.ts[i], m, c.label_mask, &ev);
            if (flags) {
                aggregator_decide(&agg, &ev, m->seq, c.rule_mask, flags, &c);
                escalated++;
            }
            int s = (int)m->seq;
            if (s >= nexp)
                continue;
            int primary = c.label_mask ? c.primary : -1;
            if (primary != exp[s].primary || c.label_mask != exp[s].labels) {
                wrong++;
                fprintf(stderr, "seq %d: primary %d labels %#x, expected %d %#x\n",
                        s, primary, c.label_mask, exp[s].primary, exp[s].labels);
            }
            if (job->res.mask_len[i] != MASK_NONE) {
                masked++;
                CHECK(job->res.mask_off[i] + job->res.mask_len[i] <= job->res.masked.len);
                CHECK((c.label_mask & CAT_PII) || m->valid_mask);
            }
            seen++;
        }
    }
    CHECK_EQ_INT(n, 0);
    CHECK_EQ_INT(seen, nexp);
    CHECK_EQ_INT(wrong, 0);
    CHECK(masked >= 10);                         /* the corpus has PII lines */
    CHECK_EQ_INT(escalated, 3);                  /* its correlator section */
    correlator_free(corr);

    reader_close(r);
    pool_free(&p);
    worker_destroy(&w);
    engine_prepared_free(ep);
    rules_free(&rs);
}

/* The thread: jobs come out in the order they went in, then the sentinel. */
static void test_worker_thread(void)
{
    char err[256];
    RuleSet rs;
    CHECK_EQ_INT(rules_load("rules/default.rules", &rs, err, sizeof err), 0);
    EnginePrepared *ep = engine_prepare(ENGINE_DFA, &rs, 0, err, sizeof err);
    atomic_bool stop;
    atomic_init(&stop, false);
    Worker w;
    CHECK_EQ_INT(worker_init(&w, 3, ep, &rs, 2, &stop, err, sizeof err), 0);
    Pool p;
    CHECK_EQ_INT(pool_init(&p, 8), 0);
    static const char *lines[] = {
        "Failed password for root from 10.0.0.1 port 22 ssh2",
        "GET /item?id=1+UNION+SELECT+password+FROM+users",
        "all good here",
    };
    for (int k = 0; k < 8; k++) {
        batch_reset(&p.jobs[k].batch);
        p.jobs[k].batch.seq_start = (uint64_t)k * 10;
        for (int l = 0; l <= k % 3; l++)
            batch_add_line(&p.jobs[k].batch, lines[l], strlen(lines[l]), 0, 0);
    }
    w.cpu = -1;
    CHECK_EQ_INT(worker_start(&w), 0);
    int in = 0, out = 0;
    bool got_eof = false;
    while (!got_eof) {                           /* ring depth 2: interleave push and pop */
        if (in < 8 && spsc_try_push(w.in, &p.jobs[in]))
            in++;
        else if (in == 8 && spsc_try_push(w.in, WORKER_EOF))
            in++;
        void *item;
        if (spsc_try_pop(w.out, &item)) {
            if (item == WORKER_EOF) {
                got_eof = true;
            } else {
                CHECK(item == &p.jobs[out]);
                Job *j = item;
                CHECK(j->res.m[0].rule_mask != 0);               /* line 0 matches */
                CHECK_EQ_INT(j->res.m[0].seq, (uint64_t)out * 10);
                if (j->batch.count == 3)
                    CHECK_EQ_INT(j->res.m[2].rule_mask, 0);
                out++;
            }
        }
    }
    worker_join(&w);
    CHECK_EQ_INT(out, 8);
    CHECK(!w.pin_failed);

    /* stop unblocks a worker waiting for input */
    CHECK_EQ_INT(worker_start(&w), 0);
    atomic_store(&stop, true);
    worker_join(&w);

    pool_free(&p);
    worker_destroy(&w);
    engine_prepared_free(ep);
    rules_free(&rs);
}

static void test_cpu_order(void)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CHECK_EQ_INT(sched_getaffinity(0, sizeof set, &set), 0);
    int allowed = CPU_COUNT(&set);
    int cpus[CPU_SETSIZE];
    int n = worker_cpu_order(cpus, CPU_SETSIZE);
    CHECK_EQ_INT(n, allowed);
    for (int i = 0; i < n; i++) {
        CHECK(CPU_ISSET(cpus[i], &set));
        for (int j = 0; j < i; j++)
            CHECK(cpus[i] != cpus[j]);
    }
    CHECK_EQ_INT(worker_cpu_order(cpus, 1), allowed > 0 ? 1 : 0);
    CHECK_EQ_INT(worker_cpu_order(cpus, 0), 0);

    /* with topology: the first entries are on distinct physical cores, and
     * every later one is an SMT sibling of one of them */
    static char seen[CPU_SETSIZE][40];
    for (int i = 0; i < n; i++) {
        char path[128], core[16] = "", pkg[16] = "";
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/core_id", cpus[i]);
        FILE *f = fopen(path, "r");
        if (!f)
            return;                              /* no topology: nothing more to check */
        if (fscanf(f, "%15s", core) != 1)
            core[0] = '\0';
        fclose(f);
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpus[i]);
        if ((f = fopen(path, "r"))) {
            if (fscanf(f, "%15s", pkg) != 1)
                pkg[0] = '\0';
            fclose(f);
        }
        snprintf(seen[i], sizeof seen[i], "%s/%s", pkg, core);
    }
    int distinct = 0;
    while (distinct < n) {
        bool dup = false;
        for (int j = 0; j < distinct; j++)
            dup |= strcmp(seen[j], seen[distinct]) == 0;
        if (dup)
            break;
        distinct++;
    }
    for (int i = distinct; i < n; i++) {
        bool dup = false;
        for (int j = 0; j < distinct; j++)
            dup |= strcmp(seen[j], seen[i]) == 0;
        CHECK(dup);
    }
    fprintf(stderr, "cpu order: %d allowed CPUs, %d physical cores first\n", n, distinct);
}

/* ---- stats ---- */

static void test_stats(void)
{
    Stats s;
    stats_init(&s);
    stats_count_line(&s, 0);
    stats_count_line(&s, CAT_AUTH | CAT_PII);
    stats_count_line(&s, CAT_SEC);
    CHECK_EQ_INT(atomic_load(&s.lines_out), 3);
    CHECK_EQ_INT(atomic_load(&s.cat[cat_index(CAT_AUTH)]), 1);
    CHECK_EQ_INT(atomic_load(&s.cat[cat_index(CAT_PII)]), 1);
    CHECK_EQ_INT(atomic_load(&s.cat[cat_index(CAT_ERR)]), 0);

    StatsSnapshot a = { .lines_out = 1000 }, b = { .lines_out = 3500, .dropped = 7, .nqueues = 3,
                                                   .queue = { 4, 0, 2 } };
    b.cat[0] = 10; b.cat[1] = 2; b.cat[2] = 5; b.cat[3] = 0; b.cat[4] = 3;
    StrBuf out;
    sb_init(&out);
    stats_format(&a, &b, 0.5, &out);
    CHECK_STR_EQ(out.data, "triage: 5000 lines/s | queues 4 0 2 | AUTH 10 SEC 2 ERR 5 PERF 0 PII 3 | dropped 7");
    sb_free(&out);
}

int main(void)
{
    test_pool();
    test_dispatch();
    test_worker_process(ENGINE_DFA, 256);
    test_worker_process(ENGINE_DFA, 7);
    test_worker_process(ENGINE_POSIX, 1);
    test_worker_thread();
    test_cpu_order();
    test_stats();
    return TEST_DONE();
}
