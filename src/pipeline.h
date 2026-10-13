#ifndef PIPELINE_H
#define PIPELINE_H

#include "cli.h"

/* Run the whole pipeline for the parsed options: rules -> reader -> engine ->
 * aggregator -> router, then print a one-line summary on stderr (unless
 * --quiet). The calling thread reads and dispatches; workers, aggregator,
 * router and the stats line run on their own threads (see pipeline.c).
 *
 * Returns the process exit status: 0 on success, 1 on a runtime error (bad
 * rule file, unreadable input, unwritable output directory, ...), with the
 * reason printed on stderr. */
int pipeline_run(const Options *o);

/* --dump-dot FILE: compile the rules (NFA and DFA, honouring --max-states)
 * and write them as Graphviz DOT to FILE ("-" = stdout), without reading
 * any input. Returns the exit status (0 ok, 1 error). */
int pipeline_dump_dot(const Options *o);

#endif /* PIPELINE_H */
