/* Subset construction: combined NFA -> combined DFA (spec §3.2 step 4).
 *
 * A DFA state stands for the set of NFA states the NFA could be in after
 * reading some prefix of the line. Starting from
 *     start0 = ε-closure(starts of ALL rules)          ('^' edges allowed)
 * each DFA state S gets, for every byte c,
 *     δ(S, c) = ε-closure(move(S, c)) ∪ U,
 *     U       = ε-closure(starts of rules not anchored with '^'),
 * so that every rule can begin a match at every offset ("unanchored search").
 * New sets are discovered with a worklist and numbered in discovery order;
 * a hash table maps each set to its number.
 *
 * Details that keep the automaton exact and small:
 *   - Kernel: a set is stored as only its NFA_CHAR, NFA_ACCEPT and NFA_EOL
 *     states. ε and '^' states add nothing once the closure is taken (no '^'
 *     can fire after offset 0), so two closures with the same kernel behave
 *     identically and become one DFA state.
 *   - '$': accept_end_mask[S] = rules whose accept state is reachable from S
 *     when NFA_EOL edges may be followed, i.e. if the line ends here.
 *   - The start state is flagged so that it never merges with a later state:
 *     it is the only state where '^' held, so its accept_end_mask may use
 *     '^' and '$' together (the empty line).
 *   - Byte classes: bytes that no charset in the NFA tells apart give the
 *     same move, so δ is computed once per class and copied to its bytes.
 *   - Every state contains all of U (start0 ⊇ U as well), so a state is
 *     stored and hashed as its kernel MINUS U's states: usually a handful of
 *     NFA states instead of ~100. Because closure(A ∪ B) = closure(A) ∪
 *     closure(B), U's own contributions are computed once up front: its
 *     accept bits, its end-of-line accept bits, and, per byte class, the
 *     states that U's charset edges lead to. */

#include "dfa.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- ε-closure with an explicit stack ---- */

typedef struct {
    const Nfa *nfa;
    uint32_t  *mark;      /* mark[s] == gen  <=>  s is in the current set */
    uint32_t   gen;
    int       *list;      /* states of the current set, in discovery order */
    int        n;
    int       *stack;
    uint64_t   rules;     /* only accept states of these rules count */
} Closure;

static void closure_reset(Closure *C)
{
    C->gen++;
    C->n = 0;
}

static void closure_add(Closure *C, int s)
{
    if (C->mark[s] != C->gen) {
        C->mark[s] = C->gen;
        C->list[C->n++] = s;
    }
}

/* Extend the current set with everything reachable from `seed` by ε edges.
 * '^' edges are followed only if `bol`, '$' edges only if `eol`. */
static void closure_from(Closure *C, int seed, bool bol, bool eol)
{
    const NfaState *st = C->nfa->st;
    int sp = 0;
    if (C->mark[seed] == C->gen)
        return;
    closure_add(C, seed);
    C->stack[sp++] = seed;
    while (sp > 0) {
        const NfaState *s = &st[C->stack[--sp]];
        int a = -1, b = -1;
        if (s->kind == NFA_EPS) {
            a = s->out1;
            b = s->out2;
        } else if ((s->kind == NFA_BOL && bol) || (s->kind == NFA_EOL && eol)) {
            a = s->out1;
        }
        if (a >= 0 && C->mark[a] != C->gen) {
            closure_add(C, a);
            C->stack[sp++] = a;
        }
        if (b >= 0 && C->mark[b] != C->gen) {
            closure_add(C, b);
            C->stack[sp++] = b;
        }
    }
}

