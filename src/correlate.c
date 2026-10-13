#include "correlate.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* One source IP: its last CORR_RING failures. */
typedef struct {
    uint32_t ip;                 /* 0 = empty slot (0.0.0.0 is never extracted) */
    uint32_t n;                  /* failures stored, <= CORR_RING */
    uint32_t head;               /* next slot to write */
    bool     bf_fired, spray_fired;
    int64_t  bf_last, spray_last; /* time of the last escalation of each kind */
    int64_t  ts[CORR_RING];
    uint64_t user[CORR_RING];    /* FNV-1a of the user name, 0 = none */
} IpEntry;

struct Correlator {
    CorrelatorConfig cfg;
    uint64_t  success_rules;     /* no-label AUTH rules (SSH_ACCEPTED) */
    IpEntry  *map;
    size_t    cap, used;         /* power-of-two capacity, occupied slots */
    int64_t  *err_ts;            /* ring of the last storm_lines ERROR times */
    int       err_n, err_head;
    bool      storm_active;
    uint64_t  count[4];
};

void correlator_default_config(CorrelatorConfig *cfg)
{
    cfg->maxretry = 5;
    cfg->findtime = 60;
    cfg->spray_users = 5;
    cfg->spray_window = 300;
    cfg->storm_lines = 100;
    cfg->storm_window = 60;
}

#define MAP_INITIAL 1024

Correlator *correlator_new(const CorrelatorConfig *cfg, const RuleSet *rs)
{
    if (cfg->maxretry < 1 || cfg->maxretry > CORR_RING || cfg->spray_users < 1 ||
        cfg->spray_users > CORR_RING || cfg->storm_lines < 1 || cfg->findtime < 0 ||
        cfg->spray_window < 0 || cfg->storm_window < 0)
        return NULL;
    Correlator *k = calloc(1, sizeof *k);
    if (!k)
        return NULL;
    k->cfg = *cfg;
    for (int r = 0; r < rs->count; r++)
        if (rs->rules[r].category == CAT_AUTH && rs->rules[r].no_label)
            k->success_rules |= 1ull << r;
    k->cap = MAP_INITIAL;
    k->map = calloc(k->cap, sizeof *k->map);
    k->err_ts = calloc((size_t)cfg->storm_lines, sizeof *k->err_ts);
    if (!k->map || !k->err_ts) {
        correlator_free(k);
        return NULL;
    }
    return k;
}

void correlator_free(Correlator *k)
{
    if (!k)
        return;
    free(k->map);
    free(k->err_ts);
    free(k);
}

uint64_t correlator_count(const Correlator *k, int flag_bit)
{
    return flag_bit >= 0 && flag_bit < 4 ? k->count[flag_bit] : 0;
}

/* ---- the IP map: open addressing, linear probing ---- */

static size_t slot_of(uint32_t ip, size_t cap)
{
    uint32_t h = ip * 2654435761u;               /* Knuth multiplicative hash */
    h ^= h >> 16;
    return h & (cap - 1);
}

static IpEntry *find(const Correlator *k, uint32_t ip)
{
    for (size_t i = slot_of(ip, k->cap);; i = (i + 1) & (k->cap - 1)) {
        if (k->map[i].ip == ip)
            return &k->map[i];
        if (k->map[i].ip == 0)
            return NULL;
    }
}

/* Double the table when it is 70% full. false on out of memory. */
static bool grow(Correlator *k)
{
    size_t ncap = k->cap * 2;
    IpEntry *nm = calloc(ncap, sizeof *nm);
    if (!nm)
        return false;
    for (size_t i = 0; i < k->cap; i++) {
        if (k->map[i].ip == 0)
            continue;
        size_t j = slot_of(k->map[i].ip, ncap);
        while (nm[j].ip != 0)
            j = (j + 1) & (ncap - 1);
        nm[j] = k->map[i];
    }
    free(k->map);
    k->map = nm;
    k->cap = ncap;
    return true;
}

/* Find or create; NULL only when the table cannot grow. */
static IpEntry *get(Correlator *k, uint32_t ip)
{
    IpEntry *e = find(k, ip);
    if (e)
        return e;
    if ((k->used + 1) * 10 > k->cap * 7 && !grow(k))
        return NULL;
    size_t i = slot_of(ip, k->cap);
    while (k->map[i].ip != 0)
        i = (i + 1) & (k->cap - 1);
    k->map[i].ip = ip;
    k->used++;
    return &k->map[i];
}

/* ---- windows ---- */

