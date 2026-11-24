/* Native imports: `import c name` and `import cxx name` (`import++ name`). The source is scanned for
 * functions whose parameters and results cross the language boundary, a generated shim that includes
 * the source gives each one the same calling convention, the system C compiler builds the two into a
 * shared library (cached by content), and the VM calls the shims. No foreign-function library is
 * needed. `// flywheel:` comments in the source name the libraries and flags it needs.
 *
 * Structs by value, function pointers, Rust, and Zig need the Flywheel compiler. */
#include "lite.h"
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>

#ifndef _WIN32
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>

/* Part of the cache key: change it when the shim's shape changes. */
#define SHIM_VERSION "1"

typedef union {
    int64_t i;
    double d;
    const char *s;
} Slot;
typedef void (*ShimFn)(const Slot *args, Slot *result);

#define MAX_PARAMS 32
typedef struct {
    char *name;
    /* kinds: v void, i integer, d real, b bool, s string, p pointer */
    char result;
    char params[MAX_PARAMS + 1];
    char *spelled[MAX_PARAMS]; /* pointers: the C type, for the cast */
    char element[MAX_PARAMS];  /* pointers: what they point to (see element_code), 0 when opaque */
    bool constant[MAX_PARAMS]; /* pointers: to const, so nothing is copied back */
    bool owned;                /* C++: a std::string result */
    int nparams;
    struct Foreign *module;
    ShimFn fn;
} ForeignFn;

typedef struct Foreign {
    char *path;
    bool cxx;
    ForeignFn **fns;
    int nfns;
    char **packages, **cflags, **ldflags;
    int npackages, ncflags, nldflags;
} Foreign;

/* ---------------------------------------------------------------- tokens */

typedef struct {
    char **items;
    int count, cap;
} Words;
static void words_add(Words *w, const char *s, size_t n) {
    if (w->count == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 64;
        w->items = xrealloc(w->items, sizeof(char *) * (size_t)w->cap);
    }
    w->items[w->count++] = xstrndup(s, n);
}
static void words_free(Words *w) {
    for (int i = 0; i < w->count; i++)
        free(w->items[i]);
    free(w->items);
}
static bool word_char(char c) { return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }
/* Identifiers, numbers, literals (as "" and ''), and punctuation, without comments and
 * preprocessor lines. */
static Words tokenize(const char *s, size_t n) {
    Words out = {0};
    bool line_start = true;
    for (size_t p = 0; p < n;) {
        char c = s[p];
        if (isspace((unsigned char)c)) {
            line_start = line_start || c == '\n';
            p++;
        } else if (line_start && c == '#') {
            while (p < n && s[p] != '\n') {
                if (s[p] == '\\' && p + 2 < n && s[p + 1] == '\r')
                    p++;
                p += s[p] == '\\' || s[p] == '\r' ? 2 : 1;
                if (p >= 2 && s[p - 2] == '\r' && s[p - 1] == '\n') {
                    p--;
                    if (p >= 2 && s[p - 2] == '\\')
                        p++;
                }
            }
        } else if (c == '/' && p + 1 < n && s[p + 1] == '/') {
            while (p < n && s[p] != '\n')
                p++;
        } else if (c == '/' && p + 1 < n && s[p + 1] == '*') {
            for (p += 2; p + 1 < n && !(s[p] == '*' && s[p + 1] == '/');)
                p++;
            p += 2;
        } else if (c == '"' || c == '\'') {
            line_start = false;
            for (p++; p < n && s[p] != c && s[p] != '\n'; p++)
                p += s[p] == '\\';
            p++;
            words_add(&out, c == '"' ? "\"\"" : "''", 2);
        } else {
            line_start = false;
            size_t q = p;
            if (word_char(c))
                while (q < n && (word_char(s[q]) || (isdigit((unsigned char)c) && s[q] == '.')))
                    q++;
            else
                q += p + 1 < n && s[p] == ':' && s[p + 1] == ':' ? 2 : 1;
            words_add(&out, s + p, q - p);
            p = q;
        }
    }
    return out;
}
static bool is(const Words *t, int i, const char *s) { return i >= 0 && i < t->count && !strcmp(t->items[i], s); }
static int matching(const Words *t, int open) {
    const char *a = t->items[open], *z = *a == '(' ? ")" : *a == '{' ? "}" : "]";
    int depth = 0;
    for (int i = open; i < t->count; i++) {
        if (is(t, i, a))
            depth++;
        else if (is(t, i, z) && --depth == 0)
            return i;
    }
    return t->count;
}

/* ---------------------------------------------------------------- types */

static bool in_list(const char *w, const char *const *list) {
    for (int i = 0; list[i]; i++)
        if (!strcmp(list[i], w))
            return true;
    return false;
}
/* The words joined with spaces only where needed ("unsigned char*"). */
static void join_words(Buf *b, const Words *t, int from, int to, const char *const *drop) {
    buf_init(b);
    buf_adds(b, "");
    for (int i = from; i < to; i++) {
        if (drop && in_list(t->items[i], drop))
            continue;
        if (b->len && word_char(t->items[i][0]) && word_char(b->data[b->len - 1]))
            buf_addc(b, ' ');
        buf_adds(b, t->items[i]);
    }
    if (!b->data)
        buf_addc(b, 0), b->len = 0;
}
static const char *const QUALIFIERS[] = {"const",      "volatile",     "register", "restrict", "__restrict",
                                         "__restrict__", "inline",     "static",   "extern",   "constexpr",
                                         "noexcept",   "\"\"",         "SDLCALL",  NULL};
