#ifndef CLI_H
#define CLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "engine.h"

typedef enum { DISPATCH_RR, DISPATCH_LEAST } DispatchKind;

/* Bits of Options.given: options that were written on the command line. */
enum {
    GIVEN_BATCH_DELAY = 1 << 0,
    GIVEN_DISPATCH    = 1 << 1,
    GIVEN_PIN         = 1 << 2,
    GIVEN_TAU         = 1 << 3,
    GIVEN_MAX_STATES  = 1 << 4,
};

/* Parsed command line (spec §8). Strings point into argv. */
typedef struct {
    int          workers;          /* -w, default 1 */
    int          batch;            /* -b, lines per batch, default 256 */
    int          batch_delay_ms;   /* --batch-delay, default 5 */
    EngineKind   engine;           /* --engine, default dfa */
    DispatchKind dispatch;         /* --dispatch, default rr */
    bool         pin;              /* --pin / --no-pin, default on */
    bool         drop;
    bool         ordered;
    bool         bench;
    bool         quiet;
    bool         help;
    const char  *rules_path;       /* -r, default rules/default.rules */
    const char  *out_dir;          /* -o, default out/ */
    const char  *dump_dot;         /* --dump-dot FILE, or NULL */
    const char  *input;            /* INPUT, or NULL / "-" for stdin */
    double       tau;              /* --tau, default 0.5 */
    long         max_states;       /* --max-states, default 0 = built-in cap */
    unsigned     given;            /* GIVEN_* bits */
} Options;

/* Print the command-line synopsis (spec §8) to `out`. */
void cli_print_usage(FILE *out);

/* Parse argv. Long options accept "--opt value" and "--opt=value"; short
 * ones accept "-w 4" and "-w4". Returns 0, or -1 with a message in `err`
 * (unknown option, missing or out-of-range value, extra argument). */
int cli_parse(int argc, char **argv, Options *o, char *err, size_t err_len);

/* Name of the first option given that the current build does not implement
 * yet (shown as "not yet implemented", exit status 2), or NULL. */
const char *cli_unimplemented(const Options *o);

#endif /* CLI_H */
