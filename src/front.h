/* Front end: tokens, AST, and parser shared between lexer.c, parser.c, and compiler.c. */
#ifndef LITE_FRONT_H
#define LITE_FRONT_H

#include "lite.h"
#include <setjmp.h>

/* Lexical helpers shared with the parser (positions are byte offsets; -1 on error). */
bool lx_start(unsigned char c);
bool lx_part(unsigned char c);
uint32_t lx_scalar_at(const char *s, size_t n, size_t p, size_t *length);
bool lx_letter_scalar(uint32_t v);
size_t lx_identifier_end(const char *s, size_t n, size_t p);
bool lx_literal_open(const char *s, size_t n, size_t p);
bool lx_literal_close(const char *s, size_t n, size_t p);
long lx_brace_end(const char *s, size_t n, size_t p);
long lx_quoted_end(const char *s, size_t n, size_t p, char quote);
bool lx_html_void(const char *tag, size_t n);
long lx_comment_end(const char *s, size_t n, size_t p);

typedef struct Node Node;
typedef struct {
    Node **items;
    int count, cap;
} NodeList;

typedef enum {
    /* expressions */
    N_INT,
    N_REAL,
    N_STRING,
    N_CHAR,
    N_BOOL,
    N_NULL,
    N_INTERP,
    N_FORMAT,
    N_NAME,
    N_MEMBER,
    N_CALL,
    N_INDEX,
    N_UNARY,
    N_BINARY,
    N_CHAIN,
    N_IS,
    N_AS,
    N_ARRAY,
    N_MAP,
    N_SET,
    N_INST,
    N_ATTR,
    N_SUPER,
    N_LAMBDA,
    N_ELEMENT,
    N_JSON,
    /* statements */
    S_EXPR,
    S_DECLARE,
    S_ASSIGN,
    S_IF,
    S_WHILE,
    S_FOR,
    S_RETURN,
    S_BREAK,
    S_CONTINUE,
    S_THROW,
    S_TRY,
    S_MATCH,
    S_DEFER,
    S_DESTRUCTURE
} NodeKind;

typedef struct Param {
    const char *name;
    const char *type;
    Node *value;
    int line;
    bool is_private, readonly;
} Param;

typedef struct FuncDecl {
    const char *name;
    const char *result; /* NULL: inferred */
    const char *owner;
    Param *params;
    int nparams;
    NodeList body;
    int line;
    bool is_abstract, is_private;
} FuncDecl;

struct Node {
    NodeKind kind;
    int line, col;
    const char *text;   /* literal, name, operator, or type text */
    NodeList args;      /* expression operands / element attributes then children */
    const char **names; /* chain operators, element attribute names */
    int nnames;
    bool grouped, outer, valueless;
    FuncDecl *fn; /* lambda */
    /* statements */
    const char *name, *type, *op;
    Node *expr, *target;
    NodeList body, other, finalizer;
    NodeList *catches;
    const char **catch_types, **catch_names;
    NodeList *patterns;
    Node **guards;
    int ncatches;
    bool has_else;
    int slot; /* pattern compilation scratch */
    int len;  /* string literal byte length (literals may hold NUL), or -1 */
};

typedef struct {
    const char *name;
    const char *base;
    const char **interfaces;
    int ninterfaces;
    Param *fields;
    int nfields;
    FuncDecl *methods;
    int nmethods;
    bool is_interface;
    const char *variant_of;
    const char **variants;
    int nvariants;
    int line;
} ClassDecl;

typedef struct {
    const char *name;
    const char **members;
    int nmembers;
    int line;
} EnumDecl;

typedef struct {
    const char *name;
    const char *alias;
    const char *kind; /* native language import: c, cxx, rust, zig */
    bool wildcard, path;
    int line;
} ImportDecl;

typedef struct {
    const char *name;
    Node *markup;
    int line;
} ComponentDecl;

typedef struct {
    ImportDecl *imports;
    int nimports;
    FuncDecl *functions;
    int nfunctions;
    ClassDecl *classes;
    int nclasses;
    EnumDecl *enums;
    int nenums;
    ComponentDecl *components;
    int ncomponents;
    const char **families; /* declared element family tags */
    int nfamilies;
    NodeList body;
    const char **privates;
    int nprivates;
    bool html, svg;
} Program;

typedef struct {
    Arena *arena;
    Token *tokens;
    int ntokens;
    int pos;
    int *body_depths;
    int nbody, capbody;
    int temporaries;
    NodeList pending;
    jmp_buf *jmp;
    CompileError *err;
    int line_offset;
} Parser;

void nl_push(Arena *a, NodeList *l, Node *n);
bool parse_program(Arena *arena, const char *src, size_t len, Program *out, CompileError *err);

#endif
