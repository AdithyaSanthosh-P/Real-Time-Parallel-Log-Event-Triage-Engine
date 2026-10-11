#include "cli.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"

void cli_print_usage(FILE *out)
{
    fputs(
        "Usage: triage [options] [INPUT|-]\n"
        "\n"
        "Classify streaming log lines into AUTH_FAILURE, SECURITY_THREAT, ERROR,\n"
        "PERF_WARNING and PII. INPUT is a file path; '-' or no argument reads stdin.\n"
        "\n"
        "Options:\n"
        "  -w N                 number of worker threads (default: 1)\n"
        "  -b BATCH             lines per batch (default: 256)\n"
        "  --batch-delay MS     flush a partial batch after MS milliseconds (default: 5)\n"
        "  --engine dfa|posix|fork\n"
        "                       matching engine (default: dfa)\n"
        "  --dispatch rr|least  batch dispatch policy: round-robin or least-loaded\n"
        "  --pin | --no-pin     pin worker threads to CPUs (or not)\n"
        "  --drop               drop batches instead of blocking when queues are full\n"
        "  --ordered            emit results in input order\n"
        "  -r FILE              rule file (default: rules/default.rules)\n"
        "  -o DIR               output directory (default: out/)\n"
        "  --tau X              confidence threshold for a label (default: 0.5)\n"
        "  --max-states N       DFA state cap before rules are split into groups\n"
        "  --dump-dot FILE      write NFA/DFA as Graphviz DOT and exit\n"
        "  --bench              disable sinks; print throughput/latency JSON on stdout\n"
        "  --quiet              suppress progress output on stderr\n"
        "  -h, --help           show this help and exit\n"
        "\n"
        "Exit status: 0 success, 1 runtime error, 2 usage error or option not yet implemented.\n",
        out);
}

/* ---- option parsing ---- */

typedef enum {
    O_WORKERS, O_BATCH, O_BATCH_DELAY, O_ENGINE, O_DISPATCH, O_PIN, O_NO_PIN,
    O_DROP, O_ORDERED, O_RULES, O_OUT, O_TAU, O_MAX_STATES, O_DUMP_DOT,
    O_BENCH, O_QUIET, O_HELP
} OptId;

static const struct {
    const char *name;        /* long name without the dashes, or NULL */
    char        shortname;   /* 0 if none */
    bool        has_value;
    OptId       id;
} OPTS[] = {
    { NULL,          'w', true,  O_WORKERS },
    { NULL,          'b', true,  O_BATCH },
    { "batch-delay", 0,   true,  O_BATCH_DELAY },
    { "engine",      0,   true,  O_ENGINE },
    { "dispatch",    0,   true,  O_DISPATCH },
    { "pin",         0,   false, O_PIN },
    { "no-pin",      0,   false, O_NO_PIN },
    { "drop",        0,   false, O_DROP },
    { "ordered",     0,   false, O_ORDERED },
    { NULL,          'r', true,  O_RULES },
    { NULL,          'o', true,  O_OUT },
    { "tau",         0,   true,  O_TAU },
    { "max-states",  0,   true,  O_MAX_STATES },
    { "dump-dot",    0,   true,  O_DUMP_DOT },
    { "bench",       0,   false, O_BENCH },
    { "quiet",       0,   false, O_QUIET },
    { "help",        'h', false, O_HELP },
};

static bool parse_long(const char *s, long min, long max, long *out)
{
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || errno == ERANGE || v < min || v > max)
        return false;
    *out = v;
    return true;
}

static void set_defaults(Options *o)
{
    memset(o, 0, sizeof *o);
    o->workers = 1;
    o->batch = 256;
    o->batch_delay_ms = 5;
    o->engine = ENGINE_DFA;
    o->dispatch = DISPATCH_RR;
    o->pin = true;
    o->rules_path = "rules/default.rules";
    o->out_dir = "out/";
    o->tau = 0.5;
}

