#ifndef MASK_H
#define MASK_H

/* PII masking for the output sinks (spec §7).
 *
 * The matched values of the PII rules are replaced in the line before it is
 * written, so no sink holds them in clear:
 *   card (Luhn rule)        XXXX…1111          (last 4 digits kept; U+2026 ellipsis)
 *   Aadhaar (Verhoeff rule) XXXX XXXX 1234
 *   PAN                     XXXXX1234X
 *   key=value secrets       key=***            (any span containing '=')
 *   e-mail address          ***@domain
 *   other digit strings     XXXXXX1234         (10+ digits: phone numbers)
 *   anything else           ***
 * (The spec lists the first four; e-mail and phone numbers are masked as well
 * so that no PII value of the corpus survives in any sink.)
 *
 * Only spans that pass the rule's validator are masked: a 12-digit order id
 * that fails Verhoeff is not an Aadhaar number and stays readable.
 *
 * The text masked is the NORMALIZED line (the one the rules matched on, so the
 * spans are exact); a masked record therefore carries the URL-decoded form of
 * the line. */

#include <stddef.h>
#include <stdint.h>

#include "extract.h"
#include "json.h"
#include "rules.h"

/* Does the set of surviving rules `kept` contain a PII-category rule? */
int mask_needed(const RuleSet *rs, uint64_t kept);

/* Write `line` with the spans of the PII rules in `kept` replaced into `out`
 * (cleared first). Returns the number of spans masked; 0 means `out` holds an
 * unchanged copy. */
int mask_pii(Extractor *x, const RuleSet *rs, const char *line, size_t len, uint64_t kept, StrBuf *out);

#endif /* MASK_H */
