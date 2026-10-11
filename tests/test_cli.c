#include <stdio.h>
#include <stdlib.h>

#include "test_main.h"
#include "../src/cli.h"

/* Capture cli_print_usage() output in memory and check that every flag in
 * the spec §8 synopsis is documented. */
int main(void)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    CHECK(f != NULL);
    if (f == NULL)
        return 1;

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
    return TEST_DONE();
}
