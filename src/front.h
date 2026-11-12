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
/* HTML void elements and the void components (`void component Icon = ...`) of the source being
 * parsed: `<Icon>` needs no `/>` or closing tag. */
bool lx_void_tag(const char *tag, size_t n);
typedef struct {
    const char **names;
    int count;
} VoidComponents;
/* The names a source declares with `void component NAME`; markup is delimited while lexing,
 * before declarations are parsed, so the lexer learns them from a scan of the text. */
VoidComponents lx_scan_void_components(Arena *arena, const char *src, size_t len);
/* Sets the void components the lexer and markup parser use; returns the previous set. */
VoidComponents lx_set_void_components(VoidComponents set);
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
    N_UNITLIT, /* 100m: text is the unit (or ns.unit), args[0] the number */
    N_THIS,    /* the receiver (`this`; `this.name` parses as N_INST) */
    N_TERNARY, /* `x if c else y`: args are c, x, y */
    N_SPAWN,   /* `spawn call`: fn is a no-parameter lambda running the call */
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
    S_DESTRUCTURE,
    S_WHEN /* lowered by lower.c before compilation */
} NodeKind;

typedef struct Param {
    const char *name;
    const char *type;
    Node *value;
    int line;
    bool is_private, readonly;
    bool is_protected; /* class members: visible in the class and its subclasses */
} Param;

typedef struct FuncDecl {
    const char *name;
    const char *result; /* NULL: inferred */
    const char *owner;
    Param *params;
    int nparams;
    NodeList body;
    int line;
    bool is_abstract, is_private, is_protected;
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
    const char *alias; /* declare: negated alias of a bool (`bool on/off = true` names off) */
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
    const char **bases; /* `extends A, B`: base classes in order */
    int nbases;
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
    /* `static` members: methods without a receiver, and fields (name and access only; their
     * initializers are S_ASSIGN statements with `valueless` set, placed in the body). */
    FuncDecl *static_methods;
    int nstatic_methods;
    Param *static_fields;
    int nstatic_fields;
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
    bool is_void; /* `void component`: used like a void element, with no children */
} ComponentDecl;

/* `unitns time` with its `unit time.s = 1` declarations (see lower.c). Units apply to every
 * numeric type. For a namespace of an imported module (compiler.c), `ref` prefixes its hidden
 * names as this module sees them ("" when the bare names resolve) and `module` is the import. */
typedef struct {
    const char *name;
    int line, col;
    const char *ref, *module;
    /* unit_key() of the namespace, for equivalence methods (__fwu_as__<key>); `qual` is
     * "metric." for an imported namespace; `shadowed` when the module declares the same name */
    const char *key, *qual;
    bool shadowed;
    const char **units, **factors;
    int *unit_lines;
    int nunits, capunits;
} UnitNs;
/* `unitconv 1metric_length.m / 1time.s = 1metric_speed.mps`, or op "=" for
 * `unitconv 1length.ft = 0.3048metric.length.m` (two namespaces measuring the same thing) */
typedef struct {
    int line, col;
    const char *op;
    const char *amounts[3], *namespaces[3], *units[3];
} UnitConv;

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
    UnitNs *unitns;
    int nunitns, capunitns;
    UnitConv *unitconvs;
    int nunitconvs, capunitconvs;
} Program;

typedef struct {
    Arena *arena;
    Token *tokens;
    int ntokens;
    int pos;
    int *body_depths;
    int nbody, capbody;
    int temporaries;
    int which_depth; /* inside a `which` condition: `.name` and a leading operator use the item */
    NodeList pending;
    bool no_ternary; /* case patterns: `if` starts the guard */
    jmp_buf *jmp;
    CompileError *err;
    int line_offset;
} Parser;

void nl_push(Arena *a, NodeList *l, Node *n);
void nl_insert(Arena *a, NodeList *l, int at, Node *n);
bool parse_program(Arena *arena, const char *src, size_t len, Program *out, CompileError *err);
/* Parses without lowering units; the compiler lowers them once imports are known. */
bool parse_program_deferred(Arena *arena, const char *src, size_t len, Program *out, CompileError *err);
/* Lowers the program's units, letting unitconv relate the imported namespaces. */
bool lower_program_units(Arena *arena, Program *prog, const UnitNs *imported, int nimported, CompileError *err);
TB_NORETURN void parser_fail(Parser *P, int line, int col, const char *fmt, ...);
/* lower.c: unit namespaces become hidden quantity classes and tables; reactive variables,
 * `when`, `computed`, and negated aliases become ordinary declarations and closures. */
void lower_units(Parser *P, Program *prog, const UnitNs *imported, int nimported, bool strict);
void lower_reactive(Parser *P, Program *prog);


#endif
