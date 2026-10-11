#include "timestamp.h"

#include <string.h>
#include <time.h>

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

/* Read exactly `w` digits at s[*p]; false if fewer are available. */
static bool digits(const char *s, size_t n, size_t *p, int w, int *out)
{
    if (*p + (size_t)w > n)
        return false;
    int v = 0;
    for (int i = 0; i < w; i++) {
        char c = s[*p + (size_t)i];
        if (!is_digit(c))
            return false;
        v = v * 10 + (c - '0');
    }
    *p += (size_t)w;
    *out = v;
    return true;
}

static bool expect(const char *s, size_t n, size_t *p, char c)
{
    if (*p < n && s[*p] == c) {
        (*p)++;
        return true;
    }
    return false;
}

static bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int days_in_month(int y, int m)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return (m == 2 && is_leap(y)) ? 29 : d[m - 1];
}

/* Validate the fields and convert (UTC). */
static bool to_epoch(int y, int mon, int day, int h, int mi, int sec, int64_t *out)
{
    if (mon < 1 || mon > 12 || day < 1 || day > days_in_month(y, mon) ||
        h > 23 || mi > 59 || sec > 60)
        return false;
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    time_t t = timegm(&tm);
    if (t == (time_t)-1)
        return false;
    *out = (int64_t)t;
    return true;
}

/* "HH:MM:SS" at s[*p]. */
static bool clock_time(const char *s, size_t n, size_t *p, int *h, int *mi, int *sec)
{
    return digits(s, n, p, 2, h) && expect(s, n, p, ':') &&
           digits(s, n, p, 2, mi) && expect(s, n, p, ':') &&
           digits(s, n, p, 2, sec);
}

static bool parse_syslog(const char *s, size_t n, size_t p, int year, int64_t *epoch)
{
    static const char names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int mon = 0;
    if (p + 3 > n)
        return false;
    for (int m = 0; m < 12; m++) {
        if (memcmp(s + p, names + 3 * m, 3) == 0) {
            mon = m + 1;
            break;
        }
    }
    if (mon == 0)
        return false;
    p += 3;

    if (!expect(s, n, &p, ' '))
        return false;
    expect(s, n, &p, ' ');                  /* space-padded single-digit day */
    if (p >= n || !is_digit(s[p]))
        return false;
    int day = s[p++] - '0';
    if (p < n && is_digit(s[p]))
        day = day * 10 + (s[p++] - '0');
    if (!expect(s, n, &p, ' '))
        return false;

    int h, mi, sec;
    if (!clock_time(s, n, &p, &h, &mi, &sec))
        return false;
    return to_epoch(year, mon, day, h, mi, sec, epoch);
}

/* Zone attached to the end of the seconds/fraction: Z, +HH, +HHMM, +HH:MM.
 * Returns the offset in seconds east of UTC; 0 if there is none. */
static int parse_zone(const char *s, size_t n, size_t p)
{
    if (p >= n)
        return 0;
    if (s[p] == 'Z')
        return 0;
    if (s[p] != '+' && s[p] != '-')
        return 0;
    int sign = s[p] == '-' ? -1 : 1;
    p++;
    int hh, mm = 0;
    if (!digits(s, n, &p, 2, &hh))
        return 0;
    if (p < n && s[p] == ':')
        p++;
    if (p < n && is_digit(s[p]) && !digits(s, n, &p, 2, &mm))
        return 0;
    if (hh > 23 || mm > 59)
        return 0;
    return sign * (hh * 3600 + mm * 60);
}

static bool parse_iso(const char *s, size_t n, size_t p, int64_t *epoch)
{
    int y, mon, day, h, mi, sec;
    if (!(digits(s, n, &p, 4, &y) && expect(s, n, &p, '-') &&
          digits(s, n, &p, 2, &mon) && expect(s, n, &p, '-') &&
          digits(s, n, &p, 2, &day)))
        return false;
    if (!expect(s, n, &p, 'T') && !expect(s, n, &p, ' '))
        return false;
    if (!clock_time(s, n, &p, &h, &mi, &sec))
        return false;

    if (p < n && (s[p] == '.' || s[p] == ',')) {   /* fraction is ignored */
        size_t q = p + 1;
        if (q < n && is_digit(s[q])) {
            while (q < n && is_digit(s[q]))
                q++;
            p = q;
        }
    }
    int zone = parse_zone(s, n, p);
    int64_t t;
    if (!to_epoch(y, mon, day, h, mi, sec, &t))
        return false;
    *epoch = t - zone;
    return true;
}

bool ts_parse(const char *line, size_t n, int year, int64_t *epoch)
{
    size_t p = 0;
    if (p < n && line[p] == '[')
        p++;
    if (p < n && is_digit(line[p]))
        return parse_iso(line, n, p, epoch);
    return parse_syslog(line, n, p, year, epoch);
}

int ts_year_of(int64_t t)
{
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    return tm.tm_year + 1900;
}
