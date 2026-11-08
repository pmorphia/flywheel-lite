/* Recursive-descent parser for Flywheel, ported from the reference compiler's front end. */
#include "front.h"
#include <ctype.h>

/* ---------------------------------------------------------------- helpers */

void nl_push(Arena *a, NodeList *l, Node *n) {
    if (l->count == l->cap) {
        int nc = GROW_CAP(l->cap);
        Node **items = arena_alloc(a, sizeof(Node *) * (size_t)nc);
        if (l->count)
            memcpy(items, l->items, sizeof(Node *) * (size_t)l->count);
        l->items = items;
        l->cap = nc;
    }
    l->items[l->count++] = n;
}

void nl_insert(Arena *a, NodeList *l, int at, Node *n) {
    nl_push(a, l, n);
    memmove(&l->items[at + 1], &l->items[at], sizeof(Node *) * (size_t)(l->count - 1 - at));
    l->items[at] = n;
}

#define GROWV(a, arr, count, cap, T)                                                              \
    do {                                                                                          \
        if ((count) == (cap)) {                                                                   \
            int nc_ = GROW_CAP(cap);                                                              \
            T *n_ = arena_alloc((a), sizeof(T) * (size_t)nc_);                                    \
            if (count)                                                                            \
                memcpy(n_, (arr), sizeof(T) * (size_t)(count));                                   \
            (arr) = n_;                                                                           \
            (cap) = nc_;                                                                          \
        }                                                                                         \
    } while (0)

typedef struct {
    char *d;
    size_t n, cap;
} SBuf;
static void sb_add(SBuf *b, const char *s, size_t n) {
    if (n > 0x7fffffffu)
        abort();
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->d = xrealloc(b->d, b->cap);
    }
    if (n)
        memcpy(b->d + b->n, s, n);
    b->n += n;
    b->d[b->n] = 0;
}
static void sb_adds(SBuf *b, const char *s) { sb_add(b, s, strlen(s)); }
static void sb_addc(SBuf *b, char c) { sb_add(b, &c, 1); }
static void sb_utf8(SBuf *b, uint32_t v) {
    char t[4];
    if (v < 0x80) {
        sb_addc(b, (char)v);
    } else if (v < 0x800) {
        t[0] = (char)(0xc0 | (v >> 6));
        t[1] = (char)(0x80 | (v & 63));
        sb_add(b, t, 2);
    } else if (v < 0x10000) {
        t[0] = (char)(0xe0 | (v >> 12));
        t[1] = (char)(0x80 | ((v >> 6) & 63));
        t[2] = (char)(0x80 | (v & 63));
        sb_add(b, t, 3);
    } else {
        t[0] = (char)(0xf0 | (v >> 18));
        t[1] = (char)(0x80 | ((v >> 12) & 63));
        t[2] = (char)(0x80 | ((v >> 6) & 63));
        t[3] = (char)(0x80 | (v & 63));
        sb_add(b, t, 4);
    }
}

TB_NORETURN static void pfail_at(Parser *P, int line, int col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(P->err->message, sizeof P->err->message, fmt, ap);
    va_end(ap);
    P->err->line = line;
    P->err->col = col;
    longjmp(*P->jmp, 1);
}

void parser_fail(Parser *P, int line, int col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(P->err->message, sizeof P->err->message, fmt, ap);
    va_end(ap);
    P->err->line = line;
    P->err->col = col;
    longjmp(*P->jmp, 1);
}

static int pindex(Parser *P, int n) {
    int active = P->nbody ? P->body_depths[P->nbody - 1] : 0;
    int i = P->pos;
    for (;;) {
        while (i + 1 < P->ntokens && P->tokens[i].kind == T_NL && P->tokens[i].depth != active)
            ++i;
        if (n == 0 || i + 1 >= P->ntokens)
            return i;
        --n;
        ++i;
    }
}
static Token *peek(Parser *P, int n) { return &P->tokens[pindex(P, n)]; }
static Token *take(Parser *P) {
    int i = pindex(P, 0);
    Token *t = &P->tokens[i];
    P->pos = t->kind == T_EOF ? i : i + 1;
    return t;
}
static bool tis(const Token *t, const char *s) {
    return (size_t)t->len == strlen(s) && memcmp(t->text, s, (size_t)t->len) == 0;
}
static bool literal_kind(const Token *t) {
    return t->kind == T_STRING || t->kind == T_MARKUP || t->kind == T_JSON || t->kind == T_CHAR;
}
static bool ptext(Parser *P, int n, const char *s) {
    Token *t = peek(P, n);
    return tis(t, s) && !literal_kind(t);
}
static bool accept(Parser *P, const char *s) {
    if (ptext(P, 0, s)) {
        take(P);
        return true;
    }
    return false;
}
static char *tstr(Parser *P, const Token *t) { return arena_strndup(P->arena, t->text, (size_t)t->len); }
TB_NORETURN static void pfail(Parser *P, const char *fmt, const char *arg) {
    Token *t = peek(P, 0);
    pfail_at(P, t->line, t->col, fmt, arg);
}
static Token *expect(Parser *P, const char *s) {
    Token *t = peek(P, 0);
    if (!ptext(P, 0, s)) {
        char got[64];
        snprintf(got, sizeof got, "%.*s", t->kind == T_NL ? 7 : t->kind == T_EOF ? 11 : (t->len > 40 ? 40 : t->len),
                 t->kind == T_NL ? "newline" : t->kind == T_EOF ? "end of file" : t->text);
        pfail_at(P, t->line, t->col, "expected '%s', got '%s'", s, got);
    }
    return take(P);
}
static const char *reserved_words[] = {
    "import", "as", "var", "const", "class", "function", "component", "returns", "return", "if",
    "else", "while", "do", "for", "in", "end", "break", "continue", "try", "catch",
    "finally", "throw", "inst", "attr", "true", "false", "null", "public", "private", "void",
    "weak", "and", "or", "not", "enum", "interface", "extends", "implements", "match", "case",
    "super", "is", "elements", "defer", "which", NULL};
static bool in_list(const Token *t, const char **list) {
    for (int i = 0; list[i]; i++)
        if (tis(t, list[i]))
            return true;
    return false;
}
static Token *identifier(Parser *P) {
    Token *t = peek(P, 0);
    if (t->kind != T_ID || in_list(t, reserved_words))
        pfail_at(P, t->line, t->col, "expected identifier%s", "");
    return take(P);
}
static void newlines(Parser *P) {
    while (peek(P, 0)->kind == T_NL)
        take(P);
}
static void finish(Parser *P) {
    Token *t = peek(P, 0);
    if (t->kind != T_NL && t->kind != T_EOF) {
        char got[64];
        snprintf(got, sizeof got, "%.*s", t->len > 40 ? 40 : t->len, t->text);
        pfail_at(P, t->line, t->col, "expected end of statement, got '%s'", got);
    }
    newlines(P);
}
static Node *node(Parser *P, NodeKind kind, const char *text, int line, int col) {
    Node *n = arena_alloc(P->arena, sizeof(Node));
    n->kind = kind;
    n->text = text;
    n->len = -1;
    n->line = line;
    n->col = col;
    return n;
}
static Node *tnode(Parser *P, NodeKind kind, const char *text, const Token *t) {
    return node(P, kind, text, t->line, t->col);
}

/* Speculative parsing: run fn; on a parse error restore the position and return false. */
typedef void (*SpecFn)(Parser *P, void *ctx);
static bool speculate(Parser *P, SpecFn fn, void *ctx) {
    jmp_buf jb;
    jmp_buf *saved = P->jmp;
    int pos = P->pos;
    int nbody = P->nbody, which_depth = P->which_depth;
    if (setjmp(jb)) {
        P->jmp = saved;
        P->pos = pos;
        P->nbody = nbody;
        P->which_depth = which_depth;
        return false;
    }
    P->jmp = &jb;
    fn(P, ctx);
    P->jmp = saved;
    return true;
}

static int precedence(const Token *t) {
    if (t->kind != T_OP && t->kind != T_ID)
        return 0;
    if (tis(t, "or"))
        return 1;
    if (tis(t, "and"))
        return 2;
    if (tis(t, "==") || tis(t, "!="))
        return 3;
    if (tis(t, "<") || tis(t, ">") || tis(t, "<=") || tis(t, ">=") || tis(t, "in") || tis(t, "is"))
        return 4;
    if (tis(t, "as"))
        return 9;
    if (tis(t, "??"))
        return 5;
    if (tis(t, "..") || tis(t, "..<"))
        return 6;
    if (tis(t, "+") || tis(t, "-"))
        return 7;
    if (tis(t, "*") || tis(t, "/") || tis(t, "//") || tis(t, "%"))
        return 8;
    return 0;
}
static int prec_of(const char *s) {
    Token t;
    t.kind = T_OP;
    t.text = s;
    t.len = (int)strlen(s);
    return precedence(&t);
}
static bool comparison(const char *s) {
    return !strcmp(s, "==") || !strcmp(s, "!=") || !strcmp(s, "<") || !strcmp(s, ">") ||
           !strcmp(s, "<=") || !strcmp(s, ">=");
}

/* ---------------------------------------------------------------- types */

