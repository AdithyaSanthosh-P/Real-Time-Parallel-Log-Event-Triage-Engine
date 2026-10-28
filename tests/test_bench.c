#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_main.h"
#include "bench.h"

static void test_hist(void)
{
    BenchHist h;
    memset(&h, 0, sizeof h);
    CHECK_EQ_INT((int)bench_hist_quantile(&h, 0.5), 0);              /* empty */
    for (int i = 1; i <= 1000; i++)
        bench_hist_add(&h, (uint64_t)i * 1000);                      /* 1 us .. 1 ms, uniform */
    CHECK_EQ_INT((int)h.count, 1000);
    CHECK_EQ_INT((int)h.max_ns, 1000000);
    /* each quantile within one bucket (~9 %) above the exact value, never below it */
    static const double q[] = { 0.5, 0.9, 0.99, 0.999 };
    for (int k = 0; k < 4; k++) {
        double exact = q[k] * 1000 * 1000.0, got = (double)bench_hist_quantile(&h, q[k]);
        CHECK(got >= exact * 0.999);
        CHECK(got <= exact * 1.10);
    }
    CHECK(bench_hist_quantile(&h, 1.0) == h.max_ns);                 /* top quantile is the maximum */
    bench_hist_add(&h, 0);                                           /* 0 ns and huge values stay in range */
    bench_hist_add(&h, UINT64_MAX / 2);
    CHECK_EQ_INT((int)h.count, 1002);
}

static void test_preload(void)
{
    char path[] = "/tmp/triage_bench_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    static const char text[] = "one\ntwo\nthree";
    CHECK(write(fd, text, sizeof text - 1) == (ssize_t)(sizeof text - 1));
    close(fd);

    size_t n = 0;
    int mem = bench_preload(path, &n);
    CHECK(mem >= 0);
    CHECK_EQ_INT((int)n, (int)(sizeof text - 1));
    char buf[64];
    ssize_t k = read(mem, buf, sizeof buf);                           /* positioned at the start */
    CHECK_EQ_INT((int)k, (int)(sizeof text - 1));
    CHECK(memcmp(buf, text, sizeof text - 1) == 0);
    close(mem);
    unlink(path);

    CHECK(bench_preload("/nonexistent/file", &n) < 0);
}

static void test_json(void)
{
    BenchHist h;
    memset(&h, 0, sizeof h);
    bench_hist_add(&h, 2000);
    BenchResult r = { .engine = "dfa", .workers = 4, .batch = 256, .lines = 1000, .bytes = 50000,
                      .seconds = 0.5, .nfa_states = 10, .dfa_groups = 2, .dfa_states = 77,
                      .build_ms = 1.5, .latency = &h };
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    bench_print_json(f, &r);
    fclose(f);
    CHECK(strstr(buf, "\"engine\":\"dfa\"") != NULL);
    CHECK(strstr(buf, "\"lines\":1000") != NULL);
    CHECK(strstr(buf, "\"lines_per_s\":2000.0") != NULL);
    CHECK(strstr(buf, "\"dfa_states\":77") != NULL);
    CHECK(buf[len - 1] == '\n' && memchr(buf, '\n', len) == buf + len - 1);   /* exactly one line */
    free(buf);
}

int main(void)
{
    test_hist();
    test_preload();
    test_json();
    return TEST_DONE();
}