/* Apply one option. `name` is for messages. Returns 0 or -1 (err filled). */
static int apply(Options *o, OptId id, const char *name, const char *val, char *err, size_t n)
{
    long v;
    switch (id) {
    case O_WORKERS:
        if (!parse_long(val, 1, 256, &v)) { snprintf(err, n, "%s: expected 1..256, got '%s'", name, val); return -1; }
        o->workers = (int)v;
        break;
    case O_BATCH:
        if (!parse_long(val, 1, BATCH_MAX, &v)) {
            snprintf(err, n, "%s: expected 1..%d, got '%s'", name, BATCH_MAX, val);
            return -1;
        }
        o->batch = (int)v;
        break;
    case O_BATCH_DELAY:
        if (!parse_long(val, 0, 60000, &v)) { snprintf(err, n, "%s: expected 0..60000 (ms), got '%s'", name, val); return -1; }
        o->batch_delay_ms = (int)v;
        o->given |= GIVEN_BATCH_DELAY;
        break;
    case O_ENGINE:
        if (strcmp(val, "dfa") == 0)        o->engine = ENGINE_DFA;
        else if (strcmp(val, "posix") == 0) o->engine = ENGINE_POSIX;
        else if (strcmp(val, "fork") == 0)  o->engine = ENGINE_FORK;
        else { snprintf(err, n, "%s: expected dfa, posix or fork, got '%s'", name, val); return -1; }
        break;
    case O_DISPATCH:
        if (strcmp(val, "rr") == 0)         o->dispatch = DISPATCH_RR;
        else if (strcmp(val, "least") == 0) o->dispatch = DISPATCH_LEAST;
        else { snprintf(err, n, "%s: expected rr or least, got '%s'", name, val); return -1; }
        o->given |= GIVEN_DISPATCH;
        break;
    case O_PIN:     o->pin = true;  o->given |= GIVEN_PIN; break;
    case O_NO_PIN:  o->pin = false; o->given |= GIVEN_PIN; break;
    case O_DROP:    o->drop = true; break;
    case O_ORDERED: o->ordered = true; break;
    case O_RULES:   o->rules_path = val; break;
    case O_OUT:     o->out_dir = val; break;
    case O_TAU: {
        char *end = NULL;
        double t = strtod(val, &end);
        if (*val == '\0' || *end != '\0' || !(t >= 0.0 && t <= 1.0)) {
            snprintf(err, n, "%s: expected a number in [0, 1], got '%s'", name, val);
            return -1;
        }
        o->tau = t;
        o->given |= GIVEN_TAU;
        break;
    }
    case O_MAX_STATES:
        if (!parse_long(val, 1, 1L << 30, &v)) { snprintf(err, n, "%s: expected 1..2^30, got '%s'", name, val); return -1; }
        o->max_states = v;
        o->given |= GIVEN_MAX_STATES;
        break;
    case O_DUMP_DOT: o->dump_dot = val; break;
    case O_BENCH:    o->bench = true; break;
    case O_QUIET:    o->quiet = true; break;
    case O_HELP:     o->help = true; break;
    }
    return 0;
}

int cli_parse(int argc, char **argv, Options *o, char *err, size_t err_len)
{
    set_defaults(o);
    bool positional_only = false;
    bool have_input = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (positional_only || arg[0] != '-' || arg[1] == '\0') {   /* INPUT, or "-" */
            if (have_input) {
                snprintf(err, err_len, "unexpected extra argument '%s'", arg);
                return -1;
            }
            o->input = arg;
            have_input = true;
            continue;
        }
        if (strcmp(arg, "--") == 0) {
            positional_only = true;
            continue;
        }

        /* find the option and its inline value, if any */
        int found = -1;
        const char *inline_val = NULL;
        char shown[64];
        if (arg[1] == '-') {
            const char *name = arg + 2;
            const char *eq = strchr(name, '=');
            size_t nlen = eq ? (size_t)(eq - name) : strlen(name);
            for (size_t k = 0; k < sizeof OPTS / sizeof OPTS[0]; k++)
                if (OPTS[k].name && strlen(OPTS[k].name) == nlen && strncmp(OPTS[k].name, name, nlen) == 0)
                    found = (int)k;
            if (eq)
                inline_val = eq + 1;
            snprintf(shown, sizeof shown, "--%.*s", (int)nlen, name);
        } else {
            for (size_t k = 0; k < sizeof OPTS / sizeof OPTS[0]; k++)
                if (OPTS[k].shortname == arg[1])
                    found = (int)k;
            if (arg[2] != '\0')
                inline_val = arg + 2;
            snprintf(shown, sizeof shown, "-%c", arg[1]);
        }
        if (found < 0) {
            snprintf(err, err_len, "unknown option '%s'", arg);
            return -1;
        }

        const char *val = NULL;
        if (OPTS[found].has_value) {
            if (inline_val) {
                val = inline_val;
            } else if (i + 1 < argc) {
                val = argv[++i];
            } else {
                snprintf(err, err_len, "option '%s' needs a value", shown);
                return -1;
            }
        } else if (inline_val) {
            snprintf(err, err_len, "option '%s' does not take a value", shown);
            return -1;
        }
        if (apply(o, OPTS[found].id, shown, val, err, err_len) != 0)
            return -1;
    }
    return 0;
}

const char *cli_unimplemented(const Options *o)
{
    if (o->help)
        return NULL;
    if (o->engine != ENGINE_POSIX)       return o->engine == ENGINE_DFA ? "--engine dfa" : "--engine fork";
    if (o->workers > 1)                  return "-w N with N > 1";
    if (o->given & GIVEN_BATCH_DELAY)    return "--batch-delay";
    if (o->given & GIVEN_DISPATCH)       return "--dispatch";
    if (o->given & GIVEN_PIN)            return o->pin ? "--pin" : "--no-pin";
    if (o->drop)                         return "--drop";
    if (o->ordered)                      return "--ordered";
    if (o->given & GIVEN_TAU)            return "--tau";
    if (o->given & GIVEN_MAX_STATES)     return "--max-states";
    if (o->dump_dot)                     return "--dump-dot";
    if (o->bench)                        return "--bench";
    return NULL;
}