/* Rule bits of the accept states in the current set. */
static uint64_t closure_accepts(const Closure *C)
{
    uint64_t m = 0;
    for (int i = 0; i < C->n; i++) {
        const NfaState *s = &C->nfa->st[C->list[i]];
        if (s->kind == NFA_ACCEPT)
            m |= (uint64_t)1 << s->rule;
    }
    return m & C->rules;
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

/* Write the sorted kernel of the current set, minus the states marked in
 * `skip` (U's states; may be NULL), into `out`; returns its size. */
static int closure_kernel(const Closure *C, const uint8_t *skip, int *out)
{
    int k = 0;
    for (int i = 0; i < C->n; i++) {
        int s = C->list[i];
        uint8_t kind = C->nfa->st[s].kind;
        if ((kind == NFA_CHAR || kind == NFA_ACCEPT || kind == NFA_EOL) && !(skip && skip[s]))
            out[k++] = s;
    }
    qsort(out, (size_t)k, sizeof *out, cmp_int);
    return k;
}

/* ---- DFA state table: kernels in one pool, hashed for lookup ---- */

typedef struct {
    int      *pool;        /* kernels back to back */
    size_t    pool_n, pool_cap;
    size_t   *off;         /* kernel of state i = pool[off[i] .. off[i] + len[i]) */
    int      *len;
    uint32_t *hash;
    int       n, cap;      /* states */
    int      *table;       /* open addressing: state index + 1, 0 = empty */
    size_t    table_cap;   /* power of two */
} StateTable;

static uint32_t hash_kernel(const int *k, int n, bool is_start)
{
    uint32_t h = 2166136261u ^ (is_start ? 0x9e3779b9u : 0);
    for (int i = 0; i < n; i++) {
        h ^= (uint32_t)k[i];
        h *= 16777619u;
    }
    return h;
}

static bool same_kernel(const StateTable *T, int i, const int *k, int n)
{
    return T->len[i] == n && (n == 0 || memcmp(T->pool + T->off[i], k, (size_t)n * sizeof *k) == 0);
}

static int table_grow(StateTable *T)
{
    size_t cap = T->table_cap ? T->table_cap * 2 : 1024;
    int *t = calloc(cap, sizeof *t);
    if (!t)
        return -1;
    for (int i = 0; i < T->n; i++) {
        size_t h = T->hash[i] & (cap - 1);
        while (t[h])
            h = (h + 1) & (cap - 1);
        t[h] = i + 1;
    }
    free(T->table);
    T->table = t;
    T->table_cap = cap;
    return 0;
}

/* Find the state with this kernel, or add it. Only state 0 is a start
 * state, so `is_start` is passed (and hashed) only when creating it.
 * Returns the state index, or -1 on out of memory. *added tells which. */
static int table_intern(StateTable *T, const int *k, int n, bool is_start, bool *added)
{
    uint32_t h = hash_kernel(k, n, is_start);
    *added = false;
    if (T->table_cap) {
        for (size_t i = h & (T->table_cap - 1); T->table[i]; i = (i + 1) & (T->table_cap - 1)) {
            int s = T->table[i] - 1;
            if (T->hash[s] == h && s != 0 && !is_start && same_kernel(T, s, k, n))
                return s;
        }
    }
    if ((size_t)(T->n + 1) * 2 > T->table_cap && table_grow(T) != 0)
        return -1;
    if (T->n == T->cap) {
        int cap = T->cap ? T->cap * 2 : 256;
        size_t *off = realloc(T->off, (size_t)cap * sizeof *off);
        if (off) T->off = off;
        int *len = realloc(T->len, (size_t)cap * sizeof *len);
        if (len) T->len = len;
        uint32_t *hs = realloc(T->hash, (size_t)cap * sizeof *hs);
        if (hs) T->hash = hs;
        if (!off || !len || !hs)
            return -1;
        T->cap = cap;
    }
    if (T->pool_n + (size_t)n > T->pool_cap) {
        size_t cap = T->pool_cap ? T->pool_cap : 4096;
        while (cap < T->pool_n + (size_t)n)
            cap *= 2;
        int *p = realloc(T->pool, cap * sizeof *p);
        if (!p)
            return -1;
        T->pool = p;
        T->pool_cap = cap;
    }
    int s = T->n++;
    if (n > 0)
        memcpy(T->pool + T->pool_n, k, (size_t)n * sizeof *k);
    T->off[s] = T->pool_n;
    T->len[s] = n;
    T->hash[s] = h;
    T->pool_n += (size_t)n;
    size_t i = h & (T->table_cap - 1);
    while (T->table[i])
        i = (i + 1) & (T->table_cap - 1);
    T->table[i] = s + 1;
    *added = true;
    return s;
}

static void table_free(StateTable *T)
{
    free(T->pool);
    free(T->off);
    free(T->len);
    free(T->hash);
    free(T->table);
    memset(T, 0, sizeof *T);
}

/* ---- byte classes ---- */

/* cls[b] = class of byte b; returns the number of classes. Starts with one
 * class and splits it by every charset of the NFA (partition refinement over
 * the 256 bytes). */
static int byte_classes(const Nfa *nfa, uint16_t cls[256])
{
    int n = 1;
    memset(cls, 0, 256 * sizeof cls[0]);
    for (int s = 0; s < nfa->n; s++) {
        if (nfa->st[s].kind != NFA_CHAR)
            continue;
        int remap[2 * 256];
        for (int i = 0; i < 2 * n; i++)
            remap[i] = -1;
        int m = 0;
        for (int b = 0; b < 256; b++) {
            int key = cls[b] * 2 + cs_has(&nfa->st[s].set, (unsigned char)b);
            if (remap[key] < 0)
                remap[key] = m++;
            cls[b] = (uint16_t)remap[key];
        }
        n = m;
    }
    return n;
}

/* ---- construction ---- */

typedef struct {
    Dfa        *d;
    int         cap;       /* allocated states in d's tables */
} Tables;

static int tables_reserve(Tables *t, int need)
{
    if (need <= t->cap)
        return 0;
    int cap = t->cap ? t->cap * 2 : 256;
    while (cap < need)
        cap *= 2;
    int32_t *next = realloc(t->d->next, (size_t)cap * 256 * sizeof *next);
    if (next) t->d->next = next;
    uint64_t *am = realloc(t->d->accept_mask, (size_t)cap * sizeof *am);
    if (am) t->d->accept_mask = am;
    uint64_t *ae = realloc(t->d->accept_end_mask, (size_t)cap * sizeof *ae);
    if (ae) t->d->accept_end_mask = ae;
    if (!next || !am || !ae)
        return -1;
    t->cap = cap;
    return 0;
}

/* End-of-line accept bits for a state whose kernel (minus U) is k[0..n):
 * close again with '$' edges allowed. U's share is added by the caller. */
static uint64_t end_accepts(Closure *C, const int *k, int n)
{
    closure_reset(C);
    for (int i = 0; i < n; i++)
        closure_from(C, k[i], false, true);
    return closure_accepts(C);
}

int dfa_build(const Nfa *nfa, uint64_t rule_mask, int max_states, Dfa *out)
{
    memset(out, 0, sizeof *out);
    out->rules = rule_mask;
    if (max_states <= 0)
        max_states = DFA_DEFAULT_MAX_STATES;

    int rc = -1;
    size_t ns = (size_t)nfa->n;
    Closure C = { nfa, calloc(ns, sizeof(uint32_t)), 0, malloc(ns * sizeof(int)), 0,
                  malloc(ns * sizeof(int)), rule_mask };
    int *kernel = malloc(ns * sizeof(int));
    uint8_t *in_u = calloc(ns, 1);
    int *u_char = malloc(ns * sizeof(int));        /* U's NFA_CHAR states */
    int *u_all = malloc(ns * sizeof(int));         /* all of U's states */
    int *u_seed = NULL;                            /* per class: targets of U's edges */
    int u_seed_off[257] = {0};
    StateTable T;
    memset(&T, 0, sizeof T);
    Tables tab = { out, 0 };
    uint16_t cls[256];
    int ncls = byte_classes(nfa, cls);
    int rep[256];                                  /* a representative byte per class */
    for (int b = 255; b >= 0; b--)
        rep[cls[b]] = b;
    uint8_t cls_bytes[256];                        /* bytes grouped by class */
    int cls_off[257] = {0};
    for (int b = 0; b < 256; b++)
        cls_off[cls[b] + 1]++;
    for (int c = 0; c < ncls; c++)
        cls_off[c + 1] += cls_off[c];
    int fill[256];
    memcpy(fill, cls_off, sizeof fill);
    for (int b = 0; b < 256; b++)
        cls_bytes[fill[cls[b]]++] = (uint8_t)b;
    if (!C.mark || !C.list || !C.stack || !kernel || !in_u || !u_char || !u_all)
        goto done;

    /* U = ε-closure(starts of the group's rules that are not '^'-anchored),
     * with its accept bits, end-of-line accept bits, and per-class seeds. */
    closure_reset(&C);
    for (int r = 0; r < nfa->nrules; r++)
        if (((rule_mask >> r) & 1) && !nfa->rule_anchored[r])
            closure_from(&C, nfa->rule_start[r], false, false);
    uint64_t u_accept = closure_accepts(&C);
    int nu = 0, nu_all = C.n;
    for (int i = 0; i < C.n; i++) {
        int st = C.list[i];
        in_u[st] = 1;
        u_all[i] = st;
        if (nfa->st[st].kind == NFA_CHAR)
            u_char[nu++] = st;
    }
    closure_reset(&C);
    for (int i = 0; i < nu_all; i++)
        closure_from(&C, u_all[i], false, true);
    uint64_t u_end = closure_accepts(&C);

    u_seed = malloc((size_t)ncls * (size_t)(nu ? nu : 1) * sizeof(int));
    if (!u_seed)
        goto done;
    for (int c = 0; c < ncls; c++) {
        u_seed_off[c + 1] = u_seed_off[c];
        for (int i = 0; i < nu; i++)
            if (cs_has(&nfa->st[u_char[i]].set, (unsigned char)rep[c]))
                u_seed[u_seed_off[c + 1]++] = nfa->st[u_char[i]].out1;
    }

    /* start0 = ε-closure(starts of ALL the group's rules), '^' allowed */
    closure_reset(&C);
    for (int r = 0; r < nfa->nrules; r++)
        if ((rule_mask >> r) & 1)
            closure_from(&C, nfa->rule_start[r], true, false);
    int nk = closure_kernel(&C, in_u, kernel);
    bool added;
    if (table_intern(&T, kernel, nk, true, &added) != 0 || tables_reserve(&tab, 1) != 0)
        goto done;
    out->nstates = 1;
    out->accept_mask[0] = closure_accepts(&C);
    /* the empty line: '^' and '$' both hold at offset 0 */
    closure_reset(&C);
    for (int r = 0; r < nfa->nrules; r++)
        if ((rule_mask >> r) & 1)
            closure_from(&C, nfa->rule_start[r], true, true);
    out->accept_end_mask[0] = closure_accepts(&C);

    /* Worklist = states in discovery order; `s` walks it. */
    for (int s = 0; s < T.n; s++) {
        for (int c = 0; c < ncls; c++) {
            unsigned char byte = (unsigned char)rep[c];

            /* move(S, c) = moves of S's own states + moves of U (precomputed),
             * then ε-closure (no '^' after offset 0, no '$' yet) */
            closure_reset(&C);
            const int *k = T.pool + T.off[s];
            for (int i = 0; i < T.len[s]; i++) {
                const NfaState *st = &nfa->st[k[i]];
                if (st->kind == NFA_CHAR && cs_has(&st->set, byte))
                    closure_from(&C, st->out1, false, false);
            }
            for (int i = u_seed_off[c]; i < u_seed_off[c + 1]; i++)
                closure_from(&C, u_seed[i], false, false);
            uint64_t acc = closure_accepts(&C) | u_accept;
            nk = closure_kernel(&C, in_u, kernel);    /* ∪ U is implicit */

            int t = table_intern(&T, kernel, nk, false, &added);
            if (t < 0)
                goto done;
            if (added) {
                if (T.n > max_states) {
                    rc = DFA_TOO_BIG;
                    goto done;
                }
                if (tables_reserve(&tab, T.n) != 0)
                    goto done;
                out->nstates = T.n;
                out->accept_mask[t] = acc;
                out->accept_end_mask[t] = end_accepts(&C, kernel, nk) | u_end | acc;
            }
            /* δ(s, b) = t for every byte b of class c */
            int32_t *row = out->next + (size_t)s * 256;
            for (int i = cls_off[c]; i < cls_off[c + 1]; i++)
                row[cls_bytes[i]] = t;
        }
    }
    rc = 0;

done:
    free(C.mark);
    free(C.list);
    free(C.stack);
    free(kernel);
    free(in_u);
    free(u_char);
    free(u_all);
    free(u_seed);
    table_free(&T);
    if (rc != 0)
        dfa_free(out);
    return rc;
}

void dfa_free(Dfa *d)
{
    free(d->next);
    free(d->accept_mask);
    free(d->accept_end_mask);
    memset(d, 0, sizeof *d);
}

/* §3.3: one table lookup per byte; accept bits are collected on the way and
 * the end-of-line bits of the final state are added at the end. */
uint64_t dfa_match(const Dfa *d, const char *s, uint32_t n)
{
    int32_t st = 0;
    uint64_t m = d->accept_mask[0];
    for (uint32_t i = 0; i < n; i++) {
        st = d->next[st * 256 + (unsigned char)s[i]];
        m |= d->accept_mask[st];
    }
    return m | d->accept_end_mask[st];
}

uint64_t dfaset_match(const DfaSet *ds, const char *s, size_t n)
{
    uint64_t m = 0;
    for (int g = 0; g < ds->ngroups; g++)
        m |= dfa_match(&ds->groups[g], s, (uint32_t)n);
    return m;
}

/* ---- groups: halve recursively on overflow (§3.2 step 4, state cap) ---- */

static int add_group(DfaSet *ds, Dfa *d)
{
    Dfa *g = realloc(ds->groups, (size_t)(ds->ngroups + 1) * sizeof *g);
    if (!g)
        return -1;
    ds->groups = g;
    ds->groups[ds->ngroups++] = *d;
    ds->total_states += d->nstates;
    return 0;
}

static int build_groups(const Nfa *nfa, uint64_t mask, int max_states, DfaSet *ds,
                        char *err, size_t err_len)
{
    Dfa d;
    int rc = dfa_build(nfa, mask, max_states, &d);
    if (rc == 0) {
        if (add_group(ds, &d) != 0) {
            dfa_free(&d);
            snprintf(err, err_len, "out of memory");
            return -1;
        }
        return 0;
    }
    if (rc < 0) {
        snprintf(err, err_len, "out of memory building the DFA");
        return -1;
    }
    int count = __builtin_popcountll(mask);
    if (count == 1) {
        snprintf(err, err_len, "rule #%d alone needs more than %d DFA states (raise --max-states)",
                 __builtin_ctzll(mask), max_states);
        return -1;
    }
    /* split the rule list in two halves (by rule order) */
    uint64_t low = 0, rest = mask;
    for (int i = 0; i < count / 2; i++) {
        uint64_t bit = rest & (~rest + 1);
        low |= bit;
        rest &= ~bit;
    }
    if (build_groups(nfa, low, max_states, ds, err, err_len) != 0)
        return -1;
    return build_groups(nfa, rest, max_states, ds, err, err_len);
}

int dfaset_build(const Nfa *nfa, int max_states, DfaSet *out, char *err, size_t err_len)
{
    memset(out, 0, sizeof *out);
    if (max_states <= 0)
        max_states = DFA_DEFAULT_MAX_STATES;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    uint64_t all = nfa->nrules == 64 ? ~(uint64_t)0 : (((uint64_t)1 << nfa->nrules) - 1);
    int rc = all ? build_groups(nfa, all, max_states, out, err, err_len) : 0;
    if (rc == 0 && !all) {                 /* no rules: one trivial DFA that never matches */
        Dfa d;
        rc = dfa_build(nfa, 0, max_states, &d) == 0 && add_group(out, &d) == 0 ? 0 : -1;
        if (rc != 0)
            snprintf(err, err_len, "out of memory");
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    out->build_ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (rc != 0)
        dfaset_free(out);
    return rc;
}

void dfaset_free(DfaSet *ds)
{
    for (int g = 0; g < ds->ngroups; g++)
        dfa_free(&ds->groups[g]);
    free(ds->groups);
    memset(ds, 0, sizeof *ds);
}
