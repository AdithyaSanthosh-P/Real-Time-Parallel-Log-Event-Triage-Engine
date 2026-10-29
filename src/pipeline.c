#include "pipeline.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "aggregate.h"
#include "bench.h"
#include "correlate.h"
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
 *   correlator       puts batches back in input order, runs the sliding
 *                    windows of §6 and re-decides escalated lines
 *   router           writes the sinks, counts stats, returns jobs to the pool
 *   stats            1 Hz line on stderr (not with --quiet)
 *
 * Every hand-off is an SPSC ring (spec §2.3), so each ring has exactly one
 * producer and one consumer thread. Shutdown: the reader pushes WORKER_EOF to
 * every worker, each worker forwards it, the aggregator forwards one to the
 * correlator after it has seen all N, the correlator forwards it to the
 * router once every batch is through, and the router stops. If any stage
 * fails it raises `stop`, which makes every blocked ring operation give up.
 *
 * Order: workers finish batches out of order, but the correlator's windows
 * need lines in input order (spec §6: log time, replayable), so it holds
 * early batches until the batch with the next expected seq arrives. Batches
 * dropped by --drop never reach a worker; the reader sends them straight to
 * the correlator on the `dropped` ring so their seq range is not awaited.
 * A side effect is that the sinks are written in input order. */

/* Jobs per worker input ring (and output ring). */
#define QUEUE_DEPTH 4

typedef struct {
    const Options  *o;
    const RuleSet  *rs;
    Aggregator      agg;
    Worker         *workers;
    int             nw;
    SpscRing      **in_rings;
    SpscRing       *to_corr;          /* aggregator -> correlator */
    SpscRing       *dropped;          /* reader -> correlator (--drop) */
    SpscRing       *to_router;        /* correlator -> router */
    Job           **pending;          /* correlator's reorder buffer (pool.n slots) */
    Correlator     *corr;
    Pool            pool;
    Router         *router;
    bool            live;             /* input is a stream (stdin): flush the sinks after every batch */
    BenchHist      *hist;             /* --bench: ingest -> aggregated latency, written by the aggregator */
    Stats           stats;
    atomic_bool     stop;
    int             router_errno;     /* written by the router thread, read after join */
    bool            seq_gap;          /* written by the correlator, read after join */
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
            for (uint32_t l = 0; l < job->batch.count; l++) {
                /* aggregator_classify, keeping the evidence for the correlator */
                aggregator_evidence(&p->agg, &res->m[l], &res->ev[l]);
                aggregator_decide(&p->agg, &res->ev[l], res->m[l].seq,
                                  res->ev[l].kept & ~res->subsumed[l], 0, &res->c[l]);
            }
            if (p->hist) {
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
                for (uint32_t l = 0; l < job->batch.count; l++)
                    bench_hist_add(p->hist, now - job->batch.ingest_ns[l]);
            }
            if (!spsc_push(p->to_corr, job, &p->stop))
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
    spsc_push(p->to_corr, WORKER_EOF, &p->stop);
    return NULL;
}

/* §6 on one batch, in order; escalated lines are decided again. */
static void correlate_job(Pipeline *p, Job *job)
{
    ResultBatch *res = &job->res;
    for (uint32_t l = 0; l < job->batch.count; l++) {
        Classified *c = &res->c[l];
        uint32_t flags = correlator_observe(p->corr, job->batch.ts[l], &res->m[l],
                                            c->label_mask, &res->ev[l]);
        if (flags)
            aggregator_decide(&p->agg, &res->ev[l], c->seq, c->rule_mask, flags, c);
    }
}

