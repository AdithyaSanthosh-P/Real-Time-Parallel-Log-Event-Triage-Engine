#include "bench.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int bench_preload(const char *path, size_t *bytes)
{
    int in = (path && strcmp(path, "-") != 0) ? open(path, O_RDONLY | O_CLOEXEC) : STDIN_FILENO;
    if (in < 0)
        return -1;
    int mem = memfd_create("triage-bench-input", MFD_CLOEXEC);
    if (mem < 0) {
        if (in != STDIN_FILENO)
            close(in);
        return -1;
    }
    static char buf[1 << 16];
    size_t total = 0;
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            goto fail;
        if (n == 0)
            break;
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(mem, buf + off, (size_t)(n - off));
            if (w < 0 && errno == EINTR)
                continue;
            if (w < 0)
                goto fail;
            off += w;
        }
        total += (size_t)n;
    }
    if (in != STDIN_FILENO)
        close(in);
    if (lseek(mem, 0, SEEK_SET) < 0) {
        close(mem);
        return -1;
    }
    *bytes = total;
    return mem;

fail: {
        int e = errno;
        if (in != STDIN_FILENO)
            close(in);
        close(mem);
        errno = e;
        return -1;
    }
}

/* bucket b covers [2^(b/8) , 2^((b+1)/8)) ns */
static int bucket_of(uint64_t ns)
{
    if (ns < 1)
        ns = 1;
    int b = (int)floor(log2((double)ns) * 8.0);
    return b >= BENCH_HIST_BUCKETS ? BENCH_HIST_BUCKETS - 1 : b;
}

void bench_hist_add(BenchHist *h, uint64_t ns)
{
    h->bucket[bucket_of(ns)]++;
    h->count++;
    if (ns > h->max_ns)
        h->max_ns = ns;
}

uint64_t bench_hist_quantile(const BenchHist *h, double q)
{
    if (h->count == 0)
        return 0;
    uint64_t need = (uint64_t)ceil(q * (double)h->count);
    if (need < 1)
        need = 1;
    uint64_t seen = 0;
    for (int b = 0; b < BENCH_HIST_BUCKETS; b++) {
        seen += h->bucket[b];
        if (seen >= need) {
            uint64_t edge = (uint64_t)ceil(pow(2.0, (double)(b + 1) / 8.0));
            return edge > h->max_ns ? h->max_ns : edge;
        }
    }
    return h->max_ns;
}

void bench_print_json(FILE *out, const BenchResult *r)
{
    double s = r->seconds > 0 ? r->seconds : 1e-9;
    fprintf(out, "{\"engine\":\"%s\",\"workers\":%d,\"batch\":%d,\"lines\":%llu,\"bytes\":%llu,"
                 "\"seconds\":%.6f,\"lines_per_s\":%.1f,\"mb_per_s\":%.3f",
            r->engine, r->workers, r->batch, (unsigned long long)r->lines, (unsigned long long)r->bytes,
            r->seconds, (double)r->lines / s, (double)r->bytes / 1e6 / s);
    if (r->latency && r->latency->count)
        fprintf(out, ",\"latency_us\":{\"p50\":%.2f,\"p90\":%.2f,\"p99\":%.2f,\"p99.9\":%.2f,\"max\":%.2f}",
                (double)bench_hist_quantile(r->latency, 0.50) / 1e3, (double)bench_hist_quantile(r->latency, 0.90) / 1e3,
                (double)bench_hist_quantile(r->latency, 0.99) / 1e3, (double)bench_hist_quantile(r->latency, 0.999) / 1e3,
                (double)r->latency->max_ns / 1e3);
    fprintf(out, ",\"nfa_states\":%d,\"dfa_groups\":%d,\"dfa_states\":%lld,\"build_ms\":%.1f}\n",
            r->nfa_states, r->dfa_groups, r->dfa_states, r->build_ms);
}