static void type_into(Parser *P, SBuf *r);
static const char *parse_type(Parser *P) {
    SBuf b = {0};
    type_into(P, &b);
    char *s = arena_strndup(P->arena, b.d ? b.d : "", b.n);
    free(b.d);
    return s;
}
static void type_into(Parser *P, SBuf *r) {
    if (accept(P, "[")) {
        sb_addc(r, '[');
        type_into(P, r);
        while (accept(P, ",")) {
            sb_addc(r, ',');
            type_into(P, r);
        }
        expect(P, "]");
        sb_addc(r, ']');
    } else if (accept(P, "(")) {
        type_into(P, r);
        expect(P, ")");
    } else if (ptext(P, 0, "function") && ptext(P, 1, "(")) {
        take(P);
        take(P);
        SBuf params = {0};
        int np = 0;
        if (!accept(P, ")")) {
            do {
                if (np++)
                    sb_addc(&params, ',');
                type_into(P, &params);
            } while (accept(P, ","));
            expect(P, ")");
        }
        sb_adds(r, "function<");
        if (accept(P, "returns"))
            type_into(P, r);
        else
            sb_adds(r, "void");
        if (np) {
            sb_addc(r, ',');
            sb_add(r, params.d, params.n);
        }
        free(params.d);
        sb_addc(r, '>');
        return;
    } else {
        Token *t = take(P);
        if (t->kind != T_ID)
            pfail_at(P, t->line, t->col, "expected type%s", "");
        size_t start = r->n;
        sb_add(r, t->text, (size_t)t->len);
        while (accept(P, ".")) {
            Token *m = identifier(P);
            sb_addc(r, '.');
            sb_add(r, m->text, (size_t)m->len);
        }
        if (accept(P, "<")) {
            sb_addc(r, '<');
            type_into(P, r);
            while (accept(P, ",")) {
                sb_addc(r, ',');
                type_into(P, r);
            }
            expect(P, ">");
            sb_addc(r, '>');
        }
        /* int[time], float[time], u8[time], float[metric.length]: a quantity in a unit namespace
         * (its hidden class, lower.c). Integer types name the integer class and float/double the
         * floating-point one; other builtin types keep their name so the compiler can say that
         * units apply only to numbers. */
        static const char *integers[] = {"int", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", NULL};
        static const char *others[] = {"string", "bool", "char", "void", "element", "fragment", NULL};
        bool integer = false, other = false;
        for (int i = 0; integers[i]; i++)
            integer = integer || tis(t, integers[i]);
        for (int i = 0; others[i]; i++)
            other = other || tis(t, others[i]);
        bool qualified = ptext(P, 2, ".") && peek(P, 3)->kind == T_ID && ptext(P, 4, "]");
        if ((integer || other || tis(t, "float") || tis(t, "double")) && r->n == start + (size_t)t->len &&
            ptext(P, 0, "[") && peek(P, 1)->kind == T_ID && (ptext(P, 2, "]") || qualified)) {
            take(P);
            Token *module = NULL;
            if (qualified) {
                module = identifier(P);
                take(P);
            }
            Token *ns = identifier(P);
            take(P);
            r->n = start;
            if (module) {
                sb_add(r, module->text, (size_t)module->len);
                sb_addc(r, '.');
            }
            sb_adds(r, "__fwu_");
            sb_add(r, ns->text, (size_t)ns->len);
            sb_adds(r, "__");
            if (integer)
                sb_adds(r, "int");
            else if (other)
                sb_add(r, t->text, (size_t)t->len);
            else
                sb_adds(r, "double");
        }
    }
    for (;;) {
        if (accept(P, "?"))
            sb_addc(r, '?');
        else if (ptext(P, 0, "[") && ptext(P, 1, "]")) {
            take(P);
            take(P);
            sb_adds(r, "[]");
        } else
            break;
    }
}
static void skip_type_params(Parser *P) {
    if (!accept(P, "<"))
        return;
    do {
        identifier(P);
        if (accept(P, "implements") || accept(P, "extends") || accept(P, "is"))
            parse_type(P);
    } while (accept(P, ","));
    expect(P, ">");
}
static void spec_typed_decl(Parser *P, void *ctx) {
    parse_type(P);
    bool yes = peek(P, 0)->kind == T_ID && ptext(P, 1, "=");
    *(bool *)ctx = yes;
}
static bool typed_declaration(Parser *P) {
    if (ptext(P, 0, "inst") || ptext(P, 0, "attr") || ptext(P, 0, "super"))
        return false;
    int pos = P->pos;
    bool yes = false;
    if (!speculate(P, spec_typed_decl, &yes))
        yes = false;
    P->pos = pos;
    return yes;
}

/* `name<T, U>` without a space before '<' supplies type arguments (speculative). */
static void spec_type_args(Parser *P, void *ctx) {
    SBuf *b = ctx;
    take(P);
    sb_addc(b, '<');
    type_into(P, b);
    while (accept(P, ",")) {
        sb_addc(b, ',');
        type_into(P, b);
    }
    expect(P, ">");
    sb_addc(b, '>');
}
static const char *with_type_args(Parser *P, const Token *name, bool allow_space) {
    Token *t = peek(P, 0);
    char *base = tstr(P, name);
    if (!tis(t, "<") || (t->spaced && !allow_space) || t->kind != T_OP)
        return base;
    SBuf b = {0};
    sb_adds(&b, base);
    int saved = P->pos;
    if (!speculate(P, spec_type_args, &b)) {
        P->pos = saved;
        free(b.d);
        return base;
    }
    char *r = arena_strndup(P->arena, b.d, b.n);
    free(b.d);
    return r;
}

/* ---------------------------------------------------------------- strings and markup */

static Node *expression(Parser *P, int level);
static Node *parse_sub_expression(Parser *P, const char *text, size_t n, int line, int col,
                                  const char *what);

static Node *string_expr(Parser *P, const char *s, size_t n, int line, int col) {
    Node *result = node(P, N_INTERP, NULL, line, col);
    SBuf text = {0};
    int line_here = line;
#define FLUSH()                                                                                   \
    do {                                                                                          \
        if (text.n) {                                                                             \
            Node *lit_ = node(P, N_STRING, arena_strndup(P->arena, text.d, text.n), line, col);  \
            lit_->len = (int)text.n;                                                              \
            nl_push(P->arena, &result->args, lit_);                                               \
            text.n = 0;                                                                           \
            text.d[0] = 0;                                                                        \
        }                                                                                         \
    } while (0)
    for (size_t p = 0; p < n; ++p) {
        char c = s[p];
        if (c == '\n')
            line_here++;
        if (c == '\\') {
            if (++p == n)
                pfail_at(P, line, col, "incomplete escape%s", "");
            switch (s[p]) {
            case 'n': sb_addc(&text, '\n'); break;
            case 'r': sb_addc(&text, '\r'); break;
            case 't': sb_addc(&text, '\t'); break;
            case '\\': sb_addc(&text, '\\'); break;
            case '"': sb_addc(&text, '"'); break;
            case '\'': sb_addc(&text, '\''); break;
            case 'u': {
                if (++p == n || s[p] != '{')
                    pfail_at(P, line, col, "expected \\u{HEX}%s", "");
                size_t e = p;
                while (e < n && s[e] != '}')
                    e++;
                if (e >= n)
                    pfail_at(P, line, col, "incomplete Unicode escape%s", "");
                uint32_t v = 0;
                size_t digits = 0;
                for (size_t q = p + 1; q < e; q++) {
                    int d = isdigit((unsigned char)s[q])   ? s[q] - '0'
                            : s[q] >= 'a' && s[q] <= 'f' ? s[q] - 'a' + 10
                            : s[q] >= 'A' && s[q] <= 'F' ? s[q] - 'A' + 10
                                                         : -1;
                    if (d < 0 || v > 0x10ffff)
                        pfail_at(P, line, col, "invalid Unicode escape%s", "");
                    v = v * 16 + (uint32_t)d;
                    digits++;
                }
                if (!digits || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
                    pfail_at(P, line, col, "invalid Unicode escape%s", "");
                sb_utf8(&text, v);
                p = e;
                break;
            }
            default:
                pfail_at(P, line, col, "unknown string escape '\\%c'", s[p]);
            }
            continue;
        }
        if ((c == '{' || c == '}') && p + 1 < n && s[p + 1] == c) {
            sb_addc(&text, c);
            ++p;
            continue;
        }
        if (lx_literal_open(s, n, p) || lx_literal_close(s, n, p)) {
            sb_addc(&text, c);
            continue;
        }
        if (c == '{') {
            FLUSH();
            long e = lx_brace_end(s, n, p);
            if (e < 0)
                pfail_at(P, line, col, "unterminated interpolation%s", "");
            const char *inner = s + p + 1;
            size_t ilen = (size_t)e - p - 1, full = ilen;
            const char *spec = NULL;
            int depth = 0;
            char quote = 0;
            for (size_t i = 0; i < ilen; ++i) {
                char d = inner[i];
                if (quote) {
                    if (d == '\\')
                        ++i;
                    else if (d == quote)
                        quote = 0;
                } else if (d == '"' || d == '\'')
                    quote = d;
                else if (d == '(' || d == '[' || d == '{')
                    ++depth;
                else if (d == ')' || d == ']' || d == '}')
                    --depth;
                else if (d == ':' && depth == 0) {
                    spec = arena_strndup(P->arena, inner + i + 1, ilen - i - 1);
                    ilen = i;
                    break;
                }
            }
            Node *value = parse_sub_expression(P, inner, ilen, line_here, col, "interpolation");
            if (spec || ilen < full) {
                Node *f = node(P, N_FORMAT, spec ? spec : "", line, col);
                nl_push(P->arena, &f->args, value);
                value = f;
            }
            nl_push(P->arena, &result->args, value);
            p = (size_t)e;
        } else if (c == '}') {
            pfail_at(P, line, col, "literal '}' must be doubled or set off by whitespace%s", "");
        } else
            sb_addc(&text, c);
    }
    FLUSH();
#undef FLUSH
    free(text.d);
    if (result->args.count == 0)
        return node(P, N_STRING, "", line, col);
    if (result->args.count == 1 && result->args.items[0]->kind == N_STRING)
        return result->args.items[0];
    return result;
}

static const char *entities(Parser *P, const char *text, size_t n, int line) {
    SBuf out = {0};
    sb_add(&out, "", 0);
    for (size_t p = 0; p < n; ++p) {
        if (text[p] != '&') {
            sb_addc(&out, text[p]);
            continue;
        }
        size_t end = p;
        while (end < n && text[end] != ';')
            end++;
        if (end >= n)
            pfail_at(P, line, 1, "markup '&' must begin an entity (use &amp;)%s", "");
        const char *name = text + p + 1;
        size_t nl = end - p - 1;
#define IS(s) (nl == strlen(s) && memcmp(name, s, nl) == 0)
        if (IS("amp"))
            sb_addc(&out, '&');
        else if (IS("lt"))
            sb_addc(&out, '<');
        else if (IS("gt"))
            sb_addc(&out, '>');
        else if (IS("quot"))
            sb_addc(&out, '"');
        else if (IS("apos"))
            sb_addc(&out, '\'');
        else if (IS("nbsp"))
            sb_utf8(&out, 0xa0);
        else if (nl > 1 && name[0] == '#') {
            bool hex = nl > 2 && (name[1] == 'x' || name[1] == 'X');
            uint32_t v = 0;
            size_t q = hex ? 2 : 1;
            if (q >= nl)
                pfail_at(P, line, 1, "invalid numeric entity%s", "");
            for (; q < nl; q++) {
                char d = name[q];
                int dv = isdigit((unsigned char)d) ? d - '0'
                         : hex && d >= 'a' && d <= 'f' ? d - 'a' + 10
                         : hex && d >= 'A' && d <= 'F' ? d - 'A' + 10
                                                       : -1;
                if (dv < 0 || v > 0x10ffff)
                    pfail_at(P, line, 1, "invalid numeric entity%s", "");
                v = v * (hex ? 16u : 10u) + (uint32_t)dv;
            }
            if (v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
                pfail_at(P, line, 1, "invalid Unicode scalar%s", "");
            sb_utf8(&out, v);
        } else {
            char nm[64];
            snprintf(nm, sizeof nm, "%.*s", (int)(nl > 40 ? 40 : nl), name);
            pfail_at(P, line, 1, "unknown markup entity '&%s;'", nm);
        }
#undef IS
        p = end;
    }
    char *r = arena_strndup(P->arena, out.d, out.n);
    free(out.d);
    return r;
}

typedef struct {
    Parser *P;
    const char *s;
    size_t n, p;
    int line, col;
} MarkupCtx;

static int mk_line(MarkupCtx *m) {
    int l = m->line;
    for (size_t i = 0; i < m->p && i < m->n; i++)
        if (m->s[i] == '\n')
            l++;
    return l;
}
static void mk_space(MarkupCtx *m) {
    while (m->p < m->n && isspace((unsigned char)m->s[m->p]))
        m->p++;
}
static const char *mk_name(MarkupCtx *m) {
    size_t begin = m->p;
    while (m->p < m->n && (lx_part((unsigned char)m->s[m->p]) || m->s[m->p] == '-' ||
                           m->s[m->p] == ':' || m->s[m->p] == '.'))
        m->p++;
    if (begin == m->p)
        pfail_at(m->P, mk_line(m), m->col, "expected markup name%s", "");
    return arena_strndup(m->P->arena, m->s + begin, m->p - begin);
}
static Node *mk_embedded(MarkupCtx *m) {
    long end = lx_brace_end(m->s, m->n, m->p);
    if (end < 0)
        pfail_at(m->P, mk_line(m), m->col, "unterminated interpolation%s", "");
    Node *v = parse_sub_expression(m->P, m->s + m->p + 1, (size_t)end - m->p - 1, mk_line(m), m->col,
                                   "markup interpolation");
    m->p = (size_t)end + 1;
    return v;
}
static Node *mk_element(MarkupCtx *m) {
    Parser *P = m->P;
    const char *s = m->s;
    size_t n = m->n;
    if (m->p >= n || s[m->p++] != '<')
        pfail_at(P, mk_line(m), m->col, "expected opening tag%s", "");
    const char *tag = "";
    if (m->p < n && s[m->p] != '>')
        tag = mk_name(m);
    Node *e = node(P, N_ELEMENT, tag, mk_line(m), m->col);
    int ncap = 0;
    mk_space(m);
    while (m->p < n && s[m->p] != '>' && s[m->p] != '/') {
        const char *a = mk_name(m);
        for (int i = 0; i < e->nnames; i++)
            if (!strcmp(e->names[i], a))
                pfail_at(P, mk_line(m), m->col, "duplicate attribute '%s'", a);
        mk_space(m);
        Node *value;
        if (m->p == n || s[m->p] != '=') {
            GROWV(P->arena, e->names, e->nnames, ncap, const char *);
            e->names[e->nnames++] = a;
            Node *v = node(P, N_STRING, "", mk_line(m), m->col);
            v->valueless = true;
            nl_push(P->arena, &e->args, v);
            continue;
        }
        m->p++;
        mk_space(m);
        if (m->p < n && (s[m->p] == '"' || s[m->p] == '\'')) {
            long end = lx_quoted_end(s, n, m->p, s[m->p]);
            if (end < 0)
                pfail_at(P, mk_line(m), m->col, "unterminated attribute value%s", "");
            value = string_expr(P, s + m->p + 1, (size_t)end - m->p - 1, mk_line(m), m->col);
            if (value->kind == N_STRING) {
                value->text = entities(P, value->text, strlen(value->text), mk_line(m));
                value->len = -1;
            } else if (value->kind == N_INTERP)
                for (int i = 0; i < value->args.count; i++)
                    if (value->args.items[i]->kind == N_STRING) {
                        value->args.items[i]->text =
                            entities(P, value->args.items[i]->text, strlen(value->args.items[i]->text), mk_line(m));
                        value->args.items[i]->len = -1;
                    }
            m->p = (size_t)end + 1;
        } else if (m->p < n && s[m->p] == '{') {
            value = mk_embedded(m);
        } else {
            size_t begin = m->p;
            while (m->p < n && !isspace((unsigned char)s[m->p]) && s[m->p] != '>' &&
                   !(s[m->p] == '/' && m->p + 1 < n && s[m->p + 1] == '>')) {
                char c = s[m->p];
                if (c == '"' || c == '\'' || c == '<' || c == '=' || c == '`' || c == '{' || c == '}')
                    pfail_at(P, mk_line(m), m->col,
                             "unquoted attribute values cannot contain %c; quote the value", c);
                m->p++;
            }
            if (begin == m->p)
                pfail_at(P, mk_line(m), m->col, "attribute '%s' needs a value after '='", a);
            value = node(P, N_STRING, entities(P, s + begin, m->p - begin, mk_line(m)), mk_line(m), m->col);
        }
        GROWV(P->arena, e->names, e->nnames, ncap, const char *);
        e->names[e->nnames++] = a;
        nl_push(P->arena, &e->args, value);
        mk_space(m);
    }
    if (m->p < n && s[m->p] == '/') {
        m->p++;
        if (m->p == n || s[m->p++] != '>')
            pfail_at(P, mk_line(m), m->col, "expected '/>'%s", "");
        return e;
    }
    if (m->p == n || s[m->p++] != '>')
        pfail_at(P, mk_line(m), m->col, "expected '>'%s", "");
    if (lx_html_void(tag, strlen(tag)))
        return e;
    SBuf text = {0};
#define MFLUSH()                                                                                  \
    do {                                                                                          \
        if (text.n) {                                                                             \
            bool has_nl = memchr(text.d, '\n', text.n) != NULL, blank = true;                     \
            for (size_t i_ = 0; i_ < text.n; i_++)                                                \
                if (!strchr(" \r\n\t", text.d[i_]))                                               \
                    blank = false;                                                                \
            if (!(has_nl && blank))                                                               \
                nl_push(P->arena, &e->args,                                                       \
                        node(P, N_STRING, entities(P, text.d, text.n, mk_line(m)), mk_line(m), m->col)); \
            text.n = 0;                                                                           \
        }                                                                                         \
    } while (0)
    while (m->p < n) {
        if (m->p + 3 < n && memcmp(s + m->p, "<!--", 4) == 0) {
            long q = lx_comment_end(s, n, m->p);
            if (q < 0)
                pfail_at(P, mk_line(m), m->col, "unterminated markup comment: expected '-->'%s", "");
            m->p = (size_t)q;
            continue;
        }
        if (s[m->p] == '<' && m->p + 1 < n && s[m->p + 1] == '/') {
            MFLUSH();
            m->p += 2;
            const char *close = "";
            if (m->p < n && s[m->p] != '>')
                close = mk_name(m);
            mk_space(m);
            if (m->p == n || s[m->p++] != '>')
                pfail_at(P, mk_line(m), m->col, "expected closing '>'%s", "");
            if (strcmp(close, tag) != 0) {
                char msg[256];
                snprintf(msg, sizeof msg, "mismatched closing tag: expected </%s>, got </%s>", tag, close);
                pfail_at(P, mk_line(m), m->col, "%s", msg);
            }
            free(text.d);
            return e;
        }
        if (s[m->p] == '<') {
            MFLUSH();
            nl_push(P->arena, &e->args, mk_element(m));
        } else if ((s[m->p] == '{' || s[m->p] == '}') && m->p + 1 < n && s[m->p + 1] == s[m->p]) {
            sb_addc(&text, s[m->p]);
            m->p += 2;
        } else if (lx_literal_open(s, n, m->p) || lx_literal_close(s, n, m->p)) {
            sb_addc(&text, s[m->p++]);
        } else if (s[m->p] == '{') {
            MFLUSH();
            nl_push(P->arena, &e->args, mk_embedded(m));
        } else if (s[m->p] == '}') {
            pfail_at(P, mk_line(m), m->col, "literal '}' must be doubled or set off by whitespace%s", "");
        } else
            sb_addc(&text, s[m->p++]);
    }
#undef MFLUSH
    free(text.d);
    pfail_at(P, mk_line(m), m->col, "missing closing tag for <%s>", tag);
}
static Node *parse_markup(Parser *P) {
    Token *t = take(P);
    MarkupCtx m = {P, t->text, (size_t)t->len, 0, t->line, t->col};
    Node *e = mk_element(&m);
    if (m.p != m.n)
        pfail_at(P, t->line, t->col, "unexpected text after markup%s", "");
    return e;
}

/* ---------------------------------------------------------------- expressions */

static Node *lambda(Parser *P);

static Node *prefix(Parser *P) {
    Token *t = peek(P, 0);
    Node *e = NULL;
    if (t->kind == T_MARKUP) {
        e = parse_markup(P);
    } else if (t->kind == T_JSON) {
        take(P);
        e = tnode(P, N_JSON, tstr(P, t), t);
    } else if (t->kind == T_STRING) {
        take(P);
        e = string_expr(P, t->text, (size_t)t->len, t->line, t->col);
    } else if (P->which_depth > 0 && t->kind == T_OP && tis(t, ".") && peek(P, 1)->kind == T_ID) {
        /* `.name` inside a `which` condition reads a member of the item being tested. */
        e = tnode(P, N_NAME, "$which", t);
    } else if (P->which_depth > 0 &&
               ((t->kind == T_OP && (tis(t, "==") || tis(t, "!=") || tis(t, "<") || tis(t, ">") ||
                                     tis(t, "<=") || tis(t, ">=") || tis(t, "*") || tis(t, "/") ||
                                     tis(t, "//") || tis(t, "%"))) ||
                (t->kind == T_ID && (tis(t, "in") || tis(t, "is"))))) {
        /* A condition that starts with an operator compares the item: `which > 10`. */
        return tnode(P, N_NAME, "$which", t);
    } else if (t->kind == T_INT || t->kind == T_REAL) {
        take(P);
        e = tnode(P, t->kind == T_INT ? N_INT : N_REAL, tstr(P, t), t);
        /* A quantity literal: a number with a unit right after it, 100m, 0.32km, 15time.s. */
        static const char *not_units[] = {"and", "or", "not", "in", "is", "as", "do", "else",
                                          "end", "which", "with", "returns", "if", "for", "while",
                                          "true", "false", "null", NULL};
        Token *u = peek(P, 0);
        if (u->kind == T_ID && !u->spaced && !in_list(u, not_units)) {
            take(P);
            SBuf unit = {0};
            sb_add(&unit, u->text, (size_t)u->len);
            if (ptext(P, 0, ".") && !peek(P, 0)->spaced && peek(P, 1)->kind == T_ID && !peek(P, 1)->spaced) {
                take(P);
                Token *m = take(P);
                sb_addc(&unit, '.');
                sb_add(&unit, m->text, (size_t)m->len);
            }
            Node *q = tnode(P, N_UNITLIT, arena_strndup(P->arena, unit.d, unit.n), t);
            free(unit.d);
            nl_push(P->arena, &q->args, e);
            e = q;
        }
    } else if (t->kind == T_CHAR) {
        take(P);
        Node *text = string_expr(P, t->text, (size_t)t->len, t->line, t->col);
        size_t len = 0, tl = text->kind == N_STRING ? strlen(text->text) : 0;
        if (tl)
            lx_scalar_at(text->text, tl, 0, &len);
        if (!tl || len != tl)
            pfail_at(P, t->line, t->col, "character literals hold exactly one character, such as 'a'%s", "");
        e = tnode(P, N_CHAR, text->text, t);
    } else if (accept(P, "true") || accept(P, "false")) {
        e = tnode(P, N_BOOL, tstr(P, t), t);
    } else if (accept(P, "null")) {
        e = tnode(P, N_NULL, "", t);
    } else if (accept(P, "-") || accept(P, "+") || accept(P, "not")) {
        e = tnode(P, N_UNARY, tstr(P, t), t);
        nl_push(P->arena, &e->args, expression(P, tis(t, "not") ? 2 : 9));
        return e;
    } else if (accept(P, "(")) {
        e = expression(P, 0);
        expect(P, ")");
        e->grouped = true;
    } else if (accept(P, "[")) {
        e = tnode(P, N_ARRAY, "", t);
        if (!accept(P, "]")) {
            do {
                nl_push(P->arena, &e->args, expression(P, 0));
            } while (accept(P, ","));
            expect(P, "]");
        }
    } else if (accept(P, "{")) {
        e = tnode(P, N_MAP, "", t);
        if (!accept(P, "}")) {
            nl_push(P->arena, &e->args, expression(P, 0));
            if (!accept(P, ":"))
                e->kind = N_SET;
            else
                nl_push(P->arena, &e->args, expression(P, 0));
            while (accept(P, ",")) {
                nl_push(P->arena, &e->args, expression(P, 0));
                if (e->kind == N_MAP) {
                    expect(P, ":");
                    nl_push(P->arena, &e->args, expression(P, 0));
                }
            }
            expect(P, "}");
        }
    } else if (accept(P, "inst") || accept(P, "attr")) {
        Token *name = identifier(P);
        e = tnode(P, tis(t, "inst") ? N_INST : N_ATTR, tstr(P, name), t);
    } else if (accept(P, "super")) {
        e = tnode(P, N_SUPER, "", t);
    } else if (t->kind == T_ID && tis(t, "function") && ptext(P, 1, "(")) {
        e = lambda(P);
    } else if (t->kind == T_ID && tis(t, "weak") && ptext(P, 1, "(")) {
        take(P);
        e = tnode(P, N_NAME, "weak", t);
    } else if (t->kind == T_ID) {
        static const char *structural[] = {
            "function", "class", "component", "import", "public", "private", "returns", "return",
            "if", "else", "while", "do", "for", "in", "end", "break", "continue", "try",
            "catch", "finally", "throw", "as", "void", "weak", "and", "or", "enum", "interface",
            "extends", "implements", "match", "case", "is", "which", NULL};
        if (in_list(t, structural)) {
            char nm[64];
            snprintf(nm, sizeof nm, "%.*s", t->len, t->text);
            pfail_at(P, t->line, t->col, "unexpected keyword '%s' in expression", nm);
        }
        take(P);
        static const char *builtin_generics[] = {"array", "tuple", "map", "set", "Result", "weak", NULL};
        e = tnode(P, N_NAME, with_type_args(P, t, in_list(t, builtin_generics)), t);
    } else {
        char got[64];
        snprintf(got, sizeof got, "%.*s", t->kind == T_NL ? 7 : t->kind == T_EOF ? 11 : (t->len > 40 ? 40 : t->len),
                 t->kind == T_NL ? "newline" : t->kind == T_EOF ? "end of file" : t->text);
        pfail_at(P, t->line, t->col, "expected expression, got '%s'", got);
    }
    for (;;) {
        if (accept(P, ".")) {
            Token *m = identifier(P);
            Node *n = tnode(P, N_MEMBER, with_type_args(P, m, false), m);
            nl_push(P->arena, &n->args, e);
            e = n;
        } else if (accept(P, "(")) {
            Node *n = tnode(P, N_CALL, "", t);
            nl_push(P->arena, &n->args, e);
            if (!accept(P, ")")) {
                do {
                    nl_push(P->arena, &n->args, expression(P, 0));
                } while (accept(P, ","));
                expect(P, ")");
            }
            e = n;
        } else if (accept(P, "[")) {
            Node *n = tnode(P, N_INDEX, "", t);
            nl_push(P->arena, &n->args, e);
            nl_push(P->arena, &n->args, expression(P, 0));
            expect(P, "]");
            e = n;
        } else
            break;
    }
    return e;
}

static Node *operators(Parser *P, Node *e, int level);
/* `items which condition` keeps the items for which the condition holds. It binds looser than
 * every other operator and chains left to right: `xs which > 1 which < 9`. It compiles to the
 * hidden builtin method `$which` called with a one-parameter lambda. */
static Node *which(Parser *P, Node *seq) {
    Token *t = take(P);
    Token *at = peek(P, 0);
    if (at->kind == T_NL || at->kind == T_EOF || tis(at, "which"))
        pfail_at(P, at->line, at->col,
                 "'which' expects a condition, such as 'which .age >= 18' or 'which > 10'%s", "");
    FuncDecl *f = arena_alloc(P->arena, sizeof(FuncDecl));
    f->line = t->line;
    f->name = "lambda";
    f->params = arena_alloc(P->arena, sizeof(Param));
    memset(f->params, 0, sizeof(Param));
    f->params[0].line = t->line;
    f->params[0].name = "$which";
    f->nparams = 1;
    Node *r = tnode(P, S_RETURN, NULL, at);
    P->which_depth++;
    r->expr = operators(P, prefix(P), 0);
    P->which_depth--;
    nl_push(P->arena, &f->body, r);
    Node *fn = tnode(P, N_LAMBDA, "", t);
    fn->fn = f;
    Node *m = tnode(P, N_MEMBER, "$which", t);
    nl_push(P->arena, &m->args, seq);
    Node *call = tnode(P, N_CALL, "", t);
    nl_push(P->arena, &call->args, m);
    nl_push(P->arena, &call->args, fn);
    return call;
}
static Node *expression(Parser *P, int level) {
    Node *e = operators(P, prefix(P), level);
    while (level == 0 && peek(P, 0)->kind == T_ID && tis(peek(P, 0), "which"))
        e = which(P, e);
    return e;
}
static Node *operators(Parser *P, Node *e, int level) {
    for (;;) {
        Token *op = peek(P, 0);
        if (!((op->kind == T_OP || op->kind == T_ID) && precedence(op) > level))
            break;
        if (tis(op, "is") || tis(op, "as")) {
            take(P);
            Node *n = tnode(P, tis(op, "is") ? N_IS : N_AS, parse_type(P), op);
            nl_push(P->arena, &n->args, e);
            e = n;
            continue;
        }
        take(P);
        const char *ops = tstr(P, op);
        int prec = precedence(op);
        if (comparison(ops) && !e->grouped && (e->kind == N_BINARY || e->kind == N_CHAIN)) {
            const char *last = e->kind == N_CHAIN ? e->names[e->nnames - 1] : e->text;
            if (comparison(last) && prec_of(last) == prec) {
                if (e->kind == N_BINARY) {
                    e->kind = N_CHAIN;
                    e->names = arena_alloc(P->arena, sizeof(char *) * 64);
                    e->names[0] = e->text;
                    e->nnames = 1;
                }
                if (e->nnames >= 63)
                    pfail_at(P, op->line, op->col, "comparison chain is too long%s", "");
                e->names[e->nnames++] = ops;
                nl_push(P->arena, &e->args, expression(P, prec));
                continue;
            }
        }
        Node *n = tnode(P, N_BINARY, ops, op);
        nl_push(P->arena, &n->args, e);
        nl_push(P->arena, &n->args, expression(P, prec));
        e = n;
    }
    return e;
}

static Node *parse_sub_expression(Parser *P, const char *text, size_t n, int line, int col,
                                  const char *what) {
    TokenList toks;
    CompileError suberr;
    memset(&suberr, 0, sizeof suberr);
    if (!lex_source(P->arena, text, n, &toks, &suberr, line - 1))
        pfail_at(P, suberr.line, col, "%s", suberr.message);
    Parser sub;
    memset(&sub, 0, sizeof sub);
    sub.arena = P->arena;
    sub.tokens = toks.items;
    sub.ntokens = toks.count;
    sub.jmp = P->jmp;
    sub.err = P->err;
    Node *v = expression(&sub, 0);
    if (peek(&sub, 0)->kind != T_EOF)
        pfail_at(P, line, col, "invalid %s expression", what);
    (void)col;
    return v;
}

typedef struct {
    const char **names;
    int count;
} NameSet;
static NameSet callables;

static bool is_callable_name(Parser *P, const Token *t) {
    (void)P;
    if (tis(t, "print"))
        return true;
    for (int i = 0; i < callables.count; i++)
        if (tis(t, callables.names[i]))
            return true;
    return false;
}

static bool attached_sign(Parser *P, int n) {
    Token *t = peek(P, n);
    return (tis(t, "-") || tis(t, "+")) && t->kind == T_OP && t->spaced && !peek(P, n + 1)->spaced;
}
/* The callee's width in tokens: a name, or a dotted path written without spaces (time.sleep). */
static int callee_width(Parser *P) {
    int width = 1;
    if (peek(P, 0)->kind == T_ID)
        while (tis(peek(P, width), ".") && !peek(P, width)->spaced &&
               peek(P, width + 1)->kind == T_ID && !peek(P, width + 1)->spaced)
            width += 2;
    return width;
}
/* `print (1+2)*3` passes `(1+2)*3`: a spaced '(' group followed by a binary operator, or by `.` or
 * `[` written against it, opens the argument (`time.sleep (delay in s).value`). */
static bool grouped_operand(Parser *P, int width) {
    Token *t1 = peek(P, width);
    if (!tis(t1, "(") || !t1->spaced || t1->kind != T_OP)
        return false;
    int depth = 0;
    for (int n = width;; ++n) {
        Token *t = peek(P, n);
        if (t->kind == T_EOF)
            return false;
        if (literal_kind(t))
            continue;
        if (t->kind == T_OP && (tis(t, "(") || tis(t, "[") || tis(t, "{")))
            ++depth;
        else if (t->kind == T_OP && (tis(t, ")") || tis(t, "]") || tis(t, "}")) && --depth == 0) {
            Token *next = peek(P, n + 1);
            if ((tis(next, ".") || tis(next, "[")) && next->kind == T_OP && !next->spaced)
                return true;
            return (next->kind == T_OP || next->kind == T_ID) && precedence(next) > 0;
        }
    }
}
static bool argument_start(const Token *t) {
    return t->kind == T_ID || t->kind == T_INT || t->kind == T_REAL || t->kind == T_STRING ||
           t->kind == T_CHAR || t->kind == T_MARKUP || t->kind == T_JSON;
}
static Node *outer(Parser *P) {
    Token *first = peek(P, 0);
    bool callable = is_callable_name(P, first);
    Node *e;
    int width = callee_width(P);
    bool grouped = (callable || width > 1) && grouped_operand(P, width);
    if (grouped && width > 1) {
        Token *t = take(P);
        e = tnode(P, N_NAME, tstr(P, t), t);
        for (; width > 1; width -= 2) {
            take(P);
            Token *m = take(P);
            Node *n = tnode(P, N_MEMBER, tstr(P, m), m);
            nl_push(P->arena, &n->args, e);
            e = n;
        }
    } else if (callable && (attached_sign(P, 1) || ptext(P, 1, "not") || grouped ||
                     (ptext(P, 1, "[") && peek(P, 1)->spaced))) {
        Token *t = take(P);
        e = tnode(P, N_NAME, tstr(P, t), t);
    } else
        e = expression(P, 0);
    Token *k = peek(P, 0);
    if ((e->kind == N_NAME || e->kind == N_MEMBER) && !e->grouped &&
        (argument_start(k) || (ptext(P, 0, "[") && k->kind == T_OP && k->spaced) || grouped ||
         (callable && attached_sign(P, 0)))) {
        /* a word operator such as `and` is not an argument */
        if (!(k->kind == T_ID && precedence(k) > 0)) {
            Node *n = node(P, N_CALL, "", e->line, e->col);
            nl_push(P->arena, &n->args, e);
            do {
                nl_push(P->arena, &n->args, expression(P, 0));
            } while (accept(P, ","));
            Node *last = n->args.items[n->args.count - 1];
            Token *nx = peek(P, 0);
            if ((last->kind == N_NAME || last->kind == N_MEMBER) && argument_start(nx) &&
                !(nx->kind == T_ID && (precedence(nx) > 0 || tis(nx, "do") ||
                                       tis(nx, "else"))))
                pfail_at(P, nx->line, nx->col, "nested calls require parentheses around their arguments%s", "");
            e = n;
        }
    }
    e->outer = true;
    return e;
}

/* ---------------------------------------------------------------- statements */

static Node *statement(Parser *P);
static FuncDecl *parse_function(Parser *P, const char *owner, bool signature_only);

static bool block_end(Parser *P) {
    Token *t = peek(P, 0);
    if (t->kind == T_EOF)
        return true;
    if (literal_kind(t))
        return false;
    return tis(t, "end") || tis(t, "else") || tis(t, "catch") || tis(t, "finally") ||
           tis(t, "case");
}
static void push_statement(Parser *P, NodeList *out, Node *s) {
    nl_push(P->arena, out, s);
    for (int i = 0; i < P->pending.count; i++)
        nl_push(P->arena, out, P->pending.items[i]);
    P->pending.count = 0;
}
static void block(Parser *P, NodeList *out) {
    newlines(P);
    while (!block_end(P))
        push_statement(P, out, statement(P));
    /* defer: the rest of the block becomes a try whose finally is the deferred statement. */
    for (int i = out->count - 1; i >= 0; --i) {
        if (out->items[i]->kind != S_DEFER)
            continue;
        Node *d = out->items[i];
        Node *t = node(P, S_TRY, NULL, d->line, d->col);
        t->finalizer = d->body;
        t->has_else = true;
        for (int j = i + 1; j < out->count; ++j)
            nl_push(P->arena, &t->body, out->items[j]);
        out->count = i;
        nl_push(P->arena, out, t);
    }
}

typedef struct {
    const char *type, *name;
} TypeBinding;
static void spec_catch_type(Parser *P, void *ctx) {
    TypeBinding *b = ctx;
    b->type = parse_type(P);
    b->name = tstr(P, identifier(P));
}
static void spec_pattern_type(Parser *P, void *ctx) {
    TypeBinding *b = ctx;
    b->type = parse_type(P);
    b->name = tstr(P, identifier(P));
    if (!ptext(P, 0, "do") && !ptext(P, 0, "if"))
        pfail(P, "pattern%s", "");
}
typedef struct {
    Node *cond, *msg;
} AssertParts;
static void spec_assert_call(Parser *P, void *ctx) {
    AssertParts *a = ctx;
    take(P);
    a->cond = expression(P, 0);
    if (accept(P, ","))
        a->msg = expression(P, 0);
    expect(P, ")");
    if (peek(P, 0)->kind != T_NL && peek(P, 0)->kind != T_EOF)
        pfail(P, "not a call%s", "");
}

typedef struct {
    Node *cond;
} WhenParts;
static void spec_when(Parser *P, void *ctx) {
    ((WhenParts *)ctx)->cond = expression(P, 0);
    if (!ptext(P, 0, "do") && !(ptext(P, 0, "run") && peek(P, 0)->kind == T_ID))
        pfail(P, "expected do%s", "");
}
/* `run` (a soft keyword) introduces a one-call body: `if/while/for/when ... run call`. */
static bool runs(Parser *P) {
    if (!ptext(P, 0, "run") || peek(P, 0)->kind != T_ID)
        return false;
    take(P);
    return true;
}
#define RUN_USAGE "'run' expects a function call, such as 'run greet()'"
/* The call after `run`, with no `end`. It cannot be a keyword statement, a declaration, or an
   assignment. */
static void run_body(Parser *P, NodeList *out) {
    Token *t = peek(P, 0);
    if (t->kind == T_NL || t->kind == T_EOF)
        pfail(P, RUN_USAGE "%s", "");
    static const char *keywords[] = {
        "import", "var",   "const",    "class", "function", "component", "return",   "if",
        "else",  "while",    "do",    "for",      "end",       "break",    "continue",
        "try",    "catch", "finally",  "throw", "match",    "case",      "defer",    "when",
        "reactive", "computed", "assert", "enum", "interface", NULL};
    if (t->kind == T_ID && in_list(t, keywords) && !(tis(t, "function") && ptext(P, 1, "(")))
        pfail(P, RUN_USAGE ", not '%s'", tstr(P, t));
    Node *s = tnode(P, S_EXPR, NULL, t);
    s->target = outer(P);
    static const char *assign_ops[] = {"=", "+=", "-=", "*=", "/=", "//=", "%=", NULL};
    Token *nx = peek(P, 0);
    if (nx->kind == T_OP && in_list(nx, assign_ops))
        pfail(P, RUN_USAGE "; assignments need 'do' ... 'end'%s", "");
    /* A bare name or member calls it with no arguments, as a statement does: `run main_loop`. */
    Node *e = s->target;
    if (e->kind != N_CALL && !((e->kind == N_NAME || e->kind == N_MEMBER) && !e->grouped))
        pfail_at(P, t->line, t->col, RUN_USAGE "%s", "");
    if (e->kind != N_CALL) {
        Node *call = node(P, N_CALL, "", e->line, e->col);
        nl_push(P->arena, &call->args, e);
        call->outer = true;
        s->target = call;
    }
    nl_push(P->arena, out, s);
}
static void spec_run_condition(Parser *P, void *ctx) {
    (void)ctx;
    expression(P, 0);
    if (!ptext(P, 0, "run") || peek(P, 0)->kind != T_ID)
        pfail(P, "expected run%s", "");
}
/* After `if condition run call`, the chain continues with `else run call` or
   `else if condition run call`, on the same line or the next. Any other `else` belongs to an
   enclosing `if ... do`. */
static void run_else(Parser *P, Node *s) {
    int saved = P->pos;
    newlines(P);
    if (ptext(P, 0, "else") && peek(P, 0)->kind == T_ID) {
        int else_pos = P->pos;
        take(P);
        if (runs(P)) {
            run_body(P, &s->other);
            finish(P);
            return;
        }
        if (ptext(P, 0, "if") && peek(P, 0)->kind == T_ID) {
            int if_pos = P->pos;
            take(P);
            bool chained = speculate(P, spec_run_condition, NULL);
            P->pos = if_pos;
            if (chained) {
                nl_push(P->arena, &s->other, statement(P));
                return;
            }
        }
        P->pos = else_pos;
    }
    P->pos = saved;
    finish(P);
}
/* `bool name/negated = ...` */
static bool alias_declaration(Parser *P) {
    if (peek(P, 0)->kind != T_ID || peek(P, 1)->kind != T_ID || !ptext(P, 2, "/") ||
        peek(P, 2)->kind != T_OP || peek(P, 3)->kind != T_ID || !ptext(P, 4, "="))
        return false;
    if (ptext(P, 0, "var") || ptext(P, 0, "const"))
        pfail(P, "negated aliases need the bool type: bool %s/... = ...", tstr(P, peek(P, 1)));
    if (!ptext(P, 0, "bool"))
        pfail(P, "negated aliases (name/negated) are only for bool, such as bool enabled/disabled = true%s", "");
    return true;
}
static Node *statement(Parser *P) {
    Token *st = peek(P, 0);
    Node *s = tnode(P, S_EXPR, NULL, st);
    if (accept(P, "throw")) {
        /* throw value, throw Type with "message", throw "Name" with "message" */
        s->kind = S_THROW;
        s->expr = expression(P, 0);
        if (ptext(P, 0, "with") && peek(P, 0)->kind == T_ID) {
            take(P);
            s->op = "with";
            if (peek(P, 0)->kind == T_NL || peek(P, 0)->kind == T_EOF)
                pfail(P, "'with' expects a message, such as throw BadInput with \"the input is bad\"%s", "");
            s->target = expression(P, 0);
        } else if (peek(P, 0)->kind != T_NL && peek(P, 0)->kind != T_EOF)
            pfail(P, "expected 'with' before the message, such as throw \"Name\" with \"message\"%s", "");
        finish(P);
        return s;
    }
    if (ptext(P, 0, "assert") && peek(P, 0)->kind == T_ID) {
        static const char *not_keyword[] = {"=", "+=", "-=", "*=", "/=", "//=", "%=", ".", "[", ")", ",", NULL};
        Token *nx = peek(P, 1);
        if (!(nx->kind == T_OP && in_list(nx, not_keyword))) {
            Token *at = take(P);
            AssertParts a = {NULL, NULL};
            if (ptext(P, 0, "(") && peek(P, 0)->kind == T_OP) {
                int saved = P->pos;
                if (!speculate(P, spec_assert_call, &a)) {
                    P->pos = saved;
                    a.cond = a.msg = NULL;
                }
            }
            if (!a.cond) {
                a.cond = expression(P, 0);
                if (peek(P, 0)->kind != T_NL && peek(P, 0)->kind != T_EOF) {
                    accept(P, ",");
                    a.msg = expression(P, 0);
                }
            }
            finish(P);
            if (!a.msg)
                a.msg = tnode(P, N_STRING, "assertion failed", at);
            Node *neg = tnode(P, N_UNARY, "not", at);
            a.cond->grouped = true;
            nl_push(P->arena, &neg->args, a.cond);
            Node *raise = tnode(P, S_THROW, NULL, at);
            raise->expr = tnode(P, N_CALL, "", at);
            nl_push(P->arena, &raise->expr->args, tnode(P, N_NAME, "AssertionError", at));
            nl_push(P->arena, &raise->expr->args, a.msg);
            s->kind = S_IF;
            s->expr = neg;
            nl_push(P->arena, &s->body, raise);
            return s;
        }
    }
    if (accept(P, "try")) {
        s->kind = S_TRY;
        finish(P);
        block(P, &s->body);
        bool catch_all = false;
        int cap = 0;
        while (accept(P, "catch")) {
            if (catch_all)
                pfail(P, "catch-all must be the last catch clause%s", "");
            const char *t = NULL, *name = NULL;
            if (peek(P, 0)->kind == T_STRING) {
                Token *q = take(P);
                if (!q->len)
                    pfail_at(P, q->line, q->col, "catch expects a plain error name such as \"ValueError\"%s", "");
                for (int i = 0; i < q->len; i++)
                    if (strchr("{}\\\"", q->text[i]))
                        pfail_at(P, q->line, q->col, "catch expects a plain error name such as \"ValueError\"%s", "");
                char *quoted = arena_alloc(P->arena, (size_t)q->len + 3);
                snprintf(quoted, (size_t)q->len + 3, "\"%.*s\"", q->len, q->text);
                t = quoted;
                name = peek(P, 0)->kind == T_ID ? tstr(P, identifier(P)) : "$caught";
                if (tis(q, "Error"))
                    catch_all = true;
            } else {
                TypeBinding b = {NULL, NULL};
                int saved = P->pos;
                if (speculate(P, spec_catch_type, &b)) {
                    t = b.type;
                    name = b.name;
                } else {
                    P->pos = saved;
                    t = "Error";
                    name = tstr(P, identifier(P));
                }
                if (!strcmp(t, "Error"))
                    catch_all = true;
            }
            GROWV(P->arena, s->catch_types, s->ncatches, cap, const char *);
            /* parallel arrays share capacity */
            const char **names = arena_alloc(P->arena, sizeof(char *) * (size_t)cap);
            NodeList *bodies = arena_alloc(P->arena, sizeof(NodeList) * (size_t)cap);
            if (s->ncatches) {
                memcpy(names, s->catch_names, sizeof(char *) * (size_t)s->ncatches);
                memcpy(bodies, s->catches, sizeof(NodeList) * (size_t)s->ncatches);
            }
            s->catch_names = names;
            s->catches = bodies;
            s->catch_types[s->ncatches] = t;
            s->catch_names[s->ncatches] = name;
            memset(&s->catches[s->ncatches], 0, sizeof(NodeList));
            finish(P);
            block(P, &s->catches[s->ncatches]);
            s->ncatches++;
        }
        if (accept(P, "finally")) {
            finish(P);
            block(P, &s->finalizer);
            s->has_else = true; /* marks an explicit finalizer */
        }
        if (s->ncatches == 0 && !s->has_else)
            pfail_at(P, s->line, s->col, "try requires catch or finally%s", "");
        expect(P, "end");
        finish(P);
        return s;
    }
    if (accept(P, "if")) {
        s->kind = S_IF;
        s->expr = expression(P, 0);
        if (runs(P)) {
            run_body(P, &s->body);
            run_else(P, s);
            return s;
        }
        expect(P, "do");
        finish(P);
        block(P, &s->body);
        if (accept(P, "else")) {
            if (ptext(P, 0, "if")) {
                nl_push(P->arena, &s->other, statement(P));
                return s;
            }
            if (runs(P)) {
                run_body(P, &s->other);
                finish(P);
                return s;
            }
            expect(P, "do");
            finish(P);
            block(P, &s->other);
        }
        expect(P, "end");
        finish(P);
        return s;
    }
    if (accept(P, "match")) {
        s->kind = S_MATCH;
        s->expr = expression(P, 0);
        finish(P);
        int cap = 0;
        while (accept(P, "case")) {
            TypeBinding b = {NULL, NULL};
            int saved = P->pos;
            if (!speculate(P, spec_pattern_type, &b)) {
                P->pos = saved;
                b.type = b.name = NULL;
            }
            if (s->ncatches == cap) {
                int nc = GROW_CAP(cap);
                const char **types = arena_alloc(P->arena, sizeof(char *) * (size_t)nc);
                const char **names = arena_alloc(P->arena, sizeof(char *) * (size_t)nc);
                NodeList *pats = arena_alloc(P->arena, sizeof(NodeList) * (size_t)nc);
                NodeList *bodies = arena_alloc(P->arena, sizeof(NodeList) * (size_t)nc);
                Node **guards = arena_alloc(P->arena, sizeof(Node *) * (size_t)nc);
                if (cap) {
                    memcpy(types, s->catch_types, sizeof(char *) * (size_t)cap);
                    memcpy(names, s->catch_names, sizeof(char *) * (size_t)cap);
                    memcpy(pats, s->patterns, sizeof(NodeList) * (size_t)cap);
                    memcpy(bodies, s->catches, sizeof(NodeList) * (size_t)cap);
                    memcpy(guards, s->guards, sizeof(Node *) * (size_t)cap);
                }
                s->catch_types = types;
                s->catch_names = names;
                s->patterns = pats;
                s->catches = bodies;
                s->guards = guards;
                cap = nc;
            }
            int i = s->ncatches++;
            s->catch_types[i] = b.type;
            s->catch_names[i] = b.name;
            memset(&s->patterns[i], 0, sizeof(NodeList));
            memset(&s->catches[i], 0, sizeof(NodeList));
            if (!b.type)
                do {
                    nl_push(P->arena, &s->patterns[i], expression(P, 0));
                } while (accept(P, ","));
            s->guards[i] = accept(P, "if") ? expression(P, 0) : NULL;
            expect(P, "do");
            finish(P);
            block(P, &s->catches[i]);
        }
        if (accept(P, "else")) {
            s->has_else = true;
            expect(P, "do");
            finish(P);
            block(P, &s->other);
        }
        if (s->ncatches == 0 && !s->has_else)
            pfail_at(P, s->line, s->col, "match requires at least one case%s", "");
        expect(P, "end");
        finish(P);
        return s;
    }
    if (accept(P, "while")) {
        s->kind = S_WHILE;
        s->expr = expression(P, 0);
        if (runs(P)) {
            run_body(P, &s->body);
            finish(P);
            return s;
        }
        expect(P, "do");
        finish(P);
        block(P, &s->body);
        expect(P, "end");
        finish(P);
        return s;
    }
    if (accept(P, "for")) {
        s->kind = S_FOR;
        if (ptext(P, 0, "("))
            pfail(P, "use 'for item in items do'; parentheses are not allowed around the binding%s", "");
        s->name = tstr(P, identifier(P));
        if (accept(P, ","))
            s->op = tstr(P, identifier(P));
        expect(P, "in");
        s->expr = expression(P, 0);
        if (runs(P)) {
            run_body(P, &s->body);
            finish(P);
            return s;
        }
        expect(P, "do");
        finish(P);
        block(P, &s->body);
        expect(P, "end");
        finish(P);
        return s;
    }
    if (accept(P, "defer")) {
        s->kind = S_DEFER;
        if (peek(P, 0)->kind == T_NL) {
            finish(P);
            block(P, &s->body);
            expect(P, "end");
            finish(P);
        } else {
            push_statement(P, &s->body, statement(P));
        }
        if (s->body.count == 0)
            pfail_at(P, s->line, s->col, "defer requires a statement%s", "");
        return s;
    }
    if (accept(P, "return")) {
        s->kind = S_RETURN;
        if (peek(P, 0)->kind != T_NL && peek(P, 0)->kind != T_EOF)
            s->expr = expression(P, 0);
        finish(P);
        return s;
    }
    if (ptext(P, 0, "break") || ptext(P, 0, "continue")) {
        Token *t = take(P);
        s->kind = tis(t, "break") ? S_BREAK : S_CONTINUE;
        finish(P);
        return s;
    }
    /* `when condition do ... end` (lowered by lower.c) */
    if (ptext(P, 0, "when") && peek(P, 0)->kind == T_ID) {
        int saved = P->pos;
        Token *at = take(P);
        WhenParts w = {NULL};
        if (peek(P, 0)->kind != T_NL && !ptext(P, 0, "do") && speculate(P, spec_when, &w)) {
            s->kind = S_WHEN;
            s->line = at->line;
            s->col = at->col;
            s->expr = w.cond;
            if (runs(P)) {
                run_body(P, &s->body);
                finish(P);
                return s;
            }
            take(P); /* do */
            finish(P);
            block(P, &s->body);
            expect(P, "end");
            finish(P);
            return s;
        }
        P->pos = saved;
    }
    /* `reactive` and `computed` declarations */
    const char *declaration_kind = NULL;
    if ((ptext(P, 0, "reactive") || ptext(P, 0, "computed")) && peek(P, 0)->kind == T_ID) {
        int saved = P->pos;
        Token *kw = take(P);
        if (ptext(P, 0, "const"))
            pfail(P, "'%s' declarations cannot be const", tstr(P, kw));
        if (ptext(P, 0, "var") || alias_declaration(P) || typed_declaration(P))
            declaration_kind = tstr(P, kw);
        else
            P->pos = saved;
    }
    if (alias_declaration(P)) {
        /* bool enabled/disabled = true: `disabled` reads and writes the negation of `enabled` */
        s->kind = S_DECLARE;
        s->op = declaration_kind ? declaration_kind : "var";
        s->type = parse_type(P);
        s->name = tstr(P, identifier(P));
        expect(P, "/");
        Token *neg = identifier(P);
        s->alias = tstr(P, neg);
        if (!strcmp(s->alias, s->name))
            pfail_at(P, neg->line, neg->col, "a negated alias needs a different name%s", "");
        expect(P, "=");
        s->expr = outer(P);
        finish(P);
        return s;
    }
    bool isconst = accept(P, "const"), inferred = accept(P, "var");
    if (declaration_kind && (ptext(P, 0, "(") || ptext(P, 0, "[")))
        pfail(P, "'%s' declares one variable at a time", declaration_kind);
    if ((isconst || inferred) && (ptext(P, 0, "(") || ptext(P, 0, "["))) {
        const char *close = tis(take(P), "(") ? ")" : "]";
        Token *names[256];
        int count = 0;
        do {
            if (count == 256)
                pfail(P, "too many names%s", "");
            names[count++] = ptext(P, 0, "_") ? take(P) : identifier(P);
        } while (accept(P, ","));
        expect(P, close);
        expect(P, "=");
        char *tmp = arena_alloc(P->arena, 32);
        snprintf(tmp, 32, "$destructured%d", ++P->temporaries);
        s->kind = S_DECLARE;
        s->op = "const";
        s->name = tmp;
        s->expr = outer(P);
        finish(P);
        Node *check = node(P, S_DESTRUCTURE, NULL, s->line, s->col);
        check->name = tmp;
        char *cnt = arena_alloc(P->arena, 16);
        snprintf(cnt, 16, "%d", count);
        check->op = cnt;
        nl_push(P->arena, &P->pending, check);
        for (int i = 0; i < count; i++) {
            if (tis(names[i], "_"))
                continue;
            Node *d = tnode(P, S_DECLARE, NULL, names[i]);
            d->op = isconst ? "const" : "var";
            d->name = tstr(P, names[i]);
            Node *ix = tnode(P, N_INDEX, "", names[i]);
            nl_push(P->arena, &ix->args, tnode(P, N_NAME, tmp, names[i]));
            char *num = arena_alloc(P->arena, 16);
            snprintf(num, 16, "%d", i);
            nl_push(P->arena, &ix->args, tnode(P, N_INT, num, names[i]));
            d->expr = ix;
            nl_push(P->arena, &P->pending, d);
        }
        return s;
    }
    if (isconst || inferred || typed_declaration(P)) {
        s->kind = S_DECLARE;
        s->op = isconst ? "const" : declaration_kind ? declaration_kind : "var";
        if (!inferred && !ptext(P, 1, "="))
            s->type = parse_type(P);
        s->name = tstr(P, identifier(P));
        expect(P, "=");
        s->expr = outer(P);
        finish(P);
        return s;
    }
    s->target = outer(P);
    static const char *assign_ops[] = {"=", "+=", "-=", "*=", "/=", "//=", "%=", NULL};
    Token *nx = peek(P, 0);
    if (nx->kind == T_OP && in_list(nx, assign_ops)) {
        s->kind = S_ASSIGN;
        s->op = tstr(P, take(P));
        s->expr = outer(P);
    } else
        s->kind = S_EXPR;
    finish(P);
    return s;
}

static Node *lambda(Parser *P) {
    Token *t = expect(P, "function");
    FuncDecl *f = arena_alloc(P->arena, sizeof(FuncDecl));
    f->line = t->line;
    f->name = "lambda";
    expect(P, "(");
    int cap = 0;
    if (!accept(P, ")")) {
        do {
            GROWV(P->arena, f->params, f->nparams, cap, Param);
            Param *p = &f->params[f->nparams++];
            memset(p, 0, sizeof *p);
            p->line = peek(P, 0)->line;
            p->type = parse_type(P);
            p->name = tstr(P, identifier(P));
            if (ptext(P, 0, "="))
                pfail(P, "lambda parameters cannot have defaults%s", "");
        } while (accept(P, ","));
        expect(P, ")");
    }
    if (accept(P, "returns"))
        f->result = parse_type(P);
    if (accept(P, "=>")) {
        Token *at = peek(P, 0);
        Node *r = tnode(P, S_RETURN, NULL, at);
        r->expr = expression(P, 0);
        nl_push(P->arena, &f->body, r);
    } else {
        if (P->nbody == P->capbody) {
            P->capbody = GROW_CAP(P->capbody);
            int *nd = arena_alloc(P->arena, sizeof(int) * (size_t)P->capbody);
            if (P->nbody)
                memcpy(nd, P->body_depths, sizeof(int) * (size_t)P->nbody);
            P->body_depths = nd;
        }
        P->body_depths[P->nbody++] = t->depth;
        finish(P);
        block(P, &f->body);
        expect(P, "end");
        P->nbody--;
    }
    Node *e = tnode(P, N_LAMBDA, "", t);
    e->fn = f;
    return e;
}

static FuncDecl *parse_function(Parser *P, const char *owner, bool signature_only) {
    Token *t = expect(P, "function");
    FuncDecl *f = arena_alloc(P->arena, sizeof(FuncDecl));
    f->line = t->line;
    f->owner = owner;
    if (owner && ptext(P, 0, "operator") && peek(P, 1)->kind != T_NL && !ptext(P, 1, "(")) {
        take(P);
        Token *op = take(P);
        static const char *overloadable[] = {"+", "-", "*", "/", "//", "%", "==", "<", "<=", ">", ">=", "[", NULL};
        if (!in_list(op, overloadable)) {
            char nm[32];
            snprintf(nm, sizeof nm, "%.*s", op->len > 20 ? 20 : op->len, op->text);
            pfail_at(P, op->line, op->col, "operator '%s' cannot be overloaded", nm);
        }
        char name[32];
        snprintf(name, sizeof name, "operator%.*s", op->len, op->text);
        if (tis(op, "[")) {
            expect(P, "]");
            strcat(name, "]");
            if (ptext(P, 0, "=") && !peek(P, 0)->spaced) {
                take(P);
                strcat(name, "=");
            }
        }
        f->name = arena_strndup(P->arena, name, strlen(name));
    } else
        f->name = tstr(P, identifier(P));
    skip_type_params(P);
    int cap = 0;
    if (accept(P, "(")) {
        if (!accept(P, ")")) {
            do {
                GROWV(P->arena, f->params, f->nparams, cap, Param);
                Param *p = &f->params[f->nparams++];
                memset(p, 0, sizeof *p);
                p->line = peek(P, 0)->line;
                p->type = parse_type(P);
                p->name = tstr(P, identifier(P));
                if (accept(P, "="))
                    p->value = expression(P, 0);
            } while (accept(P, ","));
            expect(P, ")");
        }
    } else if (!ptext(P, 0, "returns") && peek(P, 0)->kind != T_NL && peek(P, 0)->kind != T_EOF) {
        pfail(P, "function declarations with parameters require parentheses%s", "");
    }
    if (!strcmp(f->name, "operator-") && f->nparams == 0)
        f->name = "operator-()";
    if (!strncmp(f->name, "operator", 8) && strcmp(f->name, "operator-()") != 0) {
        int arity = !strcmp(f->name, "operator[]=") ? 2 : 1;
        if (f->nparams != arity)
            pfail_at(P, f->line, 1, "%s takes the wrong number of parameters", f->name);
    }
    if (accept(P, "returns"))
        f->result = parse_type(P);
    finish(P);
    if (signature_only) {
        f->is_abstract = true;
        return f;
    }
    block(P, &f->body);
    expect(P, "end");
    finish(P);
    return f;
}

/* ---------------------------------------------------------------- program */

static bool ident_ok(const char *s) {
    size_t n = strlen(s);
    if (!n || isdigit((unsigned char)s[0]))
        return false;
    return lx_identifier_end(s, n, 0) == n;
}

static bool parse_program_with(Arena *arena, const char *src, size_t len, Program *out, CompileError *err,
                               bool defer_units);
bool parse_program(Arena *arena, const char *src, size_t len, Program *out, CompileError *err) {
    return parse_program_with(arena, src, len, out, err, false);
}
bool parse_program_deferred(Arena *arena, const char *src, size_t len, Program *out, CompileError *err) {
    return parse_program_with(arena, src, len, out, err, true);
}
bool lower_program_units(Arena *arena, Program *prog, const UnitNs *imported, int nimported, CompileError *err) {
    Parser P;
    memset(&P, 0, sizeof P);
    P.arena = arena;
    P.err = err;
    jmp_buf jb;
    P.jmp = &jb;
    if (setjmp(jb))
        return false;
    lower_units(&P, prog, imported, nimported, true);
    return true;
}
static bool parse_program_with(Arena *arena, const char *src, size_t len, Program *out, CompileError *err,
                               bool defer_units) {
    memset(out, 0, sizeof *out);
    TokenList toks;
    if (!lex_source(arena, src, len, &toks, err, 0))
        return false;
    Parser P;
    memset(&P, 0, sizeof P);
    P.arena = arena;
    P.tokens = toks.items;
    P.ntokens = toks.count;
    P.err = err;
    jmp_buf jb;
    P.jmp = &jb;
    /* Names declared by `function NAME` / `class NAME` start parentheses-free calls. */
    NameSet saved_callables = callables;
    callables.names = arena_alloc(arena, sizeof(char *) * (size_t)(toks.count + 1));
    callables.count = 0;
    for (int i = 0; i + 1 < toks.count; i++)
        if (toks.items[i].kind == T_ID && (tis(&toks.items[i], "function") || tis(&toks.items[i], "class")) &&
            toks.items[i + 1].kind == T_ID)
            callables.names[callables.count++] = tstr(&P, &toks.items[i + 1]);
    if (setjmp(jb)) {
        callables = saved_callables;
        return false;
    }
    int cap_imp = 0, cap_fn = 0, cap_cls = 0, cap_en = 0, cap_comp = 0, cap_priv = 0, cap_fam = 0;
    newlines(&P);
    while (peek(&P, 0)->kind != T_EOF) {
        bool hidden = accept(&P, "private");
        bool visible = !hidden && accept(&P, "public");
        int nf = out->nfunctions, nc = out->nclasses, nb = out->body.count, ne = out->nenums,
            ncomp = out->ncomponents;
        if ((hidden || visible) && ptext(&P, 0, "import"))
            pfail(&P, "visibility applies to declarations, not imports%s", "");
        if (accept(&P, "import")) {
            ImportDecl im;
            memset(&im, 0, sizeof im);
            im.line = peek(&P, 0)->line;
            if (ptext(&P, 0, "+") && peek(&P, 0)->kind == T_OP && !peek(&P, 0)->spaced &&
                ptext(&P, 1, "+") && !peek(&P, 1)->spaced) {
                take(&P);
                take(&P);
                im.kind = "cxx";
            } else if ((ptext(&P, 0, "c") || ptext(&P, 0, "cxx") || ptext(&P, 0, "rust") || ptext(&P, 0, "zig")) &&
                       peek(&P, 0)->kind == T_ID &&
                       (peek(&P, 1)->kind == T_ID || peek(&P, 1)->kind == T_STRING) && !ptext(&P, 1, "as")) {
                im.kind = tstr(&P, take(&P));
            }
            SBuf name = {0};
            Token *name_tok = peek(&P, 0);
            if (name_tok->kind == T_STRING) {
                take(&P);
                sb_add(&name, name_tok->text, (size_t)name_tok->len);
                if (!name.n)
                    pfail_at(&P, name_tok->line, name_tok->col, "import needs a module name%s", "");
                if (name.n > 2 && !strcmp(name.d + name.n - 2, ".*")) {
                    im.wildcard = true;
                    name.n -= 2;
                    name.d[name.n] = 0;
                }
                bool dotted = name.n > 0;
                for (size_t i = 0; i < name.n && dotted; i++) {
                    unsigned char c = (unsigned char)name.d[i];
                    bool first = i == 0 || name.d[i - 1] == '.';
                    dotted = c >= 0x80 || isalpha(c) || c == '_' ||
                             (!first && (isdigit(c) || c == '.') && !(c == '.' && i + 1 == name.n));
                }
                static const char *exts[] = {".fw", ".fly", ".c", ".h", ".cpp", ".cc", ".cxx", ".hpp",
                                             ".hh", ".hxx", ".rs", ".zig", NULL};
                for (int i = 0; exts[i]; i++) {
                    size_t el = strlen(exts[i]);
                    if (name.n > el && !strcmp(name.d + name.n - el, exts[i]))
                        dotted = false;
                }
                im.path = !dotted;
                if (im.path && im.wildcard)
                    pfail_at(&P, name_tok->line, name_tok->col, "wildcard imports need a module name, not a path%s", "");
            } else {
                Token *id = identifier(&P);
                sb_add(&name, id->text, (size_t)id->len);
                while (accept(&P, ".")) {
                    if (accept(&P, "*")) {
                        im.wildcard = true;
                        break;
                    }
                    Token *part = identifier(&P);
                    sb_addc(&name, '.');
                    sb_add(&name, part->text, (size_t)part->len);
                }
            }
            im.name = arena_strndup(arena, name.d, name.n);
            free(name.d);
            const char *alias;
            if (im.path) {
                const char *base = im.name;
                size_t bl = strcspn(base, "?#");
                while (bl > 0 && (base[bl - 1] == '/' || base[bl - 1] == '\\'))
                    bl--;
                size_t start = 0;
                for (size_t i = 0; i < bl; i++)
                    if (base[i] == '/' || base[i] == '\\')
                        start = i + 1;
                size_t dot = start;
                while (dot < bl && base[dot] != '.')
                    dot++;
                alias = arena_strndup(arena, base + start, dot - start);
            } else {
                const char *dot = strrchr(im.name, '.');
                alias = dot ? dot + 1 : im.name;
            }
            if (accept(&P, "as")) {
                if (im.wildcard)
                    pfail(&P, "wildcard imports cannot have an alias%s", "");
                alias = tstr(&P, identifier(&P));
            } else if (!ident_ok(alias)) {
                pfail_at(&P, name_tok->line, name_tok->col, "cannot name the module imported from \"%s\"; add `as name`", im.name);
            }
            im.alias = alias;
            if (im.wildcard && !strcmp(im.name, "html"))
                out->html = true;
            if (im.wildcard && !strcmp(im.name, "svg"))
                out->svg = true;
            /* `import time.time` names the time unit namespace, which `import time` brings. */
            if (!im.path && !strcmp(im.name, "time.time"))
                im.name = "time";
            bool repeat = false;
            for (int i = 0; i < out->nimports; i++)
                if (!strcmp(out->imports[i].alias, alias) && !im.wildcard && !out->imports[i].wildcard) {
                    /* the same module under the same name again changes nothing */
                    if (!im.path && !out->imports[i].path && !strcmp(out->imports[i].name, im.name))
                        repeat = true;
                    else
                        pfail_at(&P, im.line, 1, "duplicate import '%s'", alias);
                }
            if (!repeat) {
                GROWV(arena, out->imports, out->nimports, cap_imp, ImportDecl);
                out->imports[out->nimports++] = im;
            }
            finish(&P);
        } else if (ptext(&P, 0, "function") && !ptext(&P, 1, "(")) {
            FuncDecl *f = parse_function(&P, NULL, false);
            GROWV(arena, out->functions, out->nfunctions, cap_fn, FuncDecl);
            out->functions[out->nfunctions++] = *f;
        } else if (accept(&P, "enum")) {
            EnumDecl en;
            memset(&en, 0, sizeof en);
            en.line = peek(&P, 0)->line;
            en.name = tstr(&P, identifier(&P));
            finish(&P);
            int capm = 0;
            Param **payloads = NULL;
            int *npay = NULL;
            int capp = 0;
            bool data = false;
            int *lines = NULL;
            while (!ptext(&P, 0, "end") && peek(&P, 0)->kind != T_EOF) {
                Token *member = identifier(&P);
                const char *mname = tstr(&P, member);
                for (int i = 0; i < en.nmembers; i++)
                    if (!strcmp(en.members[i], mname))
                        pfail_at(&P, member->line, member->col, "duplicate enum member '%s'", mname);
                if (en.nmembers == capm) {
                    int ncap = GROW_CAP(capm);
                    const char **nm = arena_alloc(arena, sizeof(char *) * (size_t)ncap);
                    Param **np = arena_alloc(arena, sizeof(Param *) * (size_t)ncap);
                    int *nn = arena_alloc(arena, sizeof(int) * (size_t)ncap);
                    int *nlines = arena_alloc(arena, sizeof(int) * (size_t)ncap);
                    if (capm) {
                        memcpy(nm, en.members, sizeof(char *) * (size_t)capm);
                        memcpy(np, payloads, sizeof(Param *) * (size_t)capm);
                        memcpy(nn, npay, sizeof(int) * (size_t)capm);
                        memcpy(nlines, lines, sizeof(int) * (size_t)capm);
                    }
                    en.members = nm;
                    payloads = np;
                    npay = nn;
                    lines = nlines;
                    capm = ncap;
                }
                int mi = en.nmembers++;
                en.members[mi] = mname;
                payloads[mi] = NULL;
                npay[mi] = 0;
                lines[mi] = member->line;
                capp = 0;
                if (accept(&P, "(")) {
                    data = true;
                    if (!accept(&P, ")")) {
                        do {
                            GROWV(arena, payloads[mi], npay[mi], capp, Param);
                            Param *fp = &payloads[mi][npay[mi]++];
                            memset(fp, 0, sizeof *fp);
                            fp->line = peek(&P, 0)->line;
                            fp->readonly = true;
                            fp->type = parse_type(&P);
                            fp->name = tstr(&P, identifier(&P));
                            if (accept(&P, "="))
                                fp->value = expression(&P, 0);
                        } while (accept(&P, ","));
                        expect(&P, ")");
                    }
                }
                if (!accept(&P, ","))
                    finish(&P);
                newlines(&P);
            }
            if (en.nmembers == 0)
                pfail_at(&P, en.line, 1, "enum requires at least one member%s", "");
            expect(&P, "end");
            finish(&P);
            if (!data) {
                GROWV(arena, out->enums, out->nenums, cap_en, EnumDecl);
                out->enums[out->nenums++] = en;
            } else {
                ClassDecl base;
                memset(&base, 0, sizeof base);
                base.name = en.name;
                base.line = en.line;
                base.variants = arena_alloc(arena, sizeof(char *) * (size_t)en.nmembers);
                for (int i = 0; i < en.nmembers; i++) {
                    ClassDecl v;
                    memset(&v, 0, sizeof v);
                    size_t vl = strlen(en.name) + strlen(en.members[i]) + 2;
                    char *vn = arena_alloc(arena, vl);
                    snprintf(vn, vl, "%s.%s", en.name, en.members[i]);
                    v.name = vn;
                    v.line = lines[i];
                    v.base = en.name;
                    v.variant_of = en.name;
                    v.fields = payloads[i];
                    v.nfields = npay[i];
                    base.variants[base.nvariants++] = vn;
                    GROWV(arena, out->classes, out->nclasses, cap_cls, ClassDecl);
                    out->classes[out->nclasses++] = v;
                }
                GROWV(arena, out->classes, out->nclasses, cap_cls, ClassDecl);
                out->classes[out->nclasses++] = base;
            }
        } else if (accept(&P, "elements")) {
            identifier(&P);
            finish(&P);
            while (!ptext(&P, 0, "end") && peek(&P, 0)->kind != T_EOF) {
                Token *first = take(&P);
                if (first->kind != T_ID)
                    pfail_at(&P, first->line, first->col, "expected a tag or attribute name%s", "");
                SBuf tag = {0};
                sb_add(&tag, first->text, (size_t)first->len);
                while (!peek(&P, 0)->spaced && (ptext(&P, 0, "-") || ptext(&P, 0, ":")) &&
                       !peek(&P, 1)->spaced && peek(&P, 1)->kind == T_ID) {
                    Token *a = take(&P), *b = take(&P);
                    sb_add(&tag, a->text, (size_t)a->len);
                    sb_add(&tag, b->text, (size_t)b->len);
                }
                GROWV(arena, out->families, out->nfamilies, cap_fam, const char *);
                out->families[out->nfamilies++] = arena_strndup(arena, tag.d, tag.n);
                free(tag.d);
                while (peek(&P, 0)->kind != T_NL && peek(&P, 0)->kind != T_EOF)
                    take(&P);
                finish(&P);
            }
            expect(&P, "end");
            finish(&P);
        } else if (ptext(&P, 0, "unitns") && peek(&P, 0)->kind == T_ID && peek(&P, 1)->kind == T_ID &&
                   (peek(&P, 2)->kind == T_NL || peek(&P, 2)->kind == T_EOF || ptext(&P, 2, "for"))) {
            /* unitns time: its units apply to every numeric type */
            take(&P);
            UnitNs ns;
            memset(&ns, 0, sizeof ns);
            ns.line = peek(&P, 0)->line;
            ns.col = peek(&P, 0)->col;
            ns.name = tstr(&P, identifier(&P));
            if (ptext(&P, 0, "for"))
                pfail_at(&P, peek(&P, 0)->line, peek(&P, 0)->col,
                         "unitns takes only a name: write `unitns %s`; units apply to every numeric type", ns.name);
            finish(&P);
            for (int i = 0; i < out->nunitns; i++)
                if (!strcmp(out->unitns[i].name, ns.name))
                    pfail_at(&P, ns.line, ns.col, "duplicate unit namespace '%s'", ns.name);
            GROWV(arena, out->unitns, out->nunitns, out->capunitns, UnitNs);
            out->unitns[out->nunitns++] = ns;
        } else if (ptext(&P, 0, "unit") && peek(&P, 0)->kind == T_ID && peek(&P, 1)->kind == T_ID &&
                   ptext(&P, 2, ".") && peek(&P, 3)->kind == T_ID && ptext(&P, 4, "=")) {
            /* unit time.min = 60 */
            take(&P);
            Token *nst = identifier(&P);
            expect(&P, ".");
            Token *ut = identifier(&P);
            expect(&P, "=");
            SBuf factor = {0};
            for (int part = 0; part < 2; part++) {
                Token *n = take(&P);
                if (n->kind != T_INT && n->kind != T_REAL)
                    pfail_at(&P, n->line, n->col, "a unit's factor is a positive number, such as unit "
                                                  "time.min = 60 or unit time.ms = 1/1000%s", "");
                sb_add(&factor, n->text, (size_t)n->len);
                if (part == 1 || !accept(&P, "/"))
                    break;
                sb_addc(&factor, '/');
            }
            finish(&P);
            UnitNs *ns = NULL;
            for (int i = 0; i < out->nunitns; i++)
                if (tis(nst, out->unitns[i].name))
                    ns = &out->unitns[i];
            if (!ns)
                pfail_at(&P, nst->line, nst->col, "unknown unit namespace '%s'; declare it first with unitns",
                         tstr(&P, nst));
            const char *uname = tstr(&P, ut);
            for (int i = 0; i < ns->nunits; i++)
                if (!strcmp(ns->units[i], uname))
                    pfail_at(&P, ut->line, ut->col, "duplicate unit '%s.%s'", ns->name, uname);
            int cap = ns->capunits;
            GROWV(arena, ns->units, ns->nunits, cap, const char *);
            cap = ns->capunits;
            GROWV(arena, ns->factors, ns->nunits, cap, const char *);
            cap = ns->capunits;
            GROWV(arena, ns->unit_lines, ns->nunits, cap, int);
            ns->capunits = cap;
            ns->units[ns->nunits] = uname;
            ns->factors[ns->nunits] = arena_strndup(arena, factor.d, factor.n);
            ns->unit_lines[ns->nunits++] = ut->line;
            free(factor.d);
        } else if (ptext(&P, 0, "unitconv") && peek(&P, 0)->kind == T_ID &&
                   (peek(&P, 1)->kind == T_INT || peek(&P, 1)->kind == T_REAL)) {
            /* unitconv 1metric_length.m / 1time.s = 1metric_speed.mps */
            UnitConv c;
            memset(&c, 0, sizeof c);
            Token *kw = take(&P);
            c.line = kw->line;
            c.col = kw->col;
            for (int k = 0; k < 3; k++) {
                Token *n = take(&P);
                if (n->kind != T_INT && n->kind != T_REAL)
                    pfail_at(&P, n->line, n->col, "unitconv relates quantities, such as unitconv "
                                                  "1metric_length.m / 1time.s = 1metric_speed.mps%s", "");
                c.amounts[k] = tstr(&P, n);
                Token *first = identifier(&P);
                if (!accept(&P, "."))
                    pfail_at(&P, first->line, first->col, "unitconv names units with their namespace, such as 1time.s%s", "");
                c.namespaces[k] = tstr(&P, first);
                c.units[k] = tstr(&P, identifier(&P));
                if (ptext(&P, 0, ".") && peek(&P, 1)->kind == T_ID) {
                    /* 0.3048metric.length.m: a namespace of an imported module */
                    take(&P);
                    size_t a = strlen(c.namespaces[k]), b = strlen(c.units[k]);
                    char *q = arena_alloc(arena, a + b + 2);
                    memcpy(q, c.namespaces[k], a);
                    q[a] = '.';
                    memcpy(q + a + 1, c.units[k], b + 1);
                    c.namespaces[k] = q;
                    c.units[k] = tstr(&P, identifier(&P));
                }
                if (k == 0) {
                    if (ptext(&P, 0, "=")) {
                        /* unitconv 1length.ft = 0.3048metric.length.m */
                        take(&P);
                        c.op = "=";
                    } else {
                        if (!ptext(&P, 0, "*") && !ptext(&P, 0, "/"))
                            pfail(&P, "unitconv expects * or /, or = to relate two namespaces that measure the same "
                                      "thing%s", "");
                        c.op = tstr(&P, take(&P));
                    }
                } else if (k == 1) {
                    if (!strcmp(c.op, "="))
                        break;
                    expect(&P, "=");
                }
            }
            finish(&P);
            GROWV(arena, out->unitconvs, out->nunitconvs, out->capunitconvs, UnitConv);
            out->unitconvs[out->nunitconvs++] = c;
        } else if (ptext(&P, 0, "error") && peek(&P, 0)->kind == T_ID && peek(&P, 1)->kind == T_ID &&
                   (peek(&P, 2)->kind == T_NL || peek(&P, 2)->kind == T_EOF || ptext(&P, 2, "("))) {
            /* error Name / error Name(int status): a class with readonly fields and a trailing
             * `string message = ""`, which `throw Name with "message"` sets. */
            take(&P);
            ClassDecl c;
            memset(&c, 0, sizeof c);
            c.line = peek(&P, 0)->line;
            c.name = tstr(&P, identifier(&P));
            int capf = 0;
            if (accept(&P, "(") && !accept(&P, ")")) {
                do {
                    GROWV(arena, c.fields, c.nfields, capf, Param);
                    Param *fp = &c.fields[c.nfields++];
                    memset(fp, 0, sizeof *fp);
                    fp->line = peek(&P, 0)->line;
                    fp->readonly = true;
                    fp->type = parse_type(&P);
                    Token *fn = identifier(&P);
                    fp->name = tstr(&P, fn);
                    if (!strcmp(fp->name, "message"))
                        pfail_at(&P, fn->line, fn->col, "error types already have a message field%s", "");
                    for (int i = 0; i + 1 < c.nfields; i++)
                        if (!strcmp(c.fields[i].name, fp->name))
                            pfail_at(&P, fn->line, fn->col, "duplicate field '%s'", fp->name);
                    if (accept(&P, "="))
                        fp->value = expression(&P, 0);
                } while (accept(&P, ","));
                expect(&P, ")");
            }
            GROWV(arena, c.fields, c.nfields, capf, Param);
            Param *mp = &c.fields[c.nfields++];
            memset(mp, 0, sizeof *mp);
            mp->line = c.line;
            mp->readonly = true;
            mp->type = "string";
            mp->name = "message";
            mp->value = node(&P, N_STRING, "", c.line, 1);
            finish(&P);
            GROWV(arena, out->classes, out->nclasses, cap_cls, ClassDecl);
            out->classes[out->nclasses++] = c;
        } else if (ptext(&P, 0, "class") || ptext(&P, 0, "interface")) {
            ClassDecl c;
            memset(&c, 0, sizeof c);
            c.is_interface = tis(take(&P), "interface");
            c.line = peek(&P, 0)->line;
            c.name = tstr(&P, identifier(&P));
            skip_type_params(&P);
            if (!c.is_interface && accept(&P, "extends"))
                c.base = parse_type(&P);
            int capi = 0;
            if (accept(&P, c.is_interface ? "extends" : "implements"))
                do {
                    GROWV(arena, c.interfaces, c.ninterfaces, capi, const char *);
                    c.interfaces[c.ninterfaces++] = parse_type(&P);
                } while (accept(&P, ","));
            finish(&P);
            int capf = 0, capm = 0;
            while (!ptext(&P, 0, "end") && peek(&P, 0)->kind != T_EOF) {
                Token *mod = peek(&P, 0);
                bool hidden_member = accept(&P, "private");
                if (!hidden_member)
                    accept(&P, "public");
                bool readonly = ptext(&P, 0, "readonly") && peek(&P, 1)->kind == T_ID && accept(&P, "readonly");
                if ((hidden_member || readonly) && c.is_interface)
                    pfail_at(&P, mod->line, mod->col, "interface members cannot have modifiers%s", "");
                if (ptext(&P, 0, "function") && !ptext(&P, 1, "(")) {
                    if (readonly)
                        pfail_at(&P, mod->line, mod->col, "readonly applies to fields%s", "");
                    FuncDecl *m = parse_function(&P, c.name, c.is_interface);
                    m->is_private = hidden_member;
                    GROWV(arena, c.methods, c.nmethods, capm, FuncDecl);
                    c.methods[c.nmethods++] = *m;
                } else if (c.is_interface) {
                    pfail(&P, "interfaces declare only function signatures%s", "");
                } else {
                    GROWV(arena, c.fields, c.nfields, capf, Param);
                    Param *fp = &c.fields[c.nfields++];
                    memset(fp, 0, sizeof *fp);
                    fp->line = peek(&P, 0)->line;
                    fp->is_private = hidden_member;
                    fp->readonly = readonly;
                    fp->type = parse_type(&P);
                    fp->name = tstr(&P, identifier(&P));
                    if (accept(&P, "="))
                        fp->value = expression(&P, 0);
                    finish(&P);
                }
            }
            expect(&P, "end");
            finish(&P);
            GROWV(arena, out->classes, out->nclasses, cap_cls, ClassDecl);
            out->classes[out->nclasses++] = c;
        } else if (accept(&P, "component")) {
            Token *n = identifier(&P);
            expect(&P, "=");
            const char *nm = tstr(&P, n);
            for (int i = 0; i < out->ncomponents; i++)
                if (!strcmp(out->components[i].name, nm))
                    pfail_at(&P, n->line, n->col, "duplicate component%s", "");
            GROWV(arena, out->components, out->ncomponents, cap_comp, ComponentDecl);
            ComponentDecl *cd = &out->components[out->ncomponents++];
            cd->name = nm;
            cd->line = n->line;
            cd->markup = expression(&P, 0);
            finish(&P);
        } else {
            Node *s = statement(&P);
            if (s->kind == S_DEFER)
                pfail_at(&P, s->line, s->col, "defer must be inside a function or block%s", "");
            push_statement(&P, &out->body, s);
        }
        const char *declared = NULL;
        if (out->nfunctions > nf)
            declared = out->functions[out->nfunctions - 1].name;
        else if (out->nclasses > nc)
            declared = out->classes[out->nclasses - 1].name;
        else if (out->nenums > ne)
            declared = out->enums[out->nenums - 1].name;
        else if (out->ncomponents > ncomp)
            declared = out->components[out->ncomponents - 1].name;
        else if (out->body.count > nb && out->body.items[nb]->kind == S_DECLARE)
            declared = out->body.items[nb]->name;
        if ((hidden || visible) && !declared)
            pfail(&P, "visibility requires a declaration%s", "");
        if (hidden) {
            GROWV(arena, out->privates, out->nprivates, cap_priv, const char *);
            out->privates[out->nprivates++] = declared;
        }
    }
    if (!defer_units)
        lower_units(&P, out, NULL, 0, false);
    lower_reactive(&P, out);
    callables = saved_callables;
    return true;
}
