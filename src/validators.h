#ifndef VALIDATORS_H
#define VALIDATORS_H

/* Checksum / structure validators for PII rules (spec §4.3, §4.2 rule notes).
 *
 * A regex can only say a span has the right shape; the validator says whether
 * the number is plausible (credit card: Luhn; Aadhaar: Verhoeff; PAN: structure).
 * The three primitives take a bare token (digits only, or the PAN letters and
 * digits); validate_span() takes a span as the regex matched it. */

#include <stdbool.h>
#include <stddef.h>

#include "rules.h"

/* Luhn mod-10 check over `n` ASCII digits (no separators); also requires
 * 13 <= n <= 19, the card number lengths of §4.2. False on any non-digit. */
bool luhn_ok(const char *d, size_t n);

/* Verhoeff check (dihedral-group D5) over `n` ASCII digits, the last being the
 * check digit. False on any non-digit or n == 0. */
bool verhoeff_ok(const char *d, size_t n);

/* Permanent Account Number structure: AAAPA9999A, i.e. 5 letters, 4 digits,
 * 1 letter, with the 4th letter a holder type in {A,B,C,F,G,H,J,L,P,T}. */
bool pan_ok(const char *s, size_t n);

/* Copy `in` to `out` without spaces and hyphens; `out` needs room for `n`
 * bytes. Returns the new length. */
size_t strip_separators(const char *in, size_t n, char *out);

/* Run rule validator `v` on a span as matched by the rule's regex. The span
 * may carry the regex's boundary characters (the `(^|[^0-9])` / `([^0-9]|$)`
 * groups of the PII rules), which are trimmed first; spaces and hyphens inside
 * a number are stripped. VAL_NONE always passes. Aadhaar additionally needs
 * exactly 12 digits starting with 2-9 (§4.2 notes). */
bool validate_span(Validator v, const char *span, size_t n);

#endif /* VALIDATORS_H */
