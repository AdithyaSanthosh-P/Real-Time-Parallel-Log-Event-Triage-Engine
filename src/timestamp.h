#ifndef TIMESTAMP_H
#define TIMESTAMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Parse the timestamp at the START of a log line (spec §6) into epoch
 * seconds. Recognised, optionally preceded by a single '[':
 *
 *   syslog   "Mon DD HH:MM:SS"   (day may be space-padded: "Oct  1 ...");
 *            the year is not in the line, so `year` is used
 *   ISO-8601 "YYYY-MM-DD[T ]HH:MM:SS" with an optional fraction ('.' or ','
 *            then digits, ignored) and an optional zone directly attached:
 *            "Z", "+HH", "+HHMM" or "+HH:MM". Without a zone the time is UTC.
 *
 * Syslog times are taken as UTC as well, so replays are deterministic.
 * Returns true and sets *epoch on success; false if the line does not start
 * with a valid timestamp (the caller then falls back to ingest time). */
bool ts_parse(const char *line, size_t n, int year, int64_t *epoch);

/* The calendar year (UTC) of `t` seconds since the epoch. */
int ts_year_of(int64_t t);

#endif /* TIMESTAMP_H */
