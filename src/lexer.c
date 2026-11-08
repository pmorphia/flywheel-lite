/* Tokenizer: a port of the Flywheel compiler's lexer, including markup-aware tokens,
 * line continuation, and the `spaced` flag that drives parentheses-free calls. */
#include "front.h"
#include <ctype.h>

bool lx_start(unsigned char c) { return isalpha(c) || c == '_'; }
bool lx_part(unsigned char c) { return isalnum(c) || c == '_'; }

uint32_t lx_scalar_at(const char *s, size_t n, size_t p, size_t *length) {
    unsigned char c = (unsigned char)s[p];
    size_t len = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    uint32_t v = len == 1 ? c : (uint32_t)(c & (0x3f >> (len - 1)));
    for (size_t i = 1; i < len && p + i < n; i++)
        v = (v << 6) | ((unsigned char)s[p + i] & 63);
    *length = len;
    return v;
}
bool lx_letter_scalar(uint32_t v) {
    if (v < 0x80)
        return isalpha((int)v) || v == '_';
    return !(v <= 0xbf || v == 0xd7 || v == 0xf7 || (v >= 0x2000 && v <= 0x2bff) ||
             (v >= 0x3000 && v <= 0x303f) || (v >= 0xfe30 && v <= 0xfe4f) ||
             (v >= 0xff00 && v <= 0xff20) || (v >= 0x1f000 && v <= 0x1faff));
}
size_t lx_identifier_end(const char *s, size_t n, size_t p) {
    while (p < n) {
        size_t len;
        uint32_t v = lx_scalar_at(s, n, p, &len);
        if (!(lx_letter_scalar(v) || (v < 0x80 && isdigit((int)v))))
            break;
        p += len;
    }
    return p;
}
static bool is_space(char c) { return isspace((unsigned char)c) != 0; }

bool lx_literal_open(const char *s, size_t n, size_t p) {
    return s[p] == '{' && (p + 1 >= n || is_space(s[p + 1]));
}
bool lx_literal_close(const char *s, size_t n, size_t p) {
    return s[p] == '}' && (p == 0 || p + 1 >= n || is_space(s[p - 1]) || is_space(s[p + 1]));
}
long lx_brace_end(const char *s, size_t n, size_t p) {
    int depth = 1;
    for (++p; p < n; ++p) {
        if (s[p] == '"') {
            long q = lx_quoted_end(s, n, p, '"');
            if (q < 0)
                return -1;
            p = (size_t)q;
        } else if (s[p] == '{')
            ++depth;
        else if (s[p] == '}' && --depth == 0)
            return (long)p;
    }
    return -1;
}
long lx_quoted_end(const char *s, size_t n, size_t p, char quote) {
    for (++p; p < n; ++p) {
        if (s[p] == '\\') {
            ++p;
            continue;
        }
        if (s[p] == '{' && p + 1 < n && s[p + 1] == '{') {
            ++p;
            continue;
        }
        if (s[p] == '{' && !lx_literal_open(s, n, p)) {
            long q = lx_brace_end(s, n, p);
            if (q < 0)
                return -1;
            p = (size_t)q;
            continue;
        }
        if (s[p] == quote)
            return (long)p;
    }
    return -1;
}
bool lx_html_void(const char *tag, size_t n) {
    static const char *tags[] = {"area", "base", "br", "col", "embed", "hr", "img", "input",
                                 "link", "meta", "param", "source", "track", "wbr", NULL};
    for (int i = 0; tags[i]; i++)
        if (strlen(tags[i]) == n && memcmp(tags[i], tag, n) == 0)
            return true;
    return false;
}
long lx_comment_end(const char *s, size_t n, size_t p) {
    for (size_t q = p + 4; q + 2 < n; q++)
        if (s[q] == '-' && s[q + 1] == '-' && s[q + 2] == '>')
            return (long)(q + 3);
    return -1;
}

typedef struct {
    const char *src;
    size_t n;
    size_t p;
    int line, col;
    int group;
    bool expected;
    TokenList *out;
    Arena *arena;
    CompileError *err;
    jmp_buf jmp;
} Lexer;

TB_NORETURN static void lfail(Lexer *L, const char *msg) {
    snprintf(L->err->message, sizeof L->err->message, "%s", msg);
    L->err->line = L->line;
    L->err->col = L->col;
    longjmp(L->jmp, 1);
}

