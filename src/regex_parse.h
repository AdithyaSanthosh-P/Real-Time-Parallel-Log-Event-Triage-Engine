#ifndef REGEX_PARSE_H
#define REGEX_PARSE_H

/* Recursive-descent regex parser (spec §3.1, §3.2 step 1).
 *
 * Grammar, one C function per nonterminal:
 *   regex   := alt END
 *   alt     := concat ( '|' concat )*
 *   concat  := repeat+
 *   repeat  := atom ( '*' | '+' | '?' | '{' INT ( ',' INT? )? '}' )*
 *   atom    := CHAR | '.' | class | '(' alt ')' | ESCAPE | '^' | '$'
 *   class   := '[' '^'? ']'? item+ ']'          (leading ']' is literal)
 *   item    := CHAR ( '-' CHAR )?
 *
 * Anything outside the supported subset is rejected with the byte offset
 * of the offending construct; nothing is silently approximated. */

#include <stdbool.h>
#include <stddef.h>

#include "ast.h"

/* Largest repeat count accepted in {n,m}; the NFA builder makes that many
 * copies, so this bounds the automaton size. */
#define REGEX_REPEAT_MAX 1000
/* Deepest parenthesis nesting accepted (bounds the parser's recursion). */
#define REGEX_DEPTH_MAX  100

typedef struct {
    size_t pos;          /* byte offset of the error in the pattern */
    char   msg[128];
} RegexError;

/* Parse `pattern[0..len)`. With `icase`, every charset is case-folded.
 * Returns the AST (free with ast_free), or NULL with `err` filled in. */
AstNode *regex_parse(const char *pattern, size_t len, bool icase, RegexError *err);

#endif /* REGEX_PARSE_H */