static void *correlator_main(void *arg)
{
    Pipeline *p = arg;
    size_t npending = 0;
    uint64_t expect = 0;                         /* seq of the next line to correlate */
    bool agg_eof = false;
    SpscBackoff bo = { 0, 0 };
    for (;;) {
        bool got = false;
        void *item;
        if (!agg_eof && spsc_try_pop(p->to_corr, &item)) {
            got = true;
            if (item == WORKER_EOF)
                agg_eof = true;                  /* every dropped batch is on `dropped` by now */
            else
                p->pending[npending++] = item;
        }
        while (spsc_try_pop(p->dropped, &item)) {
            got = true;
            p->pending[npending++] = item;
        }
        /* release every batch that is next in order */
        for (size_t i = 0; i < npending;) {
            Job *job = p->pending[i];
            if (job->batch.seq_start != expect) {
                i++;
                continue;
            }
            p->pending[i] = p->pending[--npending];
            expect += job->batch.count;
            if (!job->dropped)
                correlate_job(p, job);
            if (!spsc_push(p->to_router, job, &p->stop))
                return NULL;
            i = 0;                               /* the next one may be earlier in the array */
        }
        if (agg_eof) {
            /* nothing more can arrive; with every batch accounted for the
             * buffer is empty (a non-empty one would be a seq gap: a bug) */
            if (npending != 0) {
                p->seq_gap = true;
                atomic_store_explicit(&p->stop, true, memory_order_release);
            }
            spsc_push(p->to_router, WORKER_EOF, &p->stop);
            return NULL;
        }
        if (got) {
            bo = (SpscBackoff){ 0, 0 };
        } else {
            if (atomic_load_explicit(&p->stop, memory_order_acquire))
                return NULL;
            spsc_backoff_wait(&bo);
        }
    }
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
        uint32_t count = job->dropped ? 0 : job->batch.count;
        for (uint32_t i = 0; i < count; i++) {
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
        if (p->live && router_flush(p->router) != 0) {
            p->router_errno = errno ? errno : EIO;
            atomic_store_explicit(&p->stop, true, memory_order_release);
            return NULL;
        }
        /* the free ring holds every job, so this never waits */
        spsc_push(p->pool.free_ring, job, &p->stop);
    }
}

/* Reader + dispatcher, on the calling thread. Returns 0, or an errno value
 * for a read error. */
