#include "pipeline.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aggregate.h"
#include "dfa.h"
#include "dispatch.h"
#include "dot.h"
#include "engine.h"
#include "nfa.h"
#include "pool.h"
#include "reader.h"
#include "route.h"
#include "rules.h"
#include "spsc.h"
#include "stats.h"
#include "worker.h"

/* ---- parallel runtime (spec §2, plan T4.3-T4.6) ----
 *
 *   calling thread   reader + dispatcher: fills jobs from the pool's free ring,
 *                    hands them to the workers' input rings, then EOF sentinels
 *   worker 0..N-1    worker.c: match + post-match steps
 *   aggregator       polls every worker's output ring, classifies (§5)
 *   router           writes the sinks, counts stats, returns jobs to the pool
 *   stats            1 Hz line on stderr (not with --quiet)
 *
 * Every hand-off is an SPSC ring (spec §2.3), so each ring has exactly one
 * producer and one consumer thread. Shutdown: the reader pushes WORKER_EOF to
 * every worker, each worker forwards it, the aggregator forwards one to the
 * router after it has seen all N, and the router stops. If any stage fails it
 * raises `stop`, which makes every blocked ring operation give up. */

/* Jobs per worker input ring (and output ring). */
#define QUEUE_DEPTH 4

typedef struct {
    const Options  *o;
    const RuleSet  *rs;
    Aggregator      agg;
    Worker         *workers;
    int             nw;
    SpscRing      **in_rings;
    SpscRing       *to_router;
    Pool            pool;
    Router         *router;
    Stats           stats;
    atomic_bool     stop;
    int             router_errno;     /* written by the router thread, read after join */
} Pipeline;

static void *aggregator_main(void *arg)
{
    Pipeline *p = arg;
    bool done[256] = { false };
    int eofs = 0, next = 0;
    SpscBackoff bo = { 0, 0 };
    while (eofs < p->nw) {
        bool got = false;
        for (int k = 0; k < p->nw; k++) {
            int i = (next + k) % p->nw;
            void *item;
            if (done[i] || !spsc_try_pop(p->workers[i].out, &item))
                continue;
            got = true;
            next = (i + 1) % p->nw;              /* rotate so no worker starves */
            Job *job = item;
            if (job == WORKER_EOF) {
                done[i] = true;
                eofs++;
                break;
            }
            ResultBatch *res = &job->res;
            for (uint32_t l = 0; l < job->batch.count; l++)
                aggregator_classify(&p->agg, &res->m[l], res->subsumed[l], &res->c[l]);
            if (!spsc_push(p->to_router, job, &p->stop))
                return NULL;
            break;
        }
        if (got) {
            bo = (SpscBackoff){ 0, 0 };
        } else {
            if (atomic_load_explicit(&p->stop, memory_order_acquire))
                return NULL;
            spsc_backoff_wait(&bo);
        }
    }
    spsc_push(p->to_router, WORKER_EOF, &p->stop);
    return NULL;
}

static void *router_main(void *arg)
{
    Pipeline *p = arg;
    for (;;) {
        void *item;
        if (!spsc_pop(p->to_router, &item, &p->stop) || item == WORKER_EOF)
            return NULL;
        Job *job = item;
        const ResultBatch *res = &job->res;
        for (uint32_t i = 0; i < job->batch.count; i++) {
            const char *line = NULL;
            size_t len = 0;
            if (res->mask_len[i] != MASK_NONE) {
                line = res->masked.data + res->mask_off[i];
                len = res->mask_len[i];
            }
            if (router_emit(p->router, &job->batch, i, &res->c[i], line, len) != 0) {
                p->router_errno = errno ? errno : EIO;
                atomic_store_explicit(&p->stop, true, memory_order_release);
                return NULL;
            }
            stats_count_line(&p->stats, res->c[i].label_mask);
        }
        /* the free ring holds every job, so this never waits */
        spsc_push(p->pool.free_ring, job, &p->stop);
    }
}

/* Reader + dispatcher, on the calling thread. Returns 0, or an errno value
 * for a read error. */
static int read_and_dispatch(Pipeline *p, Reader *reader, Dispatcher *d)
{
    Job *spare = NULL;              /* a job we still own (after --drop) */
    for (;;) {
        Job *job = spare;
        spare = NULL;
        if (!job) {
            void *item;
            if (!spsc_pop(p->pool.free_ring, &item, &p->stop))
                return 0;           /* another stage failed; it reports */
            job = item;
        }
        int n = reader_next_batch(reader, &job->batch, (uint32_t)p->o->batch);
        if (n < 0)
            return errno ? errno : EIO;
        if (n == 0)
            break;
        DispatchResult r = dispatch_job(d, job);
        if (r == DISPATCH_STOPPED)
            return 0;
        if (r == DISPATCH_DROPPED)
            spare = job;
    }
    dispatch_eof(d, WORKER_EOF);
    return 0;
}

