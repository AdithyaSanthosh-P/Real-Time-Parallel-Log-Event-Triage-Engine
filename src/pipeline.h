#ifndef PIPELINE_H
#define PIPELINE_H

#include "cli.h"

/* Run the whole pipeline for the parsed options: rules -> reader -> engine ->
 * aggregator -> router, then print a one-line summary on stderr (unless
 * --quiet). T1.7: everything runs in the calling thread; the parallel runtime
 * (Phase 4) replaces the body, not the interface.
 *
 * Returns the process exit status: 0 on success, 1 on a runtime error (bad
 * rule file, unreadable input, unwritable output directory, ...), with the
 * reason printed on stderr. */
int pipeline_run(const Options *o);

#endif /* PIPELINE_H */