static bool markup_argument_start(const char *s, size_t n, size_t p) {
    ++p;
    if (p == n)
        return false;
    if (s[p] == '>')
        return true;
    if (!lx_start((unsigned char)s[p]))
        return false;
#define NAME()                                                                                    \
    while (p < n && (lx_part((unsigned char)s[p]) || s[p] == '-' || s[p] == ':' || s[p] == '.'))  \
    ++p
#define SPACE()                                                                                   \
    while (p < n && is_space(s[p]))                                                               \
    ++p
    NAME();
    SPACE();
    if (p == n)
        return false;
    if (s[p] == '>' || (s[p] == '/' && p + 1 < n && s[p + 1] == '>'))
        return true;
    while (p < n && lx_start((unsigned char)s[p])) {
        size_t begin = p;
        NAME();
        static const char *ops[] = {"and", "or", "not", "in", "is", "as", NULL};
        for (int i = 0; ops[i]; i++)
            if (strlen(ops[i]) == p - begin && memcmp(ops[i], s + begin, p - begin) == 0)
                return false;
        SPACE();
        if (p < n && (s[p] == '=' || s[p] == '>' || (s[p] == '/' && p + 1 < n && s[p + 1] == '>')))
            return true;
    }
    return false;
#undef NAME
#undef SPACE
}

static long markup_end(const char *s, size_t n, size_t p) {
    int depth = 0;
    for (; p < n; ++p) {
        if (s[p] == '{' && p + 1 < n && s[p + 1] == '{') {
            ++p;
            continue;
        }
        if (s[p] == '{' && !lx_literal_open(s, n, p)) {
            long q = lx_brace_end(s, n, p);
            if (q < 0)
                return -1;
            p = (size_t)q;
            continue;
        }
        if (s[p] != '<')
            continue;
        if (p + 3 < n && memcmp(s + p, "<!--", 4) == 0) {
            long q = lx_comment_end(s, n, p);
            if (q < 0)
                return -2;
            p = (size_t)q - 1;
            continue;
        }
        bool closing = p + 1 < n && s[p + 1] == '/';
        size_t q = p + 1;
        for (; q < n && s[q] != '>'; ++q) {
            if (s[q] == '"' || (s[q] == '\'' && !closing)) {
                long e = lx_quoted_end(s, n, q, s[q]);
                if (e < 0)
                    return -1;
                q = (size_t)e;
            } else if (s[q] == '{') {
                long e = lx_brace_end(s, n, q);
                if (e < 0)
                    return -1;
                q = (size_t)e;
            }
        }
        if (q >= n)
            break;
        size_t name_end = p + 1;
        while (name_end < q && (lx_part((unsigned char)s[name_end]) || s[name_end] == '-' ||
                                s[name_end] == ':' || s[name_end] == '.'))
            ++name_end;
        if (closing)
            --depth;
        else if ((q == 0 || s[q - 1] != '/') && !lx_html_void(s + p + 1, name_end - p - 1))
            ++depth;
        if (depth == 0)
            return (long)q;
        p = q;
    }
    return -1;
}

static void advance(Lexer *L, size_t end) {
    while (L->p < end) {
        if (L->src[L->p++] == '\n') {
            L->line++;
            L->col = 1;
        } else
            L->col++;
    }
}
static void emit(Lexer *L, TokKind kind, const char *text, size_t len, size_t end) {
    TokenList *out = L->out;
    if (out->count == out->cap) {
        out->cap = GROW_CAP(out->cap);
        Token *nt = arena_alloc(L->arena, sizeof(Token) * (size_t)out->cap);
        if (out->count)
            memcpy(nt, out->items, sizeof(Token) * (size_t)out->count);
        out->items = nt;
    }
    Token *t = &out->items[out->count++];
    t->kind = kind;
    t->text = text;
    t->len = (int)len;
    t->line = L->line;
    t->col = L->col;
    t->depth = L->group;
    t->spaced = L->p == 0 || is_space(L->src[L->p - 1]);
    advance(L, end);
}
static bool tok_is(const Token *t, const char *s) {
    return (size_t)t->len == strlen(s) && memcmp(t->text, s, (size_t)t->len) == 0;
}

static void validate_utf8(Lexer *L) {
    const unsigned char *s = (const unsigned char *)L->src;
    size_t n = L->n;
    for (size_t i = 0; i < n;) {
        unsigned c = s[i++];
        if (c < 0x80)
            continue;
        unsigned k = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 99;
        if (k == 99 || i + k > n)
            lfail(L, "source is not valid UTF-8");
        unsigned scalar = c & ((1u << (6 - k)) - 1), count = k;
        while (k--) {
            unsigned d = s[i++];
            if ((d & 0xc0) != 0x80)
                lfail(L, "source is not valid UTF-8");
            scalar = (scalar << 6) | (d & 63);
        }
        if ((count == 2 && scalar < 0x800) || (count == 3 && scalar < 0x10000) || scalar > 0x10ffff ||
            (scalar >= 0xd800 && scalar <= 0xdfff))
            lfail(L, "source is not valid UTF-8");
    }
}

