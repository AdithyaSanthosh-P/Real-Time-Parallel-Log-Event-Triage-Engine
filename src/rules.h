#ifndef RULES_H
#define RULES_H

/* Rule file loader (spec §4.1).
 *
 * One rule per line, nine tab-separated columns:
 *   id  category  severity  base  flags  validator  extract  context  regex
 * The regex is the LAST column and is taken verbatim up to the end of the
 * line, so it may itself contain tabs (JAVA_STACK has one in a bracket
 * expression) and trailing spaces (JAVA_EXCEPTION ends in ": "). Only a
 * trailing "\n" or "\r\n" is removed. Blank lines and lines starting with
 * '#' are skipped. */

#include <stddef.h>
#include <stdbool.h>

#include "types.h"

#define RULES_MAX      64     /* rule masks are uint64_t */
#define RULE_ID_MAX    32     /* including the NUL */
#define RULE_CTX_MAX   8      /* context keywords per rule */
#define RULE_CTX_LEN   32     /* including the NUL */

typedef enum { VAL_NONE, VAL_LUHN, VAL_VERHOEFF, VAL_PAN } Validator;

/* Bits of Rule.extract. */
#define EXTRACT_IP     (1u << 0)
#define EXTRACT_USER   (1u << 1)

typedef struct {
    char      id[RULE_ID_MAX];
    Category  category;
    Severity  severity;
    bool      no_label;          /* category written with a trailing '*': feeds the
                                    correlator only, never produces a label (§4.2) */
    float     base;              /* prior precision, 0 < base <= 1 (§5.1) */
    bool      icase;             /* flag 'i' */
    bool      anchored_start;    /* regex begins with '^' */
    bool      anchored_end;      /* regex ends with an unescaped '$' */
    Validator validator;
    unsigned  extract;           /* EXTRACT_* bits */
    int       n_context;
    char      context[RULE_CTX_MAX][RULE_CTX_LEN];
    char     *regex;             /* full regex text, ^ and $ included (owned) */
} Rule;

typedef struct {
    Rule rules[RULES_MAX];
    int  count;
} RuleSet;

/* Load a rule file. Returns 0 on success. On failure returns -1, writes
 * "file:line: message" (or "file: message" for I/O errors) into `err`, and
 * leaves `out` empty (nothing to free). */
int rules_load(const char *path, RuleSet *out, char *err, size_t err_len);

/* Same, from a memory buffer; `name` is used in error messages. */
int rules_parse(const char *text, size_t len, const char *name,
                RuleSet *out, char *err, size_t err_len);

/* Release the regex strings. Safe on an empty or already-freed set. */
void rules_free(RuleSet *rs);

/* Index of the rule with this id, or -1. */
int rules_find(const RuleSet *rs, const char *id);

#endif /* RULES_H */