static uint64_t user_hash(const char *u)
{
    if (!u[0])
        return 0;
    uint64_t h = 1469598103934665603ull;         /* FNV-1a */
    for (; *u; u++)
        h = (h ^ (uint8_t)*u) * 1099511628211ull;
    return h ? h : 1;
}

static inline bool in_window(int64_t t, int64_t now, int64_t w)
{
    return t <= now && t >= now - w;
}

/* Failures of `e` within `w` seconds back from `now`. */
static int failures_within(const IpEntry *e, int64_t now, int64_t w)
{
    int c = 0;
    for (uint32_t i = 0; i < e->n; i++)
        c += in_window(e->ts[i], now, w);
    return c;
}

/* Distinct user names among the failures of `e` within `w`. */
static int users_within(const IpEntry *e, int64_t now, int64_t w)
{
    uint64_t seen[CORR_RING];
    int n = 0;
    for (uint32_t i = 0; i < e->n; i++) {
        if (!e->user[i] || !in_window(e->ts[i], now, w))
            continue;
        bool dup = false;
        for (int j = 0; j < n && !dup; j++)
            dup = seen[j] == e->user[i];
        if (!dup)
            seen[n++] = e->user[i];
    }
    return n;
}

/* An AUTH failure from m->src_ip at `now`: record it, then test BRUTE_FORCE
 * and PASSWORD_SPRAY. */
static uint32_t on_failure(Correlator *k, int64_t now, const MatchResult *m)
{
    IpEntry *e = get(k, m->src_ip);
    if (!e)
        return 0;                                 /* out of memory: no correlation */
    e->ts[e->head] = now;
    e->user[e->head] = user_hash(m->user);
    e->head = (e->head + 1) % CORR_RING;
    if (e->n < CORR_RING)
        e->n++;

    uint32_t flags = 0;
    const CorrelatorConfig *c = &k->cfg;
    if (failures_within(e, now, c->findtime) >= c->maxretry &&
        (!e->bf_fired || now - e->bf_last >= c->findtime)) {
        e->bf_fired = true;
        e->bf_last = now;
        flags |= FLAG_BRUTE_FORCE;
    }
    if (users_within(e, now, c->spray_window) >= c->spray_users &&
        (!e->spray_fired || now - e->spray_last >= c->spray_window)) {
        e->spray_fired = true;
        e->spray_last = now;
        flags |= FLAG_PASSWORD_SPRAY;
    }
    return flags;
}

/* A successful login from an IP currently over the BRUTE_FORCE threshold. */
static uint32_t on_success(const Correlator *k, int64_t now, const MatchResult *m)
{
    const IpEntry *e = find(k, m->src_ip);
    if (e && failures_within(e, now, k->cfg.findtime) >= k->cfg.maxretry)
        return FLAG_FAIL_THEN_SUCCESS;
    return 0;
}

/* An ERROR line: record it, then test for the start of a storm. */
static uint32_t on_error(Correlator *k, int64_t now)
{
    k->err_ts[k->err_head] = now;
    k->err_head = (k->err_head + 1) % k->cfg.storm_lines;
    if (k->err_n < k->cfg.storm_lines)
        k->err_n++;
    int c = 0;
    for (int i = 0; i < k->err_n; i++)
        c += in_window(k->err_ts[i], now, k->cfg.storm_window);
    if (c < k->cfg.storm_lines) {
        k->storm_active = false;                  /* any storm is over */
        return 0;
    }
    if (k->storm_active)
        return 0;                                 /* same storm: suppressed */
    k->storm_active = true;
    return FLAG_ERROR_STORM;
}

uint32_t correlator_observe(Correlator *k, int64_t ts, const MatchResult *m,
                            uint32_t label_mask, Evidence *ev)
{
    uint32_t flags = 0;
    if (m->src_ip != 0) {
        if (label_mask & CAT_AUTH)
            flags |= on_failure(k, ts, m);
        else if (m->rule_mask & k->success_rules)
            flags |= on_success(k, ts, m);
    }
    if (label_mask & CAT_ERR)
        flags |= on_error(k, ts);

    /* spec §5.2: an escalation sets the category's confidence to 0.95 */
    if (flags & (FLAG_BRUTE_FORCE | FLAG_PASSWORD_SPRAY))
        aggregator_escalate(ev, CAT_SEC, SEV_HIGH);
    if (flags & FLAG_FAIL_THEN_SUCCESS)
        aggregator_escalate(ev, CAT_SEC, SEV_CRITICAL);
    if (flags & FLAG_ERROR_STORM)
        aggregator_escalate(ev, CAT_ERR, SEV_HIGH);
    for (int b = 0; b < 4; b++)
        k->count[b] += (flags >> b) & 1u;
    return flags;
}
