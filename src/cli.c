#include "cli.h"

void cli_print_usage(FILE *out)
{
    fputs(
        "Usage: triage [options] [INPUT|-]\n"
        "\n"
        "Classify streaming log lines into AUTH_FAILURE, SECURITY_THREAT, ERROR,\n"
        "PERF_WARNING and PII. INPUT is a file path; '-' or no argument reads stdin.\n"
        "\n"
        "Options:\n"
        "  -w N                 number of worker threads\n"
        "  -b BATCH             lines per batch\n"
        "  --batch-delay MS     flush a partial batch after MS milliseconds\n"
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
        "  --quiet              suppress the periodic stats line on stderr\n"
        "  -h, --help           show this help and exit\n",
        out);
}