static const char *const STORAGE[] = {"register", "inline", "static", "extern", "constexpr", "noexcept", "\"\"", NULL};
static const char *const INTEGERS[] = {
    "int", "long", "short", "unsigned", "signed", "long long", "unsigned int", "signed int", "unsigned long",
    "long int", "unsigned long long", "long long int", "unsigned long int", "unsigned long long int", "short int",
    "unsigned short", "unsigned short int", "signed char", "unsigned char", "signed long", "signed short", "int8_t",
    "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "size_t", "ssize_t", "ptrdiff_t",
    "intptr_t", "uintptr_t", "intmax_t", "uintmax_t", "long unsigned", "long unsigned int", NULL};
/* What a pointer's elements are: f float, d double, an integer by size (lower case signed: 1, 2, 4, 8 are
 * b h w q; upper case unsigned), v for void (any), 0 when unknown. */
static char element_code(const char *type) {
    static const struct {
        const char *name;
        char code;
        size_t size;
    } types[] = {{"float", 'f', 0}, {"double", 'd', 0}, {"void", 'v', 0}, {"bool", 'o', 0}, {"_Bool", 'o', 0},
                 {"signed char", 'i', 1}, {"int8_t", 'i', 1}, {"unsigned char", 'u', 1}, {"uint8_t", 'u', 1},
                 {"short", 'i', 2}, {"short int", 'i', 2}, {"int16_t", 'i', 2}, {"unsigned short", 'u', 2},
                 {"uint16_t", 'u', 2}, {"int", 'i', 4}, {"signed int", 'i', 4}, {"int32_t", 'i', 4},
                 {"unsigned", 'u', 4}, {"unsigned int", 'u', 4}, {"uint32_t", 'u', 4},
                 {"long", 'i', sizeof(long)}, {"long int", 'i', sizeof(long)}, {"unsigned long", 'u', sizeof(long)},
                 {"long long", 'i', 8}, {"int64_t", 'i', 8}, {"ssize_t", 'i', sizeof(size_t)}, {"ptrdiff_t", 'i', sizeof(size_t)},
                 {"intptr_t", 'i', sizeof(size_t)}, {"unsigned long long", 'u', 8}, {"uint64_t", 'u', 8},
                 {"size_t", 'u', sizeof(size_t)}, {"uintptr_t", 'u', sizeof(size_t)}};
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
        if (!strcmp(types[i].name, type)) {
            if (!types[i].size)
                return types[i].code;
            const char *codes = types[i].code == 'i' ? "bhwq" : "BHWQ";
            return codes[types[i].size == 1 ? 0 : types[i].size == 2 ? 1 : types[i].size == 4 ? 2 : 3];
        }
    return 0;
}
/* The kind of a C type, 0 when it cannot cross. */
static char type_kind(const Words *t, int from, int to, bool cxx, bool parameter, bool *owned) {
    Buf b;
    join_words(&b, t, from, to, QUALIFIERS);
    const char *s = b.data;
    if (!strncmp(s, "std::", 5) && !strstr(s, "string"))
        s += 5;
    bool constant = false;
    for (int i = from; i < to; i++)
        constant = constant || is(t, i, "const");
    size_t n = strlen(s);
    char k = 0;
    if (in_list(s, INTEGERS))
        k = 'i';
    else if (!strcmp(s, "float") || !strcmp(s, "double") || !strcmp(s, "long double"))
        k = 'd';
    else if (!strcmp(s, "bool") || !strcmp(s, "_Bool"))
        k = 'b';
    else if (!strcmp(s, "void"))
        k = parameter ? 0 : 'v';
    else if (!strcmp(s, "char*"))
        k = 's';
    else if (n > 1 && s[n - 1] == '*' && !strpbrk(s, "([<&"))
        k = 'p';
    else if (cxx) {
        bool ref = n && s[n - 1] == '&';
        char bare[64];
        snprintf(bare, sizeof bare, "%.*s", (int)(n - ref), s);
        const char *name = !strncmp(bare, "std::", 5) ? bare + 5 : bare;
        if ((!strcmp(name, "string") || !strcmp(name, "string_view")) && !(ref && (!constant || !parameter))) {
            k = 's';
            *owned = true;
        }
    }
    buf_free(&b);
    return k;
}
/* A parameter: its type words, then usually a name. */
static bool scan_param(ForeignFn *f, const Words *t, int from, int to, bool cxx) {
    for (int k = from; k < to; k++)
        if (is(t, k, "=")) {
            to = k; /* default argument */
            break;
        }
    if (f->nparams == MAX_PARAMS || to <= from)
        return false;
    bool owned = false;
    Words flat = {0};
    int end = to;
    bool array = false;
    if (is(t, to - 1, "]")) { /* `const float m[16]` is a pointer */
        int open = to - 1;
        while (open > from && !is(t, open, "["))
            open--;
        end = open;
        array = true;
        if (is(t, end - 1, "]"))
            return false;
    }
    int last = end;
    char k = 0;
    if (!array)
        k = type_kind(t, from, last, cxx, true, &owned);
    if (!k) {
        if (end - from < 2 || !word_char(t->items[end - 1][0]))
            return false;
        last = end - 1; /* without the name */
        if (!array)
            k = type_kind(t, from, last, cxx, true, &owned);
    }
    for (int i = from; i < last; i++)
        words_add(&flat, t->items[i], strlen(t->items[i]));
    if (array) {
        words_add(&flat, "*", 1);
        k = type_kind(&flat, 0, flat.count, cxx, true, &owned);
    }
    if (k == 'p') {
        int p = f->nparams;
        Buf b;
        join_words(&b, &flat, 0, flat.count, STORAGE);
        f->spelled[p] = xstrdup(b.data);
        buf_free(&b);
        join_words(&b, &flat, 0, flat.count - 1, QUALIFIERS);
        f->element[p] = element_code(b.data);
        buf_free(&b);
        int star = flat.count - 1;
        while (star > 0 && !is(&flat, star, "*"))
            star--;
        for (int i = 0; i < star; i++)
            f->constant[p] = f->constant[p] || is(&flat, i, "const");
    }
    words_free(&flat);
    f->params[f->nparams++] = k;
    return k != 0;
}
static const char *const KEYWORDS[] = {
    "import", "as", "var", "const", "class", "function", "component", "returns", "return", "if", "else", "while", "do",
    "for", "in", "end", "break", "continue", "try", "catch", "finally", "throw", "inst", "attr", "true", "false", "null",
    "public", "private", "void", "weak", "and", "or", "not", "enum", "interface", "extends", "implements", "match",
    "case", "super", "is", "elements", "defer", "main", "operator", NULL};

