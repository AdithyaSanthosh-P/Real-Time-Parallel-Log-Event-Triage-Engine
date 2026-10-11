#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_main.h"
#include "annot.h"
#include "engine.h"
#include "normalize.h"
#include "rules.h"

static RuleSet rs;

/* Through the engine facade, each implemented engine gives exactly the
 * annotated rule mask on every line of synthetic_small.log. */
static void check_engine(EngineKind kind, int max_states)
{
    char err[256], desc[256];
    EnginePrepared *p = engine_prepare(kind, &rs, max_states, err, sizeof err);
    CHECK(p != NULL);
    if (!p) {
        fprintf(stderr, "engine_prepare(%s): %s\n", engine_kind_name(kind), err);
        return;
    }
    engine_describe(p, desc, sizeof desc);
    fprintf(stderr, "%s\n", desc);
    CHECK(strncmp(desc, engine_kind_name(kind), strlen(engine_kind_name(kind))) == 0);

    /* two per-thread engines from one prepared state */
    Engine *a = engine_new(p, err, sizeof err);
    Engine *b = engine_new(p, err, sizeof err);
    CHECK(a != NULL && b != NULL);

    AnnotCorpus c;
    CHECK_EQ_INT(annot_load("tests/corpus/synthetic_small.log", &rs, &c), 0);
    char *norm = malloc(65536);
    for (size_t i = 0; a && b && i < c.n; i++) {
        size_t n = normalize_line(c.e[i].text, c.e[i].len, norm);
        uint64_t ma = engine_match(a, norm, n);
        uint64_t mb = engine_match(b, norm, n);
        tm_checks++;
        if (ma != c.e[i].expect || mb != ma) {
            tm_failures++;
            fprintf(stderr, "%s: line %zu: mask %#llx, expected %#llx\n", engine_kind_name(kind),
                    c.e[i].file_line, (unsigned long long)ma, (unsigned long long)c.e[i].expect);
        }
    }
    free(norm);
    annot_free(&c);
    engine_free(a);
    engine_free(b);
    engine_prepared_free(p);
}

int main(void)
{
    char err[256];
    if (rules_load("rules/default.rules", &rs, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    check_engine(ENGINE_POSIX, 0);
    check_engine(ENGINE_DFA, 0);
    check_engine(ENGINE_DFA, 500);            /* many small groups, same answers */

    /* not built yet */
    CHECK(engine_prepare(ENGINE_FORK, &rs, 0, err, sizeof err) == NULL);
    CHECK(strstr(err, "not implemented") != NULL);

    /* a cap too small for one rule is reported, not crashed on */
    CHECK(engine_prepare(ENGINE_DFA, &rs, 5, err, sizeof err) == NULL);
    CHECK(strstr(err, "--max-states") != NULL);

    /* unsupported syntax in a rule file is rejected by the dfa engine */
    static const char bad[] = "BAD\tERR\tLOW\t0.5\t-\t-\t-\t-\t\\d+\n";
    RuleSet brs;
    CHECK_EQ_INT(rules_parse(bad, sizeof bad - 1, "inline", &brs, err, sizeof err), 0);
    CHECK(engine_prepare(ENGINE_DFA, &brs, 0, err, sizeof err) == NULL);
    CHECK(strstr(err, "rule BAD: offset 0") != NULL && strstr(err, "\\d shorthand") != NULL);
    rules_free(&brs);

    rules_free(&rs);
    return TEST_DONE();
}
