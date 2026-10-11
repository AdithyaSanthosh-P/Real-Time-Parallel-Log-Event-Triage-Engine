#include <stdio.h>

#include "cli.h"
#include "pipeline.h"

int main(int argc, char **argv)
{
    Options opts;
    char err[256];

    if (cli_parse(argc, argv, &opts, err, sizeof err) != 0) {
        fprintf(stderr, "triage: %s\nTry 'triage --help'.\n", err);
        return 2;
    }
    if (opts.help) {
        cli_print_usage(stdout);
        return 0;
    }
    const char *todo = cli_unimplemented(&opts);
    if (todo) {
        fprintf(stderr, "triage: %s: not yet implemented\n", todo);
        return 2;
    }
    if (opts.dump_dot)
        return pipeline_dump_dot(&opts);
    return pipeline_run(&opts);
}
