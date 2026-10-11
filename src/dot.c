#include "dot.h"

#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "json.h"

/* Write `s` as the body of a DOT double-quoted string. */
static void put_quoted(FILE *f, const char *s)
{
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            fputc('\\', f);
        fputc(*s, f);
    }
}

static void put_set_label(FILE *f, const CharSet *set)
{
    StrBuf sb;
    sb_init(&sb);
    cs_format(set, &sb);
    put_quoted(f, sb.data ? sb.data : "?");
    sb_free(&sb);
}

/* "RULE_A RULE_B" for the bits of `mask`. */
static void put_rule_names(FILE *f, const RuleSet *rs, uint64_t mask)
{
    bool first = true;
    for (int r = 0; r < rs->count; r++) {
        if (!((mask >> r) & 1))
            continue;
        if (!first)
            fputc(' ', f);
        put_quoted(f, rs->rules[r].id);
        first = false;
    }
}

static void write_nfa(FILE *f, const RuleSet *rs, const Nfa *nfa)
{
    fprintf(f, "  subgraph cluster_nfa {\n    label=\"combined NFA (%d states)\";\n", nfa->n);
    for (int i = 0; i < nfa->n; i++) {
        const NfaState *s = &nfa->st[i];
        if (s->kind == NFA_ACCEPT) {
            fprintf(f, "    n%d [shape=doublecircle, label=\"%d\\n", i, i);
            put_rule_names(f, rs, (uint64_t)1 << s->rule);
            fprintf(f, "\"];\n");
        } else {
            fprintf(f, "    n%d [label=\"%d\"%s];\n", i, i, i == nfa->start ? ", style=bold" : "");
        }
    }
    for (int i = 0; i < nfa->n; i++) {
        const NfaState *s = &nfa->st[i];
        switch (s->kind) {
        case NFA_CHAR:
            fprintf(f, "    n%d -> n%d [label=\"", i, s->out1);
            put_set_label(f, &s->set);
            fprintf(f, "\"];\n");
            break;
        case NFA_EPS:
            if (s->out1 >= 0)
                fprintf(f, "    n%d -> n%d [label=\"ε\", style=dashed];\n", i, s->out1);
            if (s->out2 >= 0)
                fprintf(f, "    n%d -> n%d [label=\"ε\", style=dashed];\n", i, s->out2);
            break;
        case NFA_BOL:
            fprintf(f, "    n%d -> n%d [label=\"^\", style=dashed];\n", i, s->out1);
            break;
        case NFA_EOL:
            fprintf(f, "    n%d -> n%d [label=\"$\", style=dashed];\n", i, s->out1);
            break;
        default:
            break;
        }
    }
    fprintf(f, "  }\n");
}

static void write_dfa(FILE *f, const RuleSet *rs, const Dfa *d, int g)
{
    fprintf(f, "  subgraph cluster_dfa%d {\n    label=\"DFA group %d (%d states): ", g, g, d->nstates);
    put_rule_names(f, rs, d->rules);
    fprintf(f, "\";\n");
    for (int s = 0; s < d->nstates; s++) {
        uint64_t now = d->accept_mask[s];
        uint64_t end = d->accept_end_mask[s] & ~now;
        fprintf(f, "    d%d_%d [label=\"%s%d", g, s, s == 0 ? "start " : "", s);
        if (now) {
            fprintf(f, "\\n");
            put_rule_names(f, rs, now);
        }
        if (end) {
            fprintf(f, "\\n$: ");
            put_rule_names(f, rs, end);
        }
        fprintf(f, "\"%s%s];\n", now ? ", shape=doublecircle" : (end ? ", shape=Mcircle" : ""),
                s == 0 ? ", style=bold" : "");
    }
    /* one edge per (source, target), labelled with all its bytes */
    for (int s = 0; s < d->nstates; s++) {
        const int32_t *row = d->next + (size_t)s * 256;
        bool done[256] = { false };
        for (int b = 0; b < 256; b++) {
            if (done[b])
                continue;
            CharSet set;
            cs_clear(&set);
            for (int c = b; c < 256; c++) {
                if (row[c] == row[b]) {
                    cs_add(&set, (unsigned char)c);
                    done[c] = true;
                }
            }
            fprintf(f, "    d%d_%d -> d%d_%d [label=\"", g, s, g, row[b]);
            put_set_label(f, &set);
            fprintf(f, "\"];\n");
        }
    }
    fprintf(f, "  }\n");
}

int dot_write(FILE *f, const RuleSet *rs, const Nfa *nfa, const DfaSet *ds)
{
    fprintf(f, "digraph triage {\n  rankdir=LR;\n  node [shape=circle, fontsize=10];\n"
               "  edge [fontsize=9];\n");
    write_nfa(f, rs, nfa);
    if (ds)
        for (int g = 0; g < ds->ngroups; g++)
            write_dfa(f, rs, &ds->groups[g], g);
    fprintf(f, "}\n");
    return ferror(f) ? -1 : 0;
}