/* Start the workers, pinned per --pin. Returns the number started; on a
 * failure *rc is the pthread error. */
static int start_workers(Pipeline *p, int *rc, char *msg, size_t msg_len)
{
    int cpus[CPU_SETSIZE];
    int ncpu = p->o->pin ? worker_cpu_order(cpus, CPU_SETSIZE) : 0;
    StrBuf where;
    sb_init(&where);
    for (int i = 0; i < p->nw; i++) {
        p->workers[i].cpu = ncpu > 0 ? cpus[i % ncpu] : -1;
        if (ncpu > 0)
            sb_printf(&where, "%s%d", i ? "," : "", p->workers[i].cpu);
        if ((*rc = worker_start(&p->workers[i])) != 0) {
            sb_free(&where);
            return i;
        }
    }
    if (ncpu > 0)
        snprintf(msg, msg_len, "%d worker(s) pinned to CPU %s", p->nw, where.oom ? "?" : where.data);
    else
        snprintf(msg, msg_len, "%d worker(s), not pinned", p->nw);
    sb_free(&where);
    return p->nw;
}

int pipeline_run(const Options *o)
{
    int status = 1;
    char err[512];
    RuleSet rs;
    EnginePrepared *prepared = NULL;
    Reader *reader = NULL;
    Pipeline p;
    memset(&p, 0, sizeof p);
    p.o = o;
    p.rs = &rs;
    atomic_init(&p.stop, false);
    stats_init(&p.stats);
    int nw_init = 0, nw_started = 0;
    pthread_t agg_thread, router_thread;
    bool agg_started = false, router_started = false;
    StatsReporter rep = { 0 };

    if (rules_load(o->rules_path, &rs, err, sizeof err) != 0) {
        fprintf(stderr, "triage: %s\n", err);
        return 1;
    }
    aggregator_init(&p.agg, &rs, (float)o->tau);

    prepared = engine_prepare(o->engine, &rs, (int)o->max_states, err, sizeof err);
    if (!prepared) {
        fprintf(stderr, "triage: %s\n", err);
        goto done;
    }
    if (!o->quiet) {
        engine_describe(prepared, err, sizeof err);
        fprintf(stderr, "triage: %s\n", err);
    }

    /* Everything is allocated and compiled before any thread starts
     * (spec §2.2: the pool is created after the rules are compiled). */
    p.nw = o->workers;
    p.workers = calloc((size_t)p.nw, sizeof *p.workers);
    p.in_rings = calloc((size_t)p.nw, sizeof *p.in_rings);
    if (!p.workers || !p.in_rings) {
        fprintf(stderr, "triage: out of memory\n");
        goto done;
    }
    for (; nw_init < p.nw; nw_init++) {
        if (worker_init(&p.workers[nw_init], nw_init, prepared, &rs, QUEUE_DEPTH, &p.stop,
                        err, sizeof err) != 0) {
            fprintf(stderr, "triage: %s\n", err);
            goto done;
        }
        p.in_rings[nw_init] = p.workers[nw_init].in;
    }
    if (pool_init(&p.pool, pool_size_for(p.nw, QUEUE_DEPTH)) != 0 ||
        !(p.to_router = spsc_new(p.pool.n))) {
        fprintf(stderr, "triage: out of memory\n");
        goto done;
    }
    p.router = router_open(o->out_dir, &rs, err, sizeof err);
    if (!p.router) {
        fprintf(stderr, "triage: %s\n", err);
        goto done;
    }
    reader = reader_open_path(o->input, 0);
    if (!reader) {
        fprintf(stderr, "triage: %s: %s\n", o->input ? o->input : "stdin", strerror(errno));
        goto done;
    }
    reader_set_flush_ms(reader, o->batch_delay_ms);
    Dispatcher disp;
    dispatch_init(&disp, p.in_rings, p.nw, o->dispatch, o->drop, &p.stop);

    /* ---- start the threads (consumers first) ---- */
    int trc;
    if ((trc = pthread_create(&router_thread, NULL, router_main, &p)) != 0)
        goto start_failed;
    router_started = true;
    if ((trc = pthread_create(&agg_thread, NULL, aggregator_main, &p)) != 0)
        goto start_failed;
    agg_started = true;
    nw_started = start_workers(&p, &trc, err, sizeof err);
    if (nw_started < p.nw)
        goto start_failed;
    if (!o->quiet)
        fprintf(stderr, "triage: %s\n", err);
    if (!o->quiet) {
        rep = (StatsReporter){ .stats = &p.stats, .queues = p.in_rings, .nqueues = p.nw,
                               .dropped = &disp.dropped_lines, .out = stderr, .interval_ms = 1000 };
        stats_reporter_start(&rep);
    }

    int read_errno = read_and_dispatch(&p, reader, &disp);
    if (read_errno)
        atomic_store_explicit(&p.stop, true, memory_order_release);
    goto join;

start_failed:
    fprintf(stderr, "triage: cannot start threads: %s\n", strerror(trc));
    atomic_store_explicit(&p.stop, true, memory_order_release);
    read_errno = -1;

join:
    for (int i = 0; i < nw_started; i++)
        worker_join(&p.workers[i]);
    if (agg_started)
        pthread_join(agg_thread, NULL);
    if (router_started)
        pthread_join(router_thread, NULL);
    stats_reporter_stop(&rep);

    if (read_errno > 0)
        fprintf(stderr, "triage: read error: %s\n", strerror(read_errno));
    if (p.router_errno)
        fprintf(stderr, "triage: write error in %s: %s\n", o->out_dir, strerror(p.router_errno));
    for (int i = 0; i < nw_started; i++)
        if (p.workers[i].pin_failed && !o->quiet)
            fprintf(stderr, "triage: warning: could not pin worker %d to CPU %d\n", i, p.workers[i].cpu);
    if (read_errno == 0 && !p.router_errno) {
        uint64_t dropped = atomic_load(&disp.dropped_lines);
        if (!o->quiet)
            fprintf(stderr, "triage: %llu lines, %llu alerts, %llu PII, %llu dropped -> %s\n",
                    (unsigned long long)router_records(p.router),
                    (unsigned long long)router_alerts(p.router),
                    (unsigned long long)router_quarantined(p.router),
                    (unsigned long long)dropped, o->out_dir);
        status = 0;
    }

done:
    if (p.router && router_close(p.router) != 0 && status == 0) {
        fprintf(stderr, "triage: write error while closing output files in %s\n", o->out_dir);
        status = 1;
    }
    reader_close(reader);
    spsc_free(p.to_router);
    pool_free(&p.pool);
    for (int i = 0; i < nw_init; i++)
        worker_destroy(&p.workers[i]);
    free(p.workers);
    free(p.in_rings);
    engine_prepared_free(prepared);
    rules_free(&rs);
    return status;
}

