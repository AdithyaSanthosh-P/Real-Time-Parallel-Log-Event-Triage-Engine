#ifndef BP_PATTERNS_H
#define BP_PATTERNS_H

/* The base paper's five patterns, verbatim from its §3.3 pattern database.
 * Every Phase R variant (spec §10.4) uses this one definition. All are
 * compiled with REG_EXTENDED. The index is the "core number" in the paper's
 * §6 screenshots (Core 0 EMAIL ... Core 4 NAME). Do not edit the regexes:
 * Phase R reproduces the paper, including patterns that behave badly. */

#define BP_NPAT 5

typedef struct {
    const char *name;
    const char *regex;
} BpPattern;

static const BpPattern BP_PATTERNS[BP_NPAT] = {
    /* 0 */ { "EMAIL",    "[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}" },
    /* 1 */ { "PHONE",    "[6-9][0-9]{9}" },
    /* 2 */ { "QUESTION", ".*\\?$" },
    /* 3 */ { "GREETING", "^(hello|hi|hey|good morning|good afternoon)" },
    /* 4 */ { "NAME",     "^[A-Z][a-z]+ [A-Z][a-z]+$" },
};

#endif /* BP_PATTERNS_H */