static void free_fn(ForeignFn *f) {
    free(f->name);
    for (int i = 0; i < MAX_PARAMS; i++)
        free(f->spelled[i]);
    free(f);
}
/* Functions defined at the top level of tokens [from, to): `[result] name ( params ) {`. */
static void scan_functions(Foreign *m, const Words *t, int from, int to) {
    int start = from;
    for (int i = from; i < to; i++) {
        if (is(t, i, ";") || is(t, i, "}")) {
            start = i + 1;
        } else if (is(t, i, "{")) {
            int end = matching(t, i);
            if (is(t, i - 1, "\"\"") && is(t, i - 2, "extern")) /* extern "C" { ... } */
                scan_functions(m, t, i + 1, end < to ? end : to);
            i = end;
            start = i + 1;
        } else if (is(t, i, "(")) {
            int close = matching(t, i), body = close + 1;
            if (close >= to)
                return;
            while (is(t, body, "const") || is(t, body, "noexcept") || is(t, body, "override") || is(t, body, "final"))
                body++;
            if (!is(t, body, "{") || body >= to || i <= start) {
                i = close;
                continue;
            }
            const char *name = t->items[i - 1];
            bool ok = word_char(name[0]) && !isdigit((unsigned char)name[0]) && !is(t, i - 2, "::") &&
                      !in_list(name, KEYWORDS);
            ForeignFn *f = xmalloc(sizeof *f);
            memset(f, 0, sizeof *f);
            f->name = xstrdup(name);
            f->module = m;
            int head = i - 1, first = start;
            for (int k = start; k < i - 1; k++) {
                if (is(t, k, "template"))
                    ok = false;
                if (is(t, k, "__attribute__") || is(t, k, "__declspec")) {
                    head = k;
                    break;
                }
                if (k == first && is(t, k, "[") && is(t, k + 1, "[")) {
                    first = k = matching(t, k);
                    first++;
                }
            }
            if (ok)
                f->result = type_kind(t, first, head, m->cxx, false, &f->owned);
            ok = ok && f->result;
            if (ok && !(close == i + 1 || (close == i + 2 && is(t, i + 1, "void")))) {
                int depth = 0, at = i + 1;
                for (int k = i + 1; k <= close && ok; k++) {
                    if (k == close || (is(t, k, ",") && !depth)) {
                        ok = scan_param(f, t, at, k, m->cxx);
                        at = k + 1;
                    } else if (strchr("([{<", t->items[k][0]) && !t->items[k][1])
                        depth++;
                    else if (strchr(")]}>", t->items[k][0]) && !t->items[k][1])
                        depth--;
                }
            }
            for (int k = 0; k < m->nfns && ok; k++)
                ok = strcmp(m->fns[k]->name, name) != 0;
            if (ok) {
                m->fns = xrealloc(m->fns, sizeof(ForeignFn *) * (size_t)(m->nfns + 1));
                m->fns[m->nfns++] = f;
            } else
                free_fn(f);
            i = matching(t, body);
            start = i + 1;
        }
    }
}

/* ---------------------------------------------------------------- directives */

