#include <stdio.h>
#include <string.h>

#include "cli.h"

/* T0.1 stub: only --help is handled. Full CLI parsing and the pipeline
 * wiring arrive in T1.7. */
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            cli_print_usage(stdout);
            return 0;
        }
    }

    fprintf(stderr, "triage: not yet implemented (try --help)\n");
    return 2;
}
