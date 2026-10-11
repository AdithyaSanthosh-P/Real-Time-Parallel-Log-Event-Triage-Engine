#include <stdio.h>
#include <stdlib.h>

#include "test_main.h"
#include "cli.h"

/* Capture cli_print_usage() output in memory and check that every flag in
 * the spec §8 synopsis is documented. */
static void test_usage(void)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    CHECK(f != NULL);
    if (f == NULL)
        return;

    cli_print_usage(f);
    fclose(f);
    CHECK(buf != NULL && len > 0);

    static const char *const flags[] = {
        "-w N", "-b BATCH", "--batch-delay", "--engine dfa|posix|fork",
        "--dispatch rr|least", "--pin", "--no-pin", "--drop", "--ordered",
        "-r FILE", "-o DIR", "--tau", "--max-states", "--dump-dot",
        "--bench", "--quiet", "INPUT|-",
    };
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++) {
        int found = buf != NULL && strstr(buf, flags[i]) != NULL;
        if (!found)
            fprintf(stderr, "usage text is missing: %s\n", flags[i]);
        CHECK(found);
    }
    free(buf);
}

/* Parse a NULL-terminated argument list (argv[0] is supplied). */
static int parse(Options *o, char *err, const char *a1, ...)
{
    char *argv[32] = { "triage" };
    int argc = 1;
    __builtin_va_list ap;
    __builtin_va_start(ap, a1);
    for (const char *a = a1; a; a = __builtin_va_arg(ap, const char *))
        argv[argc++] = (char *)a;
    __builtin_va_end(ap);
    err[0] = '\0';
    return cli_parse(argc, argv, o, err, 256);
}

#define PARSE(o, err, ...) parse((o), (err), __VA_ARGS__, (const char *)NULL)

static void test_defaults(void)
{
    Options o;
    char err[256];
    CHECK_EQ_INT(parse(&o, err, NULL), 0);
    CHECK_EQ_INT(o.workers, 1);
    CHECK_EQ_INT(o.batch, 256);
    CHECK_EQ_INT(o.batch_delay_ms, 5);
    CHECK_EQ_INT(o.engine, ENGINE_DFA);
    CHECK_EQ_INT(o.dispatch, DISPATCH_RR);
    CHECK(o.pin && !o.drop && !o.ordered && !o.bench && !o.quiet && !o.help);
    CHECK_STR_EQ(o.rules_path, "rules/default.rules");
    CHECK_STR_EQ(o.out_dir, "out/");
    CHECK(o.dump_dot == NULL && o.input == NULL);
    CHECK(o.tau == 0.5);
    CHECK_EQ_INT(o.max_states, 0);
    CHECK_EQ_INT(o.given, 0);
    CHECK(cli_unimplemented(&o) != NULL);              /* the default engine, dfa, is not built yet */
}

static void test_valid_options(void)
{
    Options o;
    char err[256];

    CHECK_EQ_INT(PARSE(&o, err, "--engine", "posix", "-w", "1", "-b", "64", "-r", "my.rules",
                       "-o", "outdir", "--quiet", "in.log"), 0);
    CHECK_EQ_INT(o.engine, ENGINE_POSIX);
    CHECK_EQ_INT(o.workers, 1);
    CHECK_EQ_INT(o.batch, 64);
    CHECK_STR_EQ(o.rules_path, "my.rules");
    CHECK_STR_EQ(o.out_dir, "outdir");
    CHECK(o.quiet);
    CHECK_STR_EQ(o.input, "in.log");
    CHECK(cli_unimplemented(&o) == NULL);

    /* attached short values and --opt=value */
    CHECK_EQ_INT(PARSE(&o, err, "-w8", "-b16", "-rfoo.rules", "-oout2", "--engine=fork",
                       "--tau=0.7", "--batch-delay=20", "--dispatch=least", "--max-states=5000",
                       "--dump-dot=g.dot"), 0);
    CHECK_EQ_INT(o.workers, 8);
    CHECK_EQ_INT(o.batch, 16);
    CHECK_STR_EQ(o.rules_path, "foo.rules");
    CHECK_STR_EQ(o.out_dir, "out2");
    CHECK_EQ_INT(o.engine, ENGINE_FORK);
    CHECK(o.tau == 0.7);
    CHECK_EQ_INT(o.batch_delay_ms, 20);
    CHECK_EQ_INT(o.dispatch, DISPATCH_LEAST);
    CHECK_EQ_INT(o.max_states, 5000);
    CHECK_STR_EQ(o.dump_dot, "g.dot");
    CHECK_EQ_INT(o.given, GIVEN_BATCH_DELAY | GIVEN_DISPATCH | GIVEN_TAU | GIVEN_MAX_STATES);
    CHECK(o.input == NULL);

    /* flags */
    CHECK_EQ_INT(PARSE(&o, err, "--drop", "--ordered", "--bench", "--no-pin"), 0);
    CHECK(o.drop && o.ordered && o.bench && !o.pin);
    CHECK_EQ_INT(o.given, GIVEN_PIN);
    CHECK_EQ_INT(PARSE(&o, err, "--no-pin", "--pin"), 0);
    CHECK(o.pin);

    /* help, and stdin as '-' / '--' handling */
    CHECK_EQ_INT(PARSE(&o, err, "-h"), 0);
    CHECK(o.help);
    CHECK_EQ_INT(PARSE(&o, err, "--help"), 0);
    CHECK(o.help);
    CHECK_EQ_INT(PARSE(&o, err, "-"), 0);
    CHECK_STR_EQ(o.input, "-");
    CHECK_EQ_INT(PARSE(&o, err, "--", "-weird-name.log"), 0);
    CHECK_STR_EQ(o.input, "-weird-name.log");

    /* options after the input are fine; later options win */
    CHECK_EQ_INT(PARSE(&o, err, "in.log", "-w", "3", "-w", "2"), 0);
    CHECK_EQ_INT(o.workers, 2);
    CHECK_STR_EQ(o.input, "in.log");
}