static void add_text(char ***list, int *n, const char *s, size_t len) {
    *list = xrealloc(*list, sizeof(char *) * (size_t)(*n + 1));
    (*list)[(*n)++] = xstrndup(s, len);
}
static bool platform_here(const char *p) {
    if (!*p || !strcmp(p, "unix"))
        return true;
#ifdef __APPLE__
    return !strcmp(p, "macos");
#else
    return !strcmp(p, "linux");
#endif
}
/* `// flywheel: pkg sdl3 >= 3.2`, `// flywheel(unix): link m`, cflags, ldflags */
static bool scan_directives(Foreign *m, const char *text, CompileError *err) {
    for (const char *line = text; line && *line;) {
        size_t n = strcspn(line, "\n");
        char l[1024];
        snprintf(l, sizeof l, "%.*s", (int)(n < sizeof l ? n : sizeof l - 1), line);
        line += n;
        line += *line == '\n';
        char *p = strstr(l, "//");
        if (!p)
            continue;
        p += 2;
        p += strspn(p, " \t/");
        if (strncmp(p, "flywheel", 8))
            continue;
        p += 8;
        char platform[32] = "";
        if (*p == '(') {
            char *close = strchr(p, ')');
            if (!close)
                continue;
            snprintf(platform, sizeof platform, "%.*s", (int)(close - p - 1), p + 1);
            p = close + 1;
        }
        if (*p++ != ':')
            continue;
        p += strspn(p, " \t");
        size_t vn = strcspn(p, " \t\r");
        char verb[64];
        snprintf(verb, sizeof verb, "%.*s", (int)vn, p);
        p += vn;
        static const char *const verbs[] = {"link", "pkg", "cflags", "ldflags", NULL};
        static const char *const platforms[] = {"", "linux", "windows", "macos", "unix", NULL};
        if (!verb[0])
            continue;
        if (!in_list(verb, verbs)) {
            snprintf(err->message, sizeof err->message,
                     "unknown directive `flywheel: %s` in %.500s (expected link, pkg, cflags, or ldflags)", verb, m->path);
            return false;
        }
        if (!in_list(platform, platforms)) {
            snprintf(err->message, sizeof err->message,
                     "unknown platform `flywheel(%s)` in %.500s (expected linux, windows, macos, or unix)", platform,
                     m->path);
            return false;
        }
        if (!platform_here(platform))
            continue;
        p += strspn(p, " \t");
        size_t end = strlen(p);
        while (end && isspace((unsigned char)p[end - 1]))
            end--;
        p[end] = 0;
        if (!strcmp(verb, "pkg")) {
            if (*p)
                add_text(&m->packages, &m->npackages, p, end);
            continue;
        }
        for (char *w = strtok(p, " \t"); w; w = strtok(NULL, " \t")) {
            char flag[600];
            snprintf(flag, sizeof flag, "%s%s", !strcmp(verb, "link") ? "-l" : "", w);
            if (!strcmp(verb, "cflags"))
                add_text(&m->cflags, &m->ncflags, flag, strlen(flag));
            else
                add_text(&m->ldflags, &m->nldflags, flag, strlen(flag));
        }
    }
    return true;
}

/* ---------------------------------------------------------------- shim */

static void add_shim(Buf *b, Foreign *m, const char *key) {
    /* The failure hook has a name of its own in each library, so modules never share one. */
    buf_printf(b, "#define fw_foreign_fail fw_foreign_fail_%s\n#define main fw_foreign_main_%s\n", key, key);
    buf_adds(b, m->cxx ? "#include <cstdint>\n#include <cstdlib>\n#include <cstring>\n#include <exception>\n"
                         "#include <string>\n#include <string_view>\n"
                       : "#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n#include <stdbool.h>\n");
    buf_adds(b, "#include \"");
    for (const char *c = m->path; *c; c++) {
        if (*c == '"' || *c == '\\')
            buf_addc(b, '\\');
        buf_addc(b, *c);
    }
    buf_adds(b, "\"\n");
    const char *ext = m->cxx ? "extern \"C\" " : "";
    buf_printf(b,
               "typedef union { int64_t i; double d; const char *s; } fwl_slot;\n"
               "static void (*fwl_fail)(const char *);\n"
               "%svoid fw_foreign_fail(const char *message) { if (fwl_fail) fwl_fail(message); }\n"
               "%svoid fwl_host(void (*fail)(const char *)) { fwl_fail = fail; }\n"
               "static char *fwl_copy(const char *s) {\n"
               "    size_t n = s ? strlen(s) : 0;\n"
               "    char *c = (char *)malloc(n + 1);\n"
               "    if (c) { if (n) memcpy(c, s, n); c[n] = 0; }\n"
               "    return c;\n}\n",
               ext, ext);
    for (int i = 0; i < m->nfns; i++) {
        ForeignFn *f = m->fns[i];
        Buf call;
        buf_init(&call);
        buf_printf(&call, "%s(", f->name);
        for (int k = 0; k < f->nparams; k++) {
            buf_adds(&call, k ? ", " : "");
            switch (f->params[k]) {
            case 'p': buf_printf(&call, "(%s)(intptr_t)a[%d].i", f->spelled[k], k); break;
            case 's': buf_printf(&call, "(char *)a[%d].s", k); break;
            case 'd': buf_printf(&call, "a[%d].d", k); break;
            case 'b': buf_printf(&call, "a[%d].i != 0", k); break;
            default: buf_printf(&call, "a[%d].i", k);
            }
        }
        buf_addc(&call, ')');
        buf_printf(b, "%svoid fwl_%s(const fwl_slot *a, fwl_slot *r) {\n    (void)a; (void)r;\n", ext, f->name);
        if (m->cxx)
            buf_adds(b, "    try {\n");
        switch (f->result) {
        case 'v': buf_printf(b, "    %s;\n", call.data); break;
        case 'd': buf_printf(b, "    r->d = (double)%s;\n", call.data); break;
        case 'b': buf_printf(b, "    r->i = %s ? 1 : 0;\n", call.data); break;
        case 'p': buf_printf(b, "    r->i = (int64_t)(intptr_t)%s;\n", call.data); break;
        case 's':
            buf_printf(b, f->owned ? "    r->s = fwl_copy(std::string(%s).c_str());\n" : "    r->s = fwl_copy(%s);\n", call.data);
            break;
        default: buf_printf(b, "    r->i = (int64_t)%s;\n", call.data);
        }
        if (m->cxx)
            buf_adds(b, "    } catch (const std::exception &e) { fw_foreign_fail(e.what()); }\n"
                        "    catch (...) { fw_foreign_fail(\"C++ exception\"); }\n");
        buf_adds(b, "}\n");
        buf_free(&call);
    }
}

/* ---------------------------------------------------------------- building */

