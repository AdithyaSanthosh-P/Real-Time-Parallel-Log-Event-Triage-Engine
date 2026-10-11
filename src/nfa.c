/* Thompson's construction: AST -> NFA (spec §3.2 steps 2-3), plus a
 * set-of-states simulator used as the reference for the DFA.
 *
 * Construction works on fragments: a fragment is a sub-automaton with one
 * entry state (`start`) and one exit state (`end`, an NFA_EPS whose out1 is
 * still -1). Each AST node kind has one construction case (build_*), and
 * every case wires its children's fragments together with epsilon edges, so
 * the result has O(size of the regex) states and edges. */

#include "nfa.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regex_parse.h"

typedef struct {
    int start;
    int end;
} Frag;

void nfa_init(Nfa *nfa)
{
    memset(nfa, 0, sizeof *nfa);
    nfa->start = -1;
}

void nfa_free(Nfa *nfa)
{
    free(nfa->st);
    nfa_init(nfa);
}

/* Append a state; returns its index, or -1 on out of memory. */
static int new_state(Nfa *nfa, NfaKind kind)
{
    if (nfa->n == nfa->cap) {
        int cap = nfa->cap ? nfa->cap * 2 : 256;
        NfaState *p = realloc(nfa->st, (size_t)cap * sizeof *p);
        if (!p)
            return -1;
        nfa->st = p;
        nfa->cap = cap;
    }
    NfaState *s = &nfa->st[nfa->n];
    memset(s, 0, sizeof *s);
    s->kind = (uint8_t)kind;
    s->out1 = s->out2 = -1;
    s->rule = -1;
    return nfa->n++;
}

static int build(Nfa *nfa, const AstNode *n, Frag *f);

/* A single NFA_EPS state that is both entry and exit: matches "". */
static int build_empty(Nfa *nfa, Frag *f)
{
    int s = new_state(nfa, NFA_EPS);
    if (s < 0)
        return -1;
    f->start = f->end = s;
    return 0;
}

/* CHARSET:  start --set--> end */
static int build_charset(Nfa *nfa, const CharSet *set, Frag *f)
{
    int s = new_state(nfa, NFA_CHAR);
    int e = new_state(nfa, NFA_EPS);
    if (s < 0 || e < 0)
        return -1;
    nfa->st[s].set = *set;
    nfa->st[s].out1 = e;
    f->start = s;
    f->end = e;
    return 0;
}

/* BOL / EOL:  start --assert--> end  (zero-width, checked during closure) */
static int build_assert(Nfa *nfa, NfaKind kind, Frag *f)
{
    int s = new_state(nfa, kind);
    int e = new_state(nfa, NFA_EPS);
    if (s < 0 || e < 0)
        return -1;
    nfa->st[s].out1 = e;
    f->start = s;
    f->end = e;
    return 0;
}

/* CONCAT:  k1.end --eps--> k2.start ... ; start = k1.start, end = kn.end */
static int build_concat(Nfa *nfa, const AstNode *n, Frag *f)
{
    if (n->nkids == 0)
        return build_empty(nfa, f);
    Frag k;
    if (build(nfa, n->kids[0], &k) != 0)
        return -1;
    f->start = k.start;
    f->end = k.end;
    for (int i = 1; i < n->nkids; i++) {
        if (build(nfa, n->kids[i], &k) != 0)
            return -1;
        nfa->st[f->end].out1 = k.start;
        f->end = k.end;
    }
    return 0;
}

/* ALT (n-ary): a chain of two-way splits, since a state has at most two
 * epsilon edges:  s1 -> k1 | s2,  s2 -> k2 | s3, ... ; every ki.end -> end. */
