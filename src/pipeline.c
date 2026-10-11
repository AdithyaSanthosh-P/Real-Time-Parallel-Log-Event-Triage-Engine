#include "pipeline.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "aggregate.h"
#include "batch.h"
#include "dfa.h"
#include "dot.h"
#include "engine.h"
#include "nfa.h"
#include "reader.h"
#include "route.h"
#include "rules.h"

int pipeline_run(const Options *o)
{
    int status = 1;
    char err[512];
    RuleSet rs;
    EnginePrepared *prepared = NULL;
    Engine *engine = NULL;
    Router *router = NULL;
    Reader *reader = NULL;
    Batch batch;
    bool batch_ready = false;
    Aggregator agg;

    if (rules_load(o->rules_path, &rs, err, sizeof err) != 0) {
        fprintf(stderr, "triage: %s\n", err);
        return 1;
    }
    aggregator_init(&agg, &rs);

    prepared = engine_prepare(o->engine, &rs, (int)o->max_states, err, sizeof err);
    if (!prepared) {
        fprintf(stderr, "triage: %s\n", err);
        goto done;
    }
    if (!o->quiet) {
        engine_describe(prepared, err, sizeof err);
        fprintf(stderr, "triage: %s\n", err);
    }
    engine = engine_new(prepared, err, sizeof err);
    if (!engine) {
        fprintf(stderr, "triage: %s\n", err);
        goto done;
    }
    router = router_open(o->out_dir, &rs, err, sizeof err);
    if (!router) {
        fprintf(stderr, "triage: %s\n", err);
        goto done;
    }
    reader = reader_open_path(o->input, 0);
    if (!reader) {
        fprintf(stderr, "triage: %s: %s\n", o->input ? o->input : "stdin", strerror(errno));
        goto done;
    }
    if (batch_init(&batch) != 0) {
        fprintf(stderr, "triage: out of memory\n");
        goto done;
    }
    batch_ready = true;

    int n;
    while ((n = reader_next_batch(reader, &batch, (uint32_t)o->batch)) > 0) {
        for (uint32_t i = 0; i < batch.count; i++) {
            uint64_t mask = engine_match(engine, batch_norm(&batch, i), batch_norm_len(&batch, i));
            Classified c;
            aggregator_classify(&agg, batch.seq_start + i, mask, &c);
            if (router_emit(router, &batch, i, &c) != 0) {
                fprintf(stderr, "triage: write error in %s: %s\n", o->out_dir, strerror(errno));
                goto done;
            }
        }
    }
    if (n < 0) {
        fprintf(stderr, "triage: read error: %s\n", strerror(errno));
        goto done;
    }

    if (!o->quiet)
        fprintf(stderr, "triage: %llu lines, %llu alerts -> %s\n",
                (unsigned long long)router_records(router),
                (unsigned long long)router_alerts(router), o->out_dir);
    status = 0;

done:
    if (router && router_close(router) != 0 && status == 0) {
        fprintf(stderr, "triage: write error while closing output files in %s\n", o->out_dir);
        status = 1;
    }
    if (batch_ready)
        batch_free(&batch);
    reader_close(reader);
    engine_free(engine);
    engine_prepared_free(prepared);
    rules_free(&rs);
    return status;
}

int pipeline_dump_dot(const Options *o)
{
    char err[512];
    RuleSet rs;
    if (rules_load(o->rules_path, &rs, err, sizeof err) != 0) {
        fprintf(stderr, "triage: %s\n", err);
        return 1;
    }
    int status = 1;
    Nfa nfa;
    DfaSet ds;
    bool have_nfa = false, have_dfa = false;
    FILE *f = NULL;

    if (nfa_build_rules(&nfa, &rs, err, sizeof err) != 0) {
        fprintf(stderr, "triage: %s\n", err);
        goto done;
    }
    have_nfa = true;
    if (dfaset_build(&nfa, (int)o->max_states, &ds, err, sizeof err) != 0) {
        fprintf(stderr, "triage: %s\n", err);
        goto done;
    }
    have_dfa = true;

    bool to_stdout = strcmp(o->dump_dot, "-") == 0;
    f = to_stdout ? stdout : fopen(o->dump_dot, "w");
    if (!f) {
        fprintf(stderr, "triage: %s: %s\n", o->dump_dot, strerror(errno));
        goto done;
    }
    int rc = dot_write(f, &rs, &nfa, &ds);
    if ((to_stdout ? fflush(f) : fclose(f)) != 0 || rc != 0) {
        fprintf(stderr, "triage: %s: write error\n", o->dump_dot);
        goto done;
    }
    if (!o->quiet)
        fprintf(stderr, "triage: wrote NFA (%d states) and %d DFA group(s) (%lld states) to %s\n",
                nfa.n, ds.ngroups, (long long)ds.total_states, o->dump_dot);
    status = 0;

done:
    if (have_dfa)
        dfaset_free(&ds);
    if (have_nfa)
        nfa_free(&nfa);
    rules_free(&rs);
    return status;
}