static char *read_text(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    Buf b;
    buf_init(&b);
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        buf_add(&b, chunk, n);
    fclose(f);
    buf_addc(&b, 0);
    *len = b.len - 1;
    return b.data;
}
static uint64_t hash64(uint64_t h, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * 1099511628211ull;
    return h;
}
static void directory_of(const char *path, char *out, size_t size) {
    const char *slash = strrchr(path, '/');
    snprintf(out, size, "%.*s", slash ? (int)(slash - path) : 1, slash ? path : ".");
}
/* Adds the files the source includes with quotes (and theirs), so editing a header rebuilds. */
static uint64_t hash_includes(uint64_t h, const char *path, const char *text, int depth) {
    char dir[2048];
    directory_of(path, dir, sizeof dir);
    for (const char *p = text; depth < 8 && (p = strstr(p, "#include")); p++) {
        const char *q = p + 8 + strspn(p + 8, " \t");
        size_t n;
        if (*q != '"' || !(n = strcspn(q + 1, "\"\n")))
            continue;
        char file[4096];
        snprintf(file, sizeof file, "%s/%.*s", dir, (int)n, q + 1);
        size_t len;
        char *inner = read_text(file, &len);
        if (!inner)
            continue;
        h = hash_includes(hash64(h, inner, len), file, inner, depth + 1);
        free(inner);
    }
    return h;
}
static void make_dirs(char *path) {
    for (char *p = path + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(path, 0777);
            *p = '/';
        }
    mkdir(path, 0777);
}
/* Appends ` 'word'`, quoted for the shell. */
static void shell_word(Buf *b, const char *s) {
    buf_adds(b, " '");
    for (; *s; s++)
        if (*s == '\'')
            buf_adds(b, "'\\''");
        else
            buf_addc(b, *s);
    buf_addc(b, '\'');
}
/* Runs a shell command and collects what it prints; its exit status, or -1. */
static int run_command(const char *command, Buf *out) {
    fflush(NULL);
    FILE *p = popen(command, "r");
    if (!p)
        return -1;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, p)) > 0)
        buf_add(out, chunk, n);
    buf_addc(out, 0);
    out->len--;
    int status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
static bool package_flags(Foreign *m, const char *what, Buf *flags, CompileError *err) {
    for (int i = 0; i < m->npackages; i++) {
        Buf cmd, out;
        buf_init(&cmd);
        buf_init(&out);
        buf_printf(&cmd, "pkg-config %s", what);
        char *spec = xstrdup(m->packages[i]);
        for (char *w = strtok(spec, " \t"); w; w = strtok(NULL, " \t"))
            shell_word(&cmd, w);
        free(spec);
        buf_adds(&cmd, " 2>&1");
        int status = run_command(cmd.data, &out);
        if (!status)
            for (char *w = strtok(out.data, " \t\r\n"); w; w = strtok(NULL, " \t\r\n"))
                shell_word(flags, w);
        else if (status == 127)
            snprintf(err->message, sizeof err->message, "%.500s needs `pkg-config` on PATH (flywheel: pkg %.200s)",
                     m->path, m->packages[i]);
        else
            snprintf(err->message, sizeof err->message,
                     "%.300s needs the `%.200s` development package (flywheel: pkg), which pkg-config cannot find:\n%.400s",
                     m->path, m->packages[i], out.data);
        buf_free(&cmd);
        buf_free(&out);
        if (status)
            return false;
    }
    return true;
}

/* The message of fw_foreign_fail during the running call. */
static char *foreign_failure;
static void foreign_fail(const char *message) {
    if (!foreign_failure)
        foreign_failure = xstrdup(message && *message ? message : "native function failed");
}

static bool build(Foreign *m, const char *text, size_t len, CompileError *err) {
    const char *cc = getenv(m->cxx ? "CXX" : "CC");
    if (!cc || !*cc)
        cc = m->cxx ? "c++" : "cc";
    uint64_t h = hash64(14695981039346656037ull, SHIM_VERSION, strlen(SHIM_VERSION));
    h = hash64(hash64(h, m->path, strlen(m->path) + 1), cc, strlen(cc) + 1);
    h = hash_includes(hash64(h, text, len), m->path, text, 0);
    char key[32], dir[2048], lib[2200], partial[2300], shim[2400];
    snprintf(key, sizeof key, "%016llx", (unsigned long long)h);
    const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (cache && *cache)
        snprintf(dir, sizeof dir, "%s/flywheel-lite/native", cache);
    else
        snprintf(dir, sizeof dir, "%s/.cache/flywheel-lite/native", home ? home : "/tmp");
    snprintf(lib, sizeof lib, "%s/%s-%s.so", dir, m->cxx ? "cxx" : "c", key);
    if (access(lib, R_OK)) {
        make_dirs(dir);
        snprintf(partial, sizeof partial, "%s.%ld", lib, (long)getpid());
        snprintf(shim, sizeof shim, "%s.%s", partial, m->cxx ? "cpp" : "c");
        Buf src, cmd, out;
        buf_init(&src);
        buf_init(&cmd);
        buf_init(&out);
        add_shim(&src, m, key);
        FILE *f = fopen(shim, "wb");
        bool ok = f && fwrite(src.data, 1, src.len, f) == src.len;
        if (f && fclose(f))
            ok = false;
        if (!ok)
            snprintf(err->message, sizeof err->message, "cannot write %.900s", shim);
        if (ok) {
            char at[2048];
            directory_of(m->path, at, sizeof at);
            buf_printf(&cmd, "%s -O2 -fPIC -shared -w %s", cc, m->cxx ? "-std=gnu++17" : "-std=gnu11");
#ifndef __APPLE__
            buf_adds(&cmd, " -Wl,-Bsymbolic");
#endif
            Buf inc;
            buf_init(&inc);
            buf_printf(&inc, "-I%s", at);
            shell_word(&cmd, inc.data);
            buf_free(&inc);
            for (int i = 0; i < m->ncflags; i++)
                shell_word(&cmd, m->cflags[i]);
            ok = package_flags(m, "--cflags", &cmd, err);
            shell_word(&cmd, shim);
            buf_adds(&cmd, " -o");
            shell_word(&cmd, partial);
            ok = ok && package_flags(m, "--libs", &cmd, err);
            for (int i = 0; i < m->nldflags; i++)
                shell_word(&cmd, m->ldflags[i]);
            buf_adds(&cmd, " 2>&1");
        }
        if (ok) {
            int status = run_command(cmd.data, &out);
            ok = !status && !rename(partial, lib);
            if (status == 127)
                snprintf(err->message, sizeof err->message,
                         "native '%s' imports need a C%s compiler: `%.100s` was not found (set %s to name one)",
                         m->cxx ? "cxx" : "c", m->cxx ? "++" : "", cc, m->cxx ? "CXX" : "CC");
            else if (!ok)
                snprintf(err->message, sizeof err->message, "cannot compile %.300s:\n%.600s", m->path,
                         out.data ? out.data : "");
        }
        remove(shim);
        remove(partial);
        buf_free(&src);
        buf_free(&cmd);
        buf_free(&out);
        if (!ok)
            return false;
    }
    void *handle = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *why = dlerror();
        if (why && strstr(why, "Dynamic loading not supported")) /* musl's dlopen in a static program */
            why = "this flywheel-lite is statically linked and cannot load native code; build it from source";
        snprintf(err->message, sizeof err->message, "cannot load %.300s: %.600s", m->path, why ? why : "unknown error");
        return false;
    }
    void (*host)(void (*)(const char *));
    *(void **)&host = dlsym(handle, "fwl_host");
    if (host)
        host(foreign_fail);
    for (int i = 0; i < m->nfns; i++) {
        char symbol[600];
        snprintf(symbol, sizeof symbol, "fwl_%s", m->fns[i]->name);
        *(void **)&m->fns[i]->fn = dlsym(handle, symbol);
    }
    return true;
}