static int build_alt(Nfa *nfa, const AstNode *n, Frag *f)
{
    int e = new_state(nfa, NFA_EPS);
    if (e < 0)
        return -1;
    int prev_split = -1;
    for (int i = 0; i < n->nkids; i++) {
        Frag k;
        if (build(nfa, n->kids[i], &k) != 0)
            return -1;
        nfa->st[k.end].out1 = e;
        int entry = k.start;
        if (i < n->nkids - 1) {                 /* all but the last get a split */
            int s = new_state(nfa, NFA_EPS);
            if (s < 0)
                return -1;
            nfa->st[s].out1 = k.start;
            entry = s;
        }
        if (prev_split < 0)
            f->start = entry;
        else
            nfa->st[prev_split].out2 = entry;
        prev_split = entry;
    }
    f->end = e;
    return 0;
}

/* STAR:  start -> k.start | end;   k.end -> k.start | end */
static int build_star(Nfa *nfa, const AstNode *kid, Frag *f)
{
    Frag k;
    if (build(nfa, kid, &k) != 0)
        return -1;
    int s = new_state(nfa, NFA_EPS);
    int e = new_state(nfa, NFA_EPS);
    if (s < 0 || e < 0)
        return -1;
    nfa->st[s].out1 = k.start;
    nfa->st[s].out2 = e;
    nfa->st[k.end].out1 = k.start;
    nfa->st[k.end].out2 = e;
    f->start = s;
    f->end = e;
    return 0;
}

/* PLUS:  start = k.start;   k.end -> k.start | end */
static int build_plus(Nfa *nfa, const AstNode *kid, Frag *f)
{
    Frag k;
    if (build(nfa, kid, &k) != 0)
        return -1;
    int e = new_state(nfa, NFA_EPS);
    if (e < 0)
        return -1;
    nfa->st[k.end].out1 = k.start;
    nfa->st[k.end].out2 = e;
    f->start = k.start;
    f->end = e;
    return 0;
}

/* QUEST:  start -> k.start | end;   k.end -> end */
static int build_quest(Nfa *nfa, const AstNode *kid, Frag *f)
{
    Frag k;
    if (build(nfa, kid, &k) != 0)
        return -1;
    int s = new_state(nfa, NFA_EPS);
    int e = new_state(nfa, NFA_EPS);
    if (s < 0 || e < 0)
        return -1;
    nfa->st[s].out1 = k.start;
    nfa->st[s].out2 = e;
    nfa->st[k.end].out1 = e;
    f->start = s;
    f->end = e;
    return 0;
}

/* REPEAT x{n,m} is expanded into copies (§3.2 step 1):
 *   x{n}   = x x ... x                 (n copies)
 *   x{n,}  = x ... x x*                (n copies, then a star)
 *   x{n,m} = x ... x (x (x ...)?)?     (n copies, then m-n nested optionals)
 * Each copy is built afresh from the AST, so copies share no states. The
 * nested form keeps the optional tail unambiguous. */
static int build_repeat(Nfa *nfa, const AstNode *n, Frag *f)
{
    const AstNode *kid = n->kids[0];
    if (build_empty(nfa, f) != 0)
        return -1;
    for (int i = 0; i < n->min; i++) {
        Frag k;
        if (build(nfa, kid, &k) != 0)
            return -1;
        nfa->st[f->end].out1 = k.start;
        f->end = k.end;
    }
    if (n->max == REPEAT_INF) {
        Frag k;
        if (build_star(nfa, kid, &k) != 0)
            return -1;
        nfa->st[f->end].out1 = k.start;
        f->end = k.end;
        return 0;
    }
    /* optional tail: each level is  split -> (x then next level) | end */
    int optional = n->max - n->min;
    if (optional == 0)
        return 0;
    int e = new_state(nfa, NFA_EPS);
    if (e < 0)
        return -1;
    for (int i = 0; i < optional; i++) {
        int s = new_state(nfa, NFA_EPS);
        Frag k;
        if (s < 0 || build(nfa, kid, &k) != 0)
            return -1;
        nfa->st[s].out1 = k.start;
        nfa->st[s].out2 = e;
        nfa->st[f->end].out1 = s;
        f->end = k.end;
    }
    nfa->st[f->end].out1 = e;
    f->end = e;
    return 0;
}