static bool lex_text(Arena *arena, const char *src, size_t len, TokenList *out, CompileError *err,
                     int line_offset) {
    Lexer L;
    memset(&L, 0, sizeof L);
    L.src = src;
    L.n = len;
    L.line = 1 + line_offset;
    L.col = 1;
    L.expected = true;
    L.out = out;
    L.arena = arena;
    L.err = err;
    out->items = NULL;
    out->count = out->cap = 0;
    if (setjmp(L.jmp))
        return false;
    validate_utf8(&L);
    const char *s = src;
    size_t n = len;
    while (L.p < n) {
        char c = s[L.p];
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(&L, L.p + 1);
            continue;
        }
        if (c == '#') {
            size_t e = L.p;
            while (e < n && s[e] != '\n')
                e++;
            advance(&L, e);
            continue;
        }
        if (c == '\\') {
            size_t e = L.p + 1;
            while (e < n && (s[e] == ' ' || s[e] == '\t' || s[e] == '\r'))
                e++;
            if (e < n && s[e] == '#')
                while (e < n && s[e] != '\n')
                    e++;
            if (e >= n || s[e] != '\n')
                lfail(&L, "'\\' continues a line only at the end of the line");
            advance(&L, e + 1);
            continue;
        }
        if (c == '\n' || c == ';') {
            if (c == '\n' && !L.group && out->count) {
                Token *last = &out->items[out->count - 1];
                static const char *joins[] = {"+",  "-",  "*",  "/",  "//", "%",  "==", "!=",
                                              "<=", ">=", "=",  "+=", "-=", "*=", "/=", "//=",
                                              "%=", "..", "..<", ",", "=>", "??", NULL};
                bool join = false;
                if (last->kind == T_OP)
                    for (int i = 0; joins[i]; i++)
                        if (tok_is(last, joins[i]))
                            join = true;
                bool wildcard = tok_is(last, "*") && out->count > 1 &&
                                (tok_is(&out->items[out->count - 2], ".") ||
                                 tok_is(&out->items[out->count - 2], ":"));
                if ((join && !wildcard) ||
                    (last->kind == T_ID && (tok_is(last, "and") || tok_is(last, "or")))) {
                    advance(&L, L.p + 1);
                    continue;
                }
            }
            if (!L.group)
                L.expected = true;
            emit(&L, T_NL, "\n", 1, L.p + 1);
            continue;
        }
        if (c == '\'') {
            size_t e = L.p + 1;
            if (e < n && s[e] == '\\') {
                if (e + 1 < n && s[e + 1] == 'u') {
                    size_t close = e;
                    while (close < n && s[close] != '}')
                        close++;
                    e = close >= n ? n : close + 1;
                } else
                    e += 2;
            } else if (e < n && s[e] != '\'' && s[e] != '\n') {
                size_t l;
                lx_scalar_at(s, n, e, &l);
                e += l;
            }
            if (e >= n || s[e] != '\'' || e == L.p + 1)
                lfail(&L, "character literals hold exactly one character, such as 'a'");
            emit(&L, T_CHAR, s + L.p + 1, e - L.p - 1, e + 1);
            L.expected = false;
            continue;
        }
        if (c == '"') {
            long end = lx_quoted_end(s, n, L.p, '"');
            if (end < 0)
                lfail(&L, "unterminated string");
            emit(&L, T_STRING, s + L.p + 1, (size_t)end - L.p - 1, (size_t)end + 1);
            L.expected = false;
            continue;
        }
        Token *prev = out->count ? &out->items[out->count - 1] : NULL;
        bool adjacent = L.p > 0 && prev && prev->kind == T_ID &&
                        (lx_part((unsigned char)s[L.p - 1]) || (unsigned char)s[L.p - 1] >= 0x80);
        bool generic_type = adjacent ||
                            (prev && prev->kind == T_ID &&
                             (tok_is(prev, "array") || tok_is(prev, "tuple") || tok_is(prev, "map") ||
                              tok_is(prev, "set") || tok_is(prev, "Result") || tok_is(prev, "weak")));
        if (c == '<' && !generic_type && L.p + 6 <= n && memcmp(s + L.p, "<json>", 6) == 0 &&
            (L.expected || markup_argument_start(s, n, L.p))) {
            size_t close = L.p + 6;
            while (close + 7 <= n && memcmp(s + close, "</json>", 7) != 0)
                close++;
            if (close + 7 > n)
                lfail(&L, "unterminated <json> literal: expected </json>");
            emit(&L, T_JSON, s + L.p + 6, close - L.p - 6, close + 7);
            L.expected = false;
            continue;
        }
        if (c == '<' && !generic_type && (L.expected || markup_argument_start(s, n, L.p)) &&
            L.p + 1 < n && (lx_start((unsigned char)s[L.p + 1]) || s[L.p + 1] == '>')) {
            long end = markup_end(s, n, L.p);
            if (end == -2)
                lfail(&L, "unterminated markup comment: expected '-->'");
            if (end < 0)
                lfail(&L, "unterminated markup literal");
            emit(&L, T_MARKUP, s + L.p, (size_t)end - L.p + 1, (size_t)end + 1);
            L.expected = false;
            continue;
        }
        size_t sl;
        if (lx_letter_scalar(lx_scalar_at(s, n, L.p, &sl))) {
            size_t e = lx_identifier_end(s, n, L.p);
            const char *id = s + L.p;
            size_t idn = e - L.p;
            emit(&L, T_ID, id, idn, e);
            Token *t = &out->items[out->count - 1];
            L.expected = tok_is(t, "return") || tok_is(t, "do") ||
                         tok_is(t, "and") || tok_is(t, "or") || tok_is(t, "not") || tok_is(t, "in");
            continue;
        }
        if (isdigit((unsigned char)c)) {
            size_t e = L.p;
            while (e < n && isdigit((unsigned char)s[e]))
                e++;
            bool real = false;
            if (e + 1 < n && s[e] == '.' && isdigit((unsigned char)s[e + 1])) {
                real = true;
                e++;
                while (e < n && isdigit((unsigned char)s[e]))
                    e++;
            }
            /* An exponent needs digits (1e5, 2E-3); otherwise `e` starts a unit (5eV). */
            size_t d = e + 1;
            if (d < n && (s[d] == '+' || s[d] == '-'))
                d++;
            if (e < n && (s[e] == 'e' || s[e] == 'E') && d < n && isdigit((unsigned char)s[d])) {
                real = true;
                e++;
                if (e < n && (s[e] == '+' || s[e] == '-'))
                    e++;
                size_t begin = e;
                while (e < n && isdigit((unsigned char)s[e]))
                    e++;
                if (e == begin)
                    lfail(&L, "expected exponent digits");
            }
            emit(&L, real ? T_REAL : T_INT, s + L.p, e - L.p, e);
            L.expected = false;
            continue;
        }
        size_t oplen = 1;
        if (L.p + 3 <= n && memcmp(s + L.p, "//=", 3) == 0)
            oplen = 3;
        else if (L.p + 1 < n) {
            static const char *two[] = {"==", "!=", "<=", ">=", "+=", "-=", "*=", "/=",
                                        "%=", "//", "..", "=>", "??", NULL};
            for (int i = 0; two[i]; i++)
                if (s[L.p] == two[i][0] && s[L.p + 1] == two[i][1])
                    oplen = 2;
        }
        if (oplen == 2 && s[L.p] == '.' && L.p + 2 < n && s[L.p + 2] == '<')
            oplen = 3;
        if (!strchr("+-*/%=!<>()[]{}.,?:", c) || c == 0)
            lfail(&L, "invalid character");
        if (c == '(' || c == '[' || c == '{')
            L.group++;
        if (c == ')' || c == ']' || c == '}') {
            if (--L.group < 0)
                lfail(&L, "unmatched closing delimiter");
        }
        emit(&L, T_OP, s + L.p, oplen, L.p + oplen);
        L.expected = c != ')' && c != ']' && c != '}';
    }
    if (L.group)
        lfail(&L, "unclosed delimiter");
    emit(&L, T_EOF, "", 0, L.p);
    return true;
}

bool lex_source(Arena *arena, const char *src, size_t len, TokenList *out, CompileError *err,
                int line_offset) {
    /* CRLF line endings (Windows editors, or text-mode writes) read as LF, so a string literal
       that spans lines holds "\n" rather than "\r\n". */
    if (len && memchr(src, '\r', len)) {
        char *copy = arena_alloc(arena, len + 1);
        size_t m = 0;
        for (size_t i = 0; i < len; i++)
            if (src[i] != '\r' || i + 1 >= len || src[i + 1] != '\n')
                copy[m++] = src[i];
        copy[m] = '\0';
        src = copy;
        len = m;
    }
    return lex_text(arena, src, len, out, err, line_offset);
}