/* ---------------------------------------------------------------- arrays passed by address */

static size_t code_size(char c) {
    switch (c) {
    case 'b': case 'B': case 'o': return 1;
    case 'h': case 'H': return 2;
    case 'w': case 'W': case 'f': return 4;
    default: return 8;
    }
}
/* How a struct's field is stored in memory. */
static char field_code(ObjClass *k, int i, Value v) {
    if (IS_OBJ(k->field_types[i])) {
        ObjTypeDesc *t = AS_TYPE(k->field_types[i]);
        if (t->kind == TY_FLOAT)
            return 'f';
        if (t->kind == TY_INT) {
            const char *codes = t->is_unsigned ? "BHWQ" : "bhwq";
            return codes[t->bits == 8 ? 0 : t->bits == 16 ? 1 : t->bits == 32 ? 2 : 3];
        }
    }
    return IS_DOUBLE(v) ? 'd' : IS_BOOL(v) ? 'o' : IS_CHAR(v) ? 'W' : 'q';
}
static size_t align_to(size_t n, size_t a) { return (n + a - 1) / a * a; }
static bool layout(Value v, size_t *size, size_t *align) {
    ObjInstance *in = AS_INSTANCE(v);
    if (!in->klass->is_struct)
        return false;
    *size = 0;
    *align = 1;
    for (int i = 0; i < in->nfields; i++) {
        size_t s, a;
        if (IS_OBJT(in->fields[i], OBJ_INSTANCE)) {
            if (!layout(in->fields[i], &s, &a))
                return false;
        } else if (IS_OBJ(in->fields[i]) || IS_NULL(in->fields[i]))
            return false;
        else
            s = a = code_size(field_code(in->klass, i, in->fields[i]));
        *size = align_to(*size, a) + s;
        if (a > *align)
            *align = a;
    }
    *size = align_to(*size, *align);
    return true;
}
static void put_scalar(char *p, char code, Value v) {
    int64_t i = IS_DOUBLE(v) ? 0 : IS_BOOL(v) ? v.as.b : IS_CHAR(v) ? (int64_t)v.as.c : v.as.i;
    if (code == 'f') {
        float f = (float)(IS_NUM(v) ? AS_NUM(v) : 0);
        memcpy(p, &f, 4);
    } else if (code == 'd') {
        double d = IS_NUM(v) ? AS_NUM(v) : 0;
        memcpy(p, &d, 8);
    } else {
        int8_t b = (int8_t)i;
        int16_t h = (int16_t)i;
        int32_t w = (int32_t)i;
        size_t n = code_size(code);
        memcpy(p, n == 1 ? (void *)&b : n == 2 ? (void *)&h : n == 4 ? (void *)&w : (void *)&i, n);
    }
}
static Value get_scalar(const char *p, char code, Value old) {
    if (code == 'f') {
        float f;
        memcpy(&f, p, 4);
        return val_double(f);
    }
    if (code == 'd') {
        double d;
        memcpy(&d, p, 8);
        return val_double(d);
    }
    int64_t i = 0;
    size_t n = code_size(code);
    uint8_t b;
    uint16_t h;
    uint32_t w;
    if (n == 1)
        memcpy(&b, p, 1), i = islower((unsigned char)code) && code != 'o' ? (int8_t)b : b;
    else if (n == 2)
        memcpy(&h, p, 2), i = code == 'h' ? (int16_t)h : h;
    else if (n == 4)
        memcpy(&w, p, 4), i = code == 'w' ? (int64_t)(int32_t)w : (int64_t)w;
    else
        memcpy(&i, p, 8);
    return IS_BOOL(old) ? val_bool(i != 0) : IS_CHAR(old) ? val_char((uint32_t)i) : val_int(i);
}
static void put_struct(char *p, Value v) {
    ObjInstance *in = AS_INSTANCE(v);
    size_t at = 0;
    for (int i = 0; i < in->nfields; i++) {
        size_t s, a;
        if (IS_OBJT(in->fields[i], OBJ_INSTANCE)) {
            layout(in->fields[i], &s, &a);
            at = align_to(at, a);
            put_struct(p + at, in->fields[i]);
        } else {
            char code = field_code(in->klass, i, in->fields[i]);
            s = a = code_size(code);
            at = align_to(at, a);
            put_scalar(p + at, code, in->fields[i]);
        }
        at += s;
    }
}
/* A struct with the fields at p; called while collection is off. */
static Value get_struct(VM *vm, const char *p, Value old) {
    ObjInstance *in = AS_INSTANCE(old), *out = new_instance(vm, in->klass);
    size_t at = 0;
    for (int i = 0; i < in->nfields; i++) {
        size_t s, a;
        if (IS_OBJT(in->fields[i], OBJ_INSTANCE)) {
            layout(in->fields[i], &s, &a);
            at = align_to(at, a);
            out->fields[i] = get_struct(vm, p + at, in->fields[i]);
        } else {
            char code = field_code(in->klass, i, in->fields[i]);
            s = a = code_size(code);
            at = align_to(at, a);
            out->fields[i] = get_scalar(p + at, code, in->fields[i]);
        }
        at += s;
    }
    return val_obj(out);
}
typedef struct {
    ObjList *list;
    char *data;
    size_t size; /* of an element */
    char code;   /* of its elements; 0 for structs */
    bool write_back;
} Block;
static bool pack(VM *vm, ForeignFn *f, int p, ObjList *list, Block *b) {
    char code = f->element[p];
    b->list = list;
    b->write_back = !f->constant[p];
    bool structs = list->count && IS_OBJT(list->items[0], OBJ_INSTANCE);
    size_t align = 1;
    b->code = 0;
    if (structs) {
        if ((code && code != 'v') || !layout(list->items[0], &b->size, &align))
            return vm_throw(vm, "TypeError", "argument %d of %s cannot take an array of %s", p + 1, f->name,
                            AS_INSTANCE(list->items[0])->klass->name->chars);
    } else {
        if (!code)
            return vm_throw(vm, "TypeError", "argument %d of %s needs an array of structs or an int handle", p + 1, f->name);
        if (code == 'v')
            code = !list->count || IS_DOUBLE(list->items[0]) ? 'd' : IS_BOOL(list->items[0]) ? 'o' : 'q';
        b->code = code;
        b->size = code_size(code);
    }
    for (int i = 0; i < list->count; i++) {
        Value v = list->items[i];
        bool ok = structs ? IS_OBJT(v, OBJ_INSTANCE) && AS_INSTANCE(v)->klass == AS_INSTANCE(list->items[0])->klass
                  : b->code == 'f' || b->code == 'd' ? IS_NUM(v)
                                                     : IS_INT(v) || IS_BOOL(v) || IS_CHAR(v);
        if (!ok)
            return vm_throw(vm, "TypeError", "argument %d of %s points to other values than the array's", p + 1, f->name);
    }
    b->data = xmalloc(b->size * (size_t)list->count + 16);
    for (int i = 0; i < list->count; i++) {
        if (structs)
            put_struct(b->data + b->size * (size_t)i, list->items[i]);
        else
            put_scalar(b->data + b->size * (size_t)i, b->code, list->items[i]);
    }
    return true;
}
static void unpack(VM *vm, Block *b) {
    vm->gc_off++;
    for (int i = 0; b->write_back && i < b->list->count; i++) {
        const char *p = b->data + b->size * (size_t)i;
        if (b->code)
            b->list->items[i] = get_scalar(p, b->code, b->list->items[i]);
        else
            b->list->items[i] = get_struct(vm, p, b->list->items[i]);
    }
    vm->gc_off--;
    free(b->data);
}