static void test_errors(void)
{
    Options o;
    char err[256];
    struct { const char *a, *b, *msg; } bad[] = {
        { "--bogus", NULL,        "unknown option '--bogus'" },
        { "-x", NULL,             "unknown option '-x'" },
        { "-w", NULL,             "option '-w' needs a value" },
        { "--engine", NULL,       "option '--engine' needs a value" },
        { "-w", "0",              "-w: expected 1..256" },
        { "-w", "257",            "-w: expected 1..256" },
        { "-w", "four",           "-w: expected 1..256, got 'four'" },
        { "-w", "4x",             "got '4x'" },
        { "-b", "0",              "-b: expected 1..1024" },
        { "-b", "99999",          "-b: expected 1..1024" },
        { "--engine", "pcre",     "--engine: expected dfa, posix or fork, got 'pcre'" },
        { "--dispatch", "random", "--dispatch: expected rr or least" },
        { "--tau", "1.5",         "--tau: expected a number in [0, 1]" },
        { "--tau", "-0.1",        "--tau: expected a number in [0, 1]" },
        { "--tau", "abc",         "--tau: expected a number in [0, 1]" },
        { "--batch-delay", "-1",  "--batch-delay: expected 0..60000" },
        { "--max-states", "0",    "--max-states: expected 1..2^30" },
        { "--drop=yes", NULL,     "option '--drop' does not take a value" },
        { "--quiet=1", NULL,      "option '--quiet' does not take a value" },
        { "-hx", NULL,            "option '-h' does not take a value" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = bad[i].b ? PARSE(&o, err, bad[i].a, bad[i].b) : PARSE(&o, err, bad[i].a);
        CHECK_EQ_INT(rc, -1);
        int found = strstr(err, bad[i].msg) != NULL;
        if (!found)
            fprintf(stderr, "  %s %s: got \"%s\", want \"%s\"\n", bad[i].a,
                    bad[i].b ? bad[i].b : "", err, bad[i].msg);
        CHECK(found);
    }

    CHECK_EQ_INT(PARSE(&o, err, "a.log", "b.log"), -1);
    CHECK(strstr(err, "unexpected extra argument 'b.log'") != NULL);
    CHECK_EQ_INT(PARSE(&o, err, "-", "x.log"), -1);
}

static void test_unimplemented(void)
{
    Options o;
    char err[256];
    static const struct { const char *a, *b; const char *name; } nyi[] = {
        { "--engine", "dfa",      "--engine dfa" },
        { "--engine", "fork",     "--engine fork" },
        { "-w", "2",              "-w N with N > 1" },
        { "--batch-delay", "10",  "--batch-delay" },
        { "--dispatch", "least",  "--dispatch" },
        { "--pin", NULL,          "--pin" },
        { "--no-pin", NULL,       "--no-pin" },
        { "--drop", NULL,         "--drop" },
        { "--ordered", NULL,      "--ordered" },
        { "--tau", "0.4",         "--tau" },
        { "--max-states", "100",  "--max-states" },
        { "--dump-dot", "x.dot",  "--dump-dot" },
        { "--bench", NULL,        "--bench" },
    };
    for (size_t i = 0; i < sizeof nyi / sizeof nyi[0]; i++) {
        int rc = nyi[i].b ? PARSE(&o, err, "--engine", "posix", nyi[i].a, nyi[i].b)
                          : PARSE(&o, err, "--engine", "posix", nyi[i].a);
        if (strcmp(nyi[i].a, "--engine") == 0)             /* later --engine wins */
            rc = PARSE(&o, err, nyi[i].a, nyi[i].b);
        CHECK_EQ_INT(rc, 0);
        const char *got = cli_unimplemented(&o);
        CHECK(got != NULL);
        if (got)
            CHECK_STR_EQ(got, nyi[i].name);
    }

    /* what does work: posix, -w 1, -b, -r, -o, --quiet, input */
    CHECK_EQ_INT(PARSE(&o, err, "--engine", "posix", "-w", "1", "-b", "8", "--quiet", "x.log"), 0);
    CHECK(cli_unimplemented(&o) == NULL);

    /* --help never reports anything */
    CHECK_EQ_INT(PARSE(&o, err, "--help", "--bench"), 0);
    CHECK(cli_unimplemented(&o) == NULL);
}

int main(void)
{
    test_usage();
    test_defaults();
    test_valid_options();
    test_errors();
    test_unimplemented();
    return TEST_DONE();
}
