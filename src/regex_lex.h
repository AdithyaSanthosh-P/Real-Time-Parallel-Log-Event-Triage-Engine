#ifndef REGEX_LEX_H
#define REGEX_LEX_H

/* Regex lexer (spec §3.1-3.2 step 1).
 *
 * Outside bracket expressions the pattern is cut into tokens; inside
 * "[...]" the parser reads raw bytes (lex_peek_byte / lex_skip_byte),
 * because there almost every byte is literal. The lexer is where syntax
 * outside the supported subset is caught first: \d \s \w \b and other
 * unknown escapes, backreferences \1, "(?" groups, stray ']' and '}'. */

#include <stddef.h>

typedef enum {
    TK_CHAR,       /* literal byte (plain or escaped metacharacter) */
    TK_DOT,        /* . */
    TK_STAR,       /* * */
    TK_PLUS,       /* + */
    TK_QUEST,      /* ? */
    TK_LBRACE,     /* { : start of an interval {n}, {n,}, {n,m} */
    TK_PIPE,       /* | */
    TK_LPAREN,     /* ( */
    TK_RPAREN,     /* ) */
    TK_LBRACKET,   /* [ : start of a bracket expression */
    TK_CARET,      /* ^ */
    TK_DOLLAR,     /* $ */
    TK_END,
    TK_ERROR
} TokKind;

typedef struct {
    TokKind       kind;
    unsigned char c;        /* TK_CHAR: the byte */
    size_t        pos;      /* offset of the token in the pattern */
    size_t        len;      /* bytes it spans (2 for an escape) */
    char          msg[96];  /* TK_ERROR: what is wrong */
} Token;

typedef struct {
    const char *s;
    size_t      n;
    size_t      pos;        /* next unread byte */
} Lexer;

void  lex_init(Lexer *L, const char *s, size_t n);

/* The token at the current position, without consuming it. */
Token lex_peek(const Lexer *L);

/* Consume a token returned by lex_peek(). */
void  lex_advance(Lexer *L, const Token *t);

/* Raw byte access for bracket expressions and interval counts:
 * lex_peek_byte returns the byte `ahead` positions on, or -1 past the end. */
int   lex_peek_byte(const Lexer *L, size_t ahead);
void  lex_skip_byte(Lexer *L);

#endif /* REGEX_LEX_H */