/* ---------------------------------------------------------------- calls */

static bool bad_argument(VM *vm, ForeignFn *f, int i, const char *want, Value got) {
    Buf b;
    buf_init(&b);
    type_name_of(vm, got, &b);
    vm_throw(vm, "TypeError", "argument %d of %s must be %s, not %s", i + 1, f->name, want, b.data ? b.data : "this");
    buf_free(&b);
    return false;
}
static bool call_foreign(VM *vm, int argc, Value *args, Value *out) {
    ForeignFn *f = (ForeignFn *)(intptr_t)vm->desc_args[0].as.i;
    Slot slots[MAX_PARAMS] = {{0}}, result;
    Block blocks[MAX_PARAMS];
    int nblocks = 0;
    bool ok = true;
    result.i = 0;
    for (int i = 0; i < argc && ok; i++) {
        Value v = args[i];
        switch (f->params[i]) {
        case 'i':
            if (!(ok = IS_INT(v)))
                bad_argument(vm, f, i, "an int", v);
            slots[i].i = v.as.i;
            break;
        case 'd':
            if (!(ok = IS_NUM(v)))
                bad_argument(vm, f, i, "a double", v);
            else
                slots[i].d = AS_NUM(v);
            break;
        case 'b':
            if (!(ok = IS_BOOL(v)))
                bad_argument(vm, f, i, "a bool", v);
            slots[i].i = v.as.b;
            break;
        case 's':
            if (!(ok = IS_STRING(v)))
                bad_argument(vm, f, i, "a string", v);
            else
                slots[i].s = AS_CSTR(v);
            break;
        default:
            if (IS_INT(v))
                slots[i].i = v.as.i;
            else if (!IS_ARRAY(v))
                ok = bad_argument(vm, f, i, "an int handle or an array", v);
            else if ((ok = pack(vm, f, i, AS_LIST(v), &blocks[nblocks])))
                slots[i].i = (int64_t)(intptr_t)blocks[nblocks++].data;
        }
    }
    if (ok && !f->fn)
        ok = vm_throw(vm, "NativeError", "%s is not in the compiled module", f->name);
    if (ok)
        f->fn(slots, &result);
    for (int i = 0; i < nblocks; i++) {
        blocks[i].write_back = blocks[i].write_back && ok;
        unpack(vm, &blocks[i]);
    }
    if (ok && foreign_failure) {
        ok = vm_throw(vm, "NativeError", "%s", foreign_failure);
        free(foreign_failure);
        foreign_failure = NULL;
        if (f->result == 's')
            free((char *)result.s);
    }
    if (!ok)
        return false;
    switch (f->result) {
    case 'v': *out = val_null(); break;
    case 'd': *out = val_double(result.d); break;
    case 'b': *out = val_bool(result.i != 0); break;
    case 's':
        *out = val_obj(str_cstr(vm, result.s ? result.s : ""));
        free((char *)result.s);
        break;
    default: *out = val_int(result.i);
    }
    return true;
}