/* One construction case per AST node kind. */
static int build(Nfa *nfa, const AstNode *n, Frag *f)
{
    switch (n->kind) {
    case NODE_CHARSET: return build_charset(nfa, &n->set, f);
    case NODE_CONCAT:  return build_concat(nfa, n, f);
    case NODE_ALT:     return build_alt(nfa, n, f);
    case NODE_STAR:    return build_star(nfa, n->kids[0], f);
    case NODE_PLUS:    return build_plus(nfa, n->kids[0], f);
    case NODE_QUEST:   return build_quest(nfa, n->kids[0], f);
    case NODE_REPEAT:  return build_repeat(nfa, n, f);
    case NODE_BOL:     return build_assert(nfa, NFA_BOL, f);
    case NODE_EOL:     return build_assert(nfa, NFA_EOL, f);
    }
    return -1;
}

/* Does every path through `n` begin with '^'?  Such a rule can only match
 * at offset 0, so the DFA does not restart it at later offsets (§3.2 step 4,
 * "rules without anchored_start"). Conservative: false when unsure. */
static bool starts_with_bol(const AstNode *n)
{
    switch (n->kind) {
    case NODE_BOL:
        return true;
    case NODE_CONCAT:
        return n->nkids > 0 && starts_with_bol(n->kids[0]);
    case NODE_ALT:
        for (int i = 0; i < n->nkids; i++)
            if (!starts_with_bol(n->kids[i]))
                return false;
        return true;
    default:
        return false;
    }
}

int nfa_add_rule(Nfa *nfa, const AstNode *ast, int rule)
{
    if (rule < 0 || rule >= RULES_MAX || rule != nfa->nrules)
        return -1;
    Frag f;
    if (build(nfa, ast, &f) != 0)
        return -1;
    int acc = new_state(nfa, NFA_ACCEPT);
    if (acc < 0)
        return -1;
    nfa->st[acc].rule = rule;
    nfa->st[f.end].out1 = acc;
    nfa->rule_start[rule] = f.start;
    nfa->rule_accept[rule] = acc;
    nfa->rule_anchored[rule] = starts_with_bol(ast);
    nfa->nrules++;
    return 0;
}

/* Combined NFA (§3.2 step 3): a fresh start with epsilon edges to every
 * rule's start, again as a chain of two-way splits. */
int nfa_finish(Nfa *nfa)
{
    if (nfa->nrules == 0) {                     /* no rules: a start that matches nothing */
        nfa->start = new_state(nfa, NFA_EPS);
        return nfa->start < 0 ? -1 : 0;
    }
    int next = nfa->rule_start[nfa->nrules - 1];
    for (int r = nfa->nrules - 2; r >= 0; r--) {
        int s = new_state(nfa, NFA_EPS);
        if (s < 0)
            return -1;
        nfa->st[s].out1 = nfa->rule_start[r];
        nfa->st[s].out2 = next;
        next = s;
    }
    nfa->start = next;
    return 0;
}

int nfa_build_rules(Nfa *nfa, const RuleSet *rs, char *err, size_t err_len)
{
    nfa_init(nfa);
    for (int i = 0; i < rs->count; i++) {
        const Rule *r = &rs->rules[i];
        RegexError re;
        AstNode *ast = regex_parse(r->regex, strlen(r->regex), r->icase, &re);
        if (!ast) {
            snprintf(err, err_len, "rule %s: offset %zu: %s", r->id, re.pos, re.msg);
            nfa_free(nfa);
            return -1;
        }
        int rc = nfa_add_rule(nfa, ast, i);
        ast_free(ast);
        if (rc != 0) {
            snprintf(err, err_len, "rule %s: out of memory building the NFA", r->id);
            nfa_free(nfa);
            return -1;
        }
    }
    if (nfa_finish(nfa) != 0) {
        snprintf(err, err_len, "out of memory building the NFA");
        nfa_free(nfa);
        return -1;
    }
    return 0;
}