int pipeline_dump_dot(const Options *o)
{
    char err[512];
    RuleSet rs;
    if (rules_load(o->rules_path, &rs, err, sizeof err) != 0) {
        fprintf(stderr, "triage: %s\n", err);
        return 1;
    }
    int status = 1;
    Nfa nfa;
    DfaSet ds;
    bool have_nfa = false, have_dfa = false;
    FILE *f = NULL;

    if (nfa_build_rules(&nfa, &rs, err, sizeof err) != 0) {
        fprintf(stderr, "triage: %s\n", err);
        goto done;
    }
    have_nfa = true;
    if (dfaset_build(&nfa, (int)o->max_states, &ds, err, sizeof err) != 0) {
        fprintf(stderr, "triage: %s\n", err);
        goto done;
    }
    have_dfa = true;

    bool to_stdout = strcmp(o->dump_dot, "-") == 0;
    f = to_stdout ? stdout : fopen(o->dump_dot, "w");
    if (!f) {
        fprintf(stderr, "triage: %s: %s\n", o->dump_dot, strerror(errno));
        goto done;
    }
    int rc = dot_write(f, &rs, &nfa, &ds);
    if ((to_stdout ? fflush(f) : fclose(f)) != 0 || rc != 0) {
        fprintf(stderr, "triage: %s: write error\n", o->dump_dot);
        goto done;
    }
    if (!o->quiet)
        fprintf(stderr, "triage: wrote NFA (%d states) and %d DFA group(s) (%lld states) to %s\n",
                nfa.n, ds.ngroups, (long long)ds.total_states, o->dump_dot);
    status = 0;

done:
    if (have_dfa)
        dfaset_free(&ds);
    if (have_nfa)
        nfa_free(&nfa);
    rules_free(&rs);
    return status;
}