/* ---------------------------------------------------------------- modules */

static bool is_file(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISREG(st.st_mode);
}
static char *find_source(VM *vm, const char *name, bool path, bool cxx, const char *from) {
    static const char *const c_ext[] = {".c", ".h", NULL};
    static const char *const cxx_ext[] = {".cpp", ".cc", ".cxx", ".c++", ".hpp", ".hh", ".hxx", ".h", NULL};
    const char *const *exts = cxx ? cxx_ext : c_ext;
    char rel[1024], dirs[2][2048], file[5200];
    snprintf(rel, sizeof rel, "%s", name);
    for (char *c = rel; !path && *c; c++)
        if (*c == '.')
            *c = '/';
    directory_of(from ? from : ".", dirs[0], sizeof dirs[0]);
    directory_of(vm->main_path ? vm->main_path : ".", dirs[1], sizeof dirs[1]);
    for (int d = 0; d < 2; d++) {
        const char *dir = path && rel[0] == '/' ? "" : dirs[d];
        const char *sep = *dir ? "/" : "";
        snprintf(file, sizeof file, "%s%s%s", dir, sep, rel);
        if (path && is_file(file))
            return realpath(file, NULL);
        for (int e = 0; exts[e]; e++) {
            snprintf(file, sizeof file, "%s%s%s%s", dir, sep, rel, exts[e]);
            if (is_file(file))
                return realpath(file, NULL);
        }
    }
    return NULL;
}

ObjModule *lib_foreign_module(VM *vm, const char *kind, const char *name, bool path, const char *from,
                              CompileError *err) {
    bool cxx = !strcmp(kind, "cxx");
    if (!cxx && strcmp(kind, "c")) {
        snprintf(err->message, sizeof err->message,
                 "native '%s' imports need the Flywheel compiler; Flywheel Lite builds C and C++", kind);
        return NULL;
    }
    char *file = find_source(vm, name, path, cxx, from);
    if (!file) {
        snprintf(err->message, sizeof err->message, "%s source for '%.500s' not found", cxx ? "C++" : "C", name);
        return NULL;
    }
    char key[5200];
    snprintf(key, sizeof key, "<%s %s>", kind, file);
    Value existing;
    if (table_get(&vm->modules, str_intern_c(vm, key), &existing)) {
        free(file);
        return AS_MODULE(existing);
    }
    size_t len;
    char *text = read_text(file, &len);
    Foreign *m = xmalloc(sizeof *m); /* lives as long as the program, with its library */
    memset(m, 0, sizeof *m);
    m->path = file;
    m->cxx = cxx;
    bool ok = text != NULL;
    if (!ok)
        snprintf(err->message, sizeof err->message, "cannot read %.900s", file);
    ok = ok && scan_directives(m, text, err);
    if (ok) {
        Words t = tokenize(text, len);
        scan_functions(m, &t, 0, t.count);
        words_free(&t);
        if (!m->nfns) {
            snprintf(err->message, sizeof err->message,
                     "no importable functions in %.600s (functions need int, floating-point, bool, string, pointer, or "
                     "void parameters and results)",
                     file);
            ok = false;
        }
    }
    ok = ok && (vm->check_only || build(m, text, len, err));
    free(text);
    if (!ok)
        return NULL;
    vm->gc_off++;
    const char *base = strrchr(name, '.');
    ObjModule *mod = new_module(vm, str_intern_c(vm, !path && base ? base + 1 : name));
    mod->native = mod->initialized = true;
    table_set(vm, &vm->modules, str_intern_c(vm, key), val_obj(mod));
    for (int i = 0; i < m->nfns; i++) {
        ForeignFn *f = m->fns[i];
        ObjNative *n = new_native(vm, f->name, call_foreign, f->nparams, f->nparams);
        n->data = val_int((int64_t)(intptr_t)f);
        module_set_value(vm, mod, f->name, val_obj(n));
    }
    vm->gc_off--;
    return mod;
}

#else

ObjModule *lib_foreign_module(VM *vm, const char *kind, const char *name, bool path, const char *from,
                              CompileError *err) {
    (void)vm, (void)name, (void)path, (void)from;
    snprintf(err->message, sizeof err->message,
             "native '%s' imports need the Flywheel compiler on Windows; Flywheel Lite builds native code on Linux and macOS",
             kind);
    return NULL;
}

#endif
