#include "stats.h"

#include <time.h>

void stats_init(Stats *s)
{
    atomic_init(&s->lines_out, 0);
    for (int c = 0; c < N_CATEGORIES; c++)
        atomic_init(&s->cat[c], 0);
}

void stats_count_line(Stats *s, uint32_t label_mask)
{
    atomic_fetch_add_explicit(&s->lines_out, 1, memory_order_relaxed);
    for (int c = 0; c < N_CATEGORIES; c++)
        if (label_mask & (uint32_t)cat_from_index(c))
            atomic_fetch_add_explicit(&s->cat[c], 1, memory_order_relaxed);
}

/* Short names keep the line within a terminal width. */
static const char *const SHORT[N_CATEGORIES] = { "AUTH", "SEC", "ERR", "PERF", "PII" };

void stats_format(const StatsSnapshot *prev, const StatsSnapshot *now, double secs, StrBuf *out)
{
    sb_clear(out);
    double rate = secs > 0 ? (double)(now->lines_out - prev->lines_out) / secs : 0.0;
    sb_printf(out, "triage: %.0f lines/s | queues", rate);
    for (int i = 0; i < now->nqueues; i++)
        sb_printf(out, " %zu", now->queue[i]);
    sb_puts(out, " |");
    for (int c = 0; c < N_CATEGORIES; c++)
        sb_printf(out, " %s %llu", SHORT[c], (unsigned long long)now->cat[c]);
    sb_printf(out, " | dropped %llu", (unsigned long long)now->dropped);
}

static void snapshot(StatsReporter *r, StatsSnapshot *s)
{
    s->lines_out = atomic_load_explicit(&r->stats->lines_out, memory_order_relaxed);
    for (int c = 0; c < N_CATEGORIES; c++)
        s->cat[c] = atomic_load_explicit(&r->stats->cat[c], memory_order_relaxed);
    s->dropped = r->dropped ? atomic_load_explicit(r->dropped, memory_order_relaxed) : 0;
    s->nqueues = r->nqueues < 256 ? r->nqueues : 256;
    for (int i = 0; i < s->nqueues; i++)
        s->queue[i] = spsc_size(r->queues[i]);
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

#define TICK_MS 20      /* how quickly the reporter notices `done` */

static void *reporter_main(void *arg)
{
    StatsReporter *r = arg;
    StatsSnapshot prev, cur;
    StrBuf line;
    sb_init(&line);
    snapshot(r, &prev);
    double t_prev = now_s();
    long waited = 0;
    while (!atomic_load_explicit(&r->done, memory_order_acquire)) {
        struct timespec ts = { 0, TICK_MS * 1000000L };
        nanosleep(&ts, NULL);
        waited += TICK_MS;
        if (waited < r->interval_ms)
            continue;
        waited = 0;
        double t = now_s();
        snapshot(r, &cur);
        stats_format(&prev, &cur, t - t_prev, &line);
        if (!line.oom)
            fprintf(r->out, "%.*s\n", (int)line.len, line.data);
        prev = cur;
        t_prev = t;
    }
    sb_free(&line);
    return NULL;
}

int stats_reporter_start(StatsReporter *r)
{
    atomic_init(&r->done, false);
    int rc = pthread_create(&r->thread, NULL, reporter_main, r);
    r->started = rc == 0;
    return rc;
}

void stats_reporter_stop(StatsReporter *r)
{
    if (!r || !r->started)
        return;
    atomic_store_explicit(&r->done, true, memory_order_release);
    pthread_join(r->thread, NULL);
    r->started = false;
}
