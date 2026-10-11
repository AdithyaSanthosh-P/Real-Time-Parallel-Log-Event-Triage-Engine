#ifndef AST_H
#define AST_H

/* Regex abstract syntax tree (spec §3.2 step 1).
 *
 * Node kinds:
 *   NODE_CHARSET  one byte from a 256-bit set (literals, '.', [...] all
 *                 become charsets; case folding is already applied)
 *   NODE_CONCAT   n-ary sequence;  zero children = matches the empty string
 *   NODE_ALT      n-ary alternation
 *   NODE_STAR / NODE_PLUS / NODE_QUEST   one child
 *   NODE_REPEAT   child{min,max}; max = REPEAT_INF for {n,}. Expanded into
 *                 copies by the NFA builder (§3.2: "expanded ... during NFA build")
 *   NODE_BOL / NODE_EOL   zero-width '^' / '$' assertions. The spec talks of
 *                 ^/$ only as rule-level flags, but four default rules use them
 *                 inside a group, e.g. "(^|[^0-9])", so they are assertions here;
 *                 the parser still rejects them anywhere a character could be
 *                 consumed before '^' or after '$'. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "json.h"

/* 256-bit byte set: bit b of w[b >> 6]. */
typedef struct {
    uint64_t w[4];
} CharSet;

void cs_clear(CharSet *s);
void cs_fill(CharSet *s);                        /* all 256 bytes */
void cs_add(CharSet *s, unsigned char c);
void cs_add_range(CharSet *s, unsigned char lo, unsigned char hi);
void cs_union(CharSet *s, const CharSet *t);
void cs_negate(CharSet *s);
bool cs_has(const CharSet *s, unsigned char c);
int  cs_count(const CharSet *s);
bool cs_equal(const CharSet *a, const CharSet *b);
bool cs_empty(const CharSet *s);
/* Add the other case of every ASCII letter in the set (icase rules). */
void cs_fold_case(CharSet *s);
/* Append a readable form: 'a' for one byte, else [a-z0-9] or [^...] (when
 * the complement is shorter); non-printable bytes as \xHH. */
void cs_format(const CharSet *s, StrBuf *out);

typedef enum {
    NODE_CHARSET,
    NODE_CONCAT,
    NODE_ALT,
    NODE_STAR,
    NODE_PLUS,
    NODE_QUEST,
    NODE_REPEAT,
    NODE_BOL,
    NODE_EOL
} NodeKind;

#define REPEAT_INF (-1)

typedef struct AstNode {
    NodeKind         kind;
    size_t           pos;        /* byte offset in the pattern, for errors */
    CharSet          set;        /* NODE_CHARSET */
    int              min, max;   /* NODE_REPEAT */
    int              nkids;
    struct AstNode **kids;
} AstNode;

/* Constructors return NULL on out of memory. ast_add_kid returns 0 / -1. */
AstNode *ast_new(NodeKind kind, size_t pos);
int      ast_add_kid(AstNode *parent, AstNode *kid);
void     ast_free(AstNode *n);

/* S-expression form, used by the tests and for debugging:
 *   (cat 'a' [0-9] (star 'b'))  (alt ...)  (plus X)  (quest X)
 *   (rep 2 5 X)  (rep 4 inf X)  bol  eol  (cat) for the empty string */
void ast_format(const AstNode *n, StrBuf *out);
void ast_print(const AstNode *n, FILE *f);

/* True if the subtree can consume at least one byte. */
bool ast_can_consume(const AstNode *n);

#endif /* AST_H */
