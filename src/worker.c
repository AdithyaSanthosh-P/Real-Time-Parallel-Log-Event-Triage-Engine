#include "worker.h"

#include <errno.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "batch.h"
#include "mask.h"

/* Only its address is used; max_align_t is aligned enough for a Job. */
static max_align_t eof_marker;
Job *const WORKER_EOF = (Job *)(void *)&eof_marker;

int worker_init(Worker *w, int id, const EnginePrepared *ep, const RuleSet *rs,
                size_t depth, atomic_bool *stop, char *err, size_t err_len)
{
    memset(w, 0, sizeof *w);
    w->id = id;
    w->cpu = -1;
    w->rs = rs;
    w->stop = stop;
    sb_init(&w->scratch);
    char sub[256];
    if (!(w->engine = engine_new(ep, sub, sizeof sub)) ||
        !(w->extractor = extractor_new(rs, sub, sizeof sub))) {
        snprintf(err, err_len, "worker %d: %s", id, sub);
        worker_destroy(w);
        return -1;
    }
    w->in = spsc_new(depth);
    w->out = spsc_new(depth);
    if (!w->in || !w->out) {
        snprintf(err, err_len, "worker %d: out of memory", id);
        worker_destroy(w);
        return -1;
    }
    return 0;
}

void worker_destroy(Worker *w)
{
    engine_free(w->engine);
    extractor_free(w->extractor);
    sb_free(&w->scratch);
    spsc_free(w->in);
    spsc_free(w->out);
    w->engine = NULL;
    w->extractor = NULL;
    w->in = w->out = NULL;
}

/* One line: the same steps, in the same order, as the single-threaded
 * pipeline of Phase 3 (match, then §3.4 post-match steps on matched lines). */
static void process_line(Worker *w, Job *job, uint32_t i)
{
    const Batch *b = &job->batch;
    ResultBatch *res = &job->res;
    const char *norm = batch_norm(b, i);
    size_t len = batch_norm_len(b, i);

    MatchResult *m = &res->m[i];
    memset(m, 0, sizeof *m);
    m->seq = b->seq_start + i;
    m->rule_mask = engine_match(w->engine, norm, len);
    res->subsumed[i] = 0;
    res->mask_len[i] = MASK_NONE;
    if (!m->rule_mask)
        return;

    extract_line(w->extractor, norm, len, m->rule_mask, m);
    m->ctx_mask = context_mask(w->rs, norm, len, m->rule_mask);
    res->subsumed[i] = extract_subsumed(w->extractor, norm, len, m->valid_mask);
    /* spec §7: PII values are masked in every sink */
    if (m->valid_mask && mask_needed(w->rs, m->valid_mask) &&
        mask_pii(w->extractor, w->rs, norm, len, m->valid_mask, &w->scratch) > 0 &&
        res->masked.len <= UINT32_MAX - w->scratch.len) {
        res->mask_off[i] = (uint32_t)res->masked.len;
        res->mask_len[i] = (uint32_t)w->scratch.len;
        sb_append(&res->masked, w->scratch.data, w->scratch.len);
    }
}

void worker_process(Worker *w, Job *job)
{
    sb_clear(&job->res.masked);
    for (uint32_t i = 0; i < job->batch.count; i++)
        process_line(w, job, i);
}

static void *worker_main(void *arg)
{
    Worker *w = arg;
    if (w->cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(w->cpu, &set);
        if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0)
            w->pin_failed = true;
    }
    for (;;) {
        void *item;
        if (!spsc_pop(w->in, &item, w->stop))
            break;                                   /* another stage failed */
        Job *job = item;
        if (job != WORKER_EOF)
            worker_process(w, job);
        if (!spsc_push(w->out, job, w->stop) || job == WORKER_EOF)
            break;
    }
    return NULL;
}

int worker_start(Worker *w)
{
    int rc = pthread_create(&w->thread, NULL, worker_main, w);
    w->started = rc == 0;
    return rc;
}

void worker_join(Worker *w)
{
    if (w->started)
        pthread_join(w->thread, NULL);
    w->started = false;
}

/* ---- CPU order for pinning ---- */

/* sysfs topology value for `cpu`, or -1. */
static long topo_value(int cpu, const char *name)
{
    char path[128];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/%s", cpu, name);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1)
        v = -1;
    fclose(f);
    return v;
}

int worker_cpu_order(int *cpus, int max)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    if (max <= 0 || sched_getaffinity(0, sizeof set, &set) != 0)
        return 0;

    int allowed[CPU_SETSIZE];
    long core[CPU_SETSIZE], pkg[CPU_SETSIZE];
    int n = 0;
    for (int c = 0; c < CPU_SETSIZE; c++) {
        if (!CPU_ISSET(c, &set))
            continue;
        allowed[n] = c;
        core[n] = topo_value(c, "core_id");
        pkg[n] = topo_value(c, "physical_package_id");
        n++;
    }

    /* First pass: the first allowed CPU of every (package, core) pair;
     * second pass: everything else. Both in numeric CPU order. */
    bool taken[CPU_SETSIZE] = { false };
    int out = 0;
    for (int pass = 0; pass < 2 && out < max; pass++) {
        for (int i = 0; i < n && out < max; i++) {
            if (taken[i])
                continue;
            bool first_of_core = true;
            if (pass == 0 && core[i] >= 0) {
                for (int j = 0; j < i; j++)
                    if (core[j] == core[i] && pkg[j] == pkg[i]) {
                        first_of_core = false;
                        break;
                    }
            }
            if (pass == 0 && !first_of_core)
                continue;
            taken[i] = true;
            cpus[out++] = allowed[i];
        }
    }
    return out;
}