static int read_and_dispatch(Pipeline *p, Reader *reader, Dispatcher *d)
{
    for (;;) {
        void *item;
        if (!spsc_pop(p->pool.free_ring, &item, &p->stop))
            return 0;               /* another stage failed; it reports */
        Job *job = item;
        job->dropped = false;
        int n = reader_next_batch(reader, &job->batch, (uint32_t)p->o->batch);
        if (n < 0)
            return errno ? errno : EIO;
        if (n == 0)
            break;
        DispatchResult r = dispatch_job(d, job);
        if (r == DISPATCH_STOPPED)
            return 0;
        if (r == DISPATCH_DROPPED) {
            /* its seq range must still reach the correlator; the ring holds
             * every job, so this never waits */
            job->dropped = true;
            spsc_push(p->dropped, job, &p->stop);
        }
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
    pthread_t agg_thread, corr_thread, router_thread;
    bool agg_started = false, corr_started = false, router_started = false;
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
    const bool quiet = o->quiet || o->bench;           /* --bench prints only its JSON on stdout */
    if (!quiet) {
        engine_describe(prepared, err, sizeof err);
        fprintf(stderr, "triage: %s\n", err);
    }

    /* Everything is allocated and compiled before any thread starts
     * (spec §2.2: the pool is created after the rules are compiled). */
    p.nw = o->workers;
    if (o->engine == ENGINE_FORK && p.nw != 1) {      /* spec §2.1: single dispatcher, no pool */
        fprintf(stderr, "triage: warning: --engine fork is single-threaded; ignoring -w %d\n", p.nw);
        p.nw = 1;
    }
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
    CorrelatorConfig ccfg;
    correlator_default_config(&ccfg);
    if (pool_init(&p.pool, pool_size_for(p.nw, QUEUE_DEPTH)) != 0 ||
        !(p.to_corr = spsc_new(p.pool.n)) || !(p.dropped = spsc_new(p.pool.n)) ||
        !(p.to_router = spsc_new(p.pool.n)) ||
        !(p.pending = calloc(p.pool.n, sizeof *p.pending)) ||
        !(p.corr = correlator_new(&ccfg, &rs))) {
        fprintf(stderr, "triage: out of memory\n");
        goto done;
    }
    p.router = o->bench ? router_open_null(&rs, err, sizeof err) : router_open(o->out_dir, &rs, err, sizeof err);
    if (!p.router) {
        fprintf(stderr, "triage: %s\n", err);
        goto done;
    }
    p.live = !o->bench && (!o->input || strcmp(o->input, "-") == 0);
    BenchHist hist;
    size_t bench_bytes = 0;
    if (o->bench) {
        /* the whole input is read into memory before the clock starts (spec §10.2) */
        memset(&hist, 0, sizeof hist);
        p.hist = &hist;
        int fd = bench_preload(o->input, &bench_bytes);
        reader = fd < 0 ? NULL : reader_open_fd(fd, true, 0);
        if (!reader && fd >= 0)
            close(fd);
    } else {
        reader = reader_open_path(o->input, 0);
    }
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
    if ((trc = pthread_create(&corr_thread, NULL, correlator_main, &p)) != 0)
        goto start_failed;
    corr_started = true;
    if ((trc = pthread_create(&agg_thread, NULL, aggregator_main, &p)) != 0)
        goto start_failed;
    agg_started = true;
    nw_started = start_workers(&p, &trc, err, sizeof err);
    if (nw_started < p.nw)
        goto start_failed;
    if (!quiet)
        fprintf(stderr, "triage: %s\n", err);
    if (!quiet) {
        rep = (StatsReporter){ .stats = &p.stats, .queues = p.in_rings, .nqueues = p.nw,
                               .dropped = &disp.dropped_lines, .out = stderr, .interval_ms = 1000 };
        stats_reporter_start(&rep);
    }

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
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
    if (corr_started)
        pthread_join(corr_thread, NULL);
    if (router_started)
        pthread_join(router_thread, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    stats_reporter_stop(&rep);

    if (read_errno > 0)
        fprintf(stderr, "triage: read error: %s\n", strerror(read_errno));
    if (p.router_errno)
        fprintf(stderr, "triage: write error in %s: %s\n", o->out_dir, strerror(p.router_errno));
    if (p.seq_gap)
        fprintf(stderr, "triage: internal error: batches missing at end of input\n");
    for (int i = 0; i < nw_started; i++)
        if (p.workers[i].pin_failed && !quiet)
            fprintf(stderr, "triage: warning: could not pin worker %d to CPU %d\n", i, p.workers[i].cpu);
    if (read_errno == 0 && !p.router_errno && !p.seq_gap) {
        uint64_t dropped = atomic_load(&disp.dropped_lines);
        uint64_t esc = 0;
        for (int b = 0; b < 4; b++)
            esc += correlator_count(p.corr, b);
        if (o->bench) {
            BenchResult br = { .engine = engine_kind_name(o->engine), .workers = p.nw, .batch = o->batch,
                               .lines = router_records(p.router), .bytes = bench_bytes,
                               .seconds = (double)(t_end.tv_sec - t_start.tv_sec) +
                                          (double)(t_end.tv_nsec - t_start.tv_nsec) / 1e9,
                               .latency = &hist };
            engine_dfa_info(prepared, &br.nfa_states, &br.dfa_groups, &br.dfa_states, &br.build_ms);
            bench_print_json(stdout, &br);
        }
        if (!quiet)
            fprintf(stderr, "triage: %llu lines, %llu alerts, %llu PII, %llu escalations, %llu dropped -> %s\n",
                    (unsigned long long)router_records(p.router),
                    (unsigned long long)router_alerts(p.router),
                    (unsigned long long)router_quarantined(p.router),
                    (unsigned long long)esc, (unsigned long long)dropped, o->out_dir);
        status = 0;
    }

done:
    if (p.router && router_close(p.router) != 0 && status == 0) {
        fprintf(stderr, "triage: write error while closing output files in %s\n", o->out_dir);
        status = 1;
    }
    reader_close(reader);
    correlator_free(p.corr);
    free(p.pending);
    spsc_free(p.to_corr);
    spsc_free(p.dropped);
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