/* ---- simulation ----
 *
 * The current set of NFA states is kept as a list plus a "mark" array that
 * records the step at which a state was last added (so the set never needs
 * clearing). ε-closure is computed with an explicit stack: pop a state, and
 * push its epsilon successors that are not in the set yet; NFA_BOL edges are
 * followed only at offset 0 and NFA_EOL edges only at the end of the line. */

typedef struct {
    int      *list;
    int       n;
    uint32_t *mark;
    uint32_t  gen;
    int      *stack;
} StateSet;

static void add_closure(const Nfa *nfa, StateSet *S, int s0, bool at_bol, bool at_eol)
{
    int sp = 0;
    if (S->mark[s0] == S->gen)
        return;
    S->mark[s0] = S->gen;
    S->list[S->n++] = s0;
    S->stack[sp++] = s0;
    while (sp > 0) {
        const NfaState *st = &nfa->st[S->stack[--sp]];
        int outs[2] = { -1, -1 };
        switch (st->kind) {
        case NFA_EPS: outs[0] = st->out1; outs[1] = st->out2; break;
        case NFA_BOL: if (at_bol) outs[0] = st->out1; break;
        case NFA_EOL: if (at_eol) outs[0] = st->out1; break;
        default: break;                         /* CHAR and ACCEPT: no epsilon edges */
        }
        for (int k = 0; k < 2; k++) {
            int t = outs[k];
            if (t >= 0 && S->mark[t] != S->gen) {
                S->mark[t] = S->gen;
                S->list[S->n++] = t;
                S->stack[sp++] = t;
            }
        }
    }
}

static uint64_t accepted(const Nfa *nfa, const StateSet *S)
{
    uint64_t m = 0;
    for (int i = 0; i < S->n; i++) {
        const NfaState *st = &nfa->st[S->list[i]];
        if (st->kind == NFA_ACCEPT)
            m |= (uint64_t)1 << st->rule;
    }
    return m;
}

uint64_t nfa_simulate(const Nfa *nfa, const char *line, size_t len)
{
    size_t n = (size_t)nfa->n;
    StateSet cur = {0}, nxt = {0};
    uint32_t *mark = calloc(n, sizeof *mark);
    cur.list = malloc(n * sizeof(int));
    nxt.list = malloc(n * sizeof(int));
    cur.stack = nxt.stack = malloc(n * sizeof(int));
    if (!mark || !cur.list || !nxt.list || !cur.stack) {
        free(mark); free(cur.list); free(nxt.list); free(cur.stack);
        return 0;
    }
    cur.mark = nxt.mark = mark;   /* one mark array; a new generation per step */
    uint32_t gen = 1;

    /* offset 0: closure of the combined start (all rules, '^' allowed) */
    cur.gen = gen;
    add_closure(nfa, &cur, nfa->start, true, len == 0);
    uint64_t mask = accepted(nfa, &cur);

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)line[i];
        bool at_eol = (i + 1 == len);
        nxt.n = 0;
        nxt.gen = ++gen;
        /* move on c, then close */
        for (int k = 0; k < cur.n; k++) {
            const NfaState *st = &nfa->st[cur.list[k]];
            if (st->kind == NFA_CHAR && cs_has(&st->set, c))
                add_closure(nfa, &nxt, st->out1, false, at_eol);
        }
        /* unanchored search: every rule may start again at offset i+1 */
        for (int r = 0; r < nfa->nrules; r++)
            if (!nfa->rule_anchored[r])
                add_closure(nfa, &nxt, nfa->rule_start[r], false, at_eol);
        mask |= accepted(nfa, &nxt);

        StateSet t = cur;
        cur = nxt;
        nxt = t;
    }

    free(mark);
    free(cur.list);
    free(nxt.list);
    free(cur.stack);
    return mask;
}
