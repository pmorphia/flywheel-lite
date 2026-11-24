/* toml.parse: TOML 1.0 documents as json.value trees, as the Flywheel compiler reads them (dates
 * and times become strings). */
#include "lite.h"
#include <ctype.h>
#include <math.h>
#include <setjmp.h>

typedef struct {
    VM *vm;
    const char *s;
    size_t n, i;
    int line;
    jmp_buf jump;
    char message[512];
    /* tables declared by [headers], and inline tables, which cannot be extended */
    ObjMap **defined, **sealed;
    int ndefined, nsealed;
} Toml;

typedef struct {
    ObjString *parts[64];
    int n;
} Path;

TB_NORETURN static void fail(Toml *t, const char *fmt, const char *arg) {
    int n = snprintf(t->message, sizeof t->message, "TOML line %d: ", t->line);
    snprintf(t->message + n, sizeof t->message - (size_t)n, fmt, arg);
    longjmp(t->jump, 1);
}
static bool at(Toml *t, char c) { return t->i < t->n && t->s[t->i] == c; }
static bool ahead(Toml *t, const char *text) {
    size_t k = strlen(text);
    return t->n - t->i >= k && !memcmp(t->s + t->i, text, k);
}
static void spaces(Toml *t) {
    while (at(t, ' ') || at(t, '\t'))
        t->i++;
}
static void comment(Toml *t) {
    spaces(t);
    if (at(t, '#'))
        while (t->i < t->n && t->s[t->i] != '\n')
            t->i++;
}
static bool newline(Toml *t) {
    size_t k = ahead(t, "\r\n") ? 2 : at(t, '\n') ? 1 : 0;
    t->i += k;
    t->line += k != 0;
    return k != 0;
}
static void blank(Toml *t) {
    do
        comment(t);
    while (newline(t));
}
static void end_of_line(Toml *t) {
    comment(t);
    if (t->i < t->n && !newline(t))
        fail(t, "expected the end of the line%s", "");
}
static ObjString *take(Toml *t, Buf *b) {
    ObjString *s = str_copy(t->vm, b->data ? b->data : "", b->len);
    buf_free(b);
    return s;
}
TB_NORETURN static void fail_buf(Toml *t, Buf *b, const char *message) {
    buf_free(b);
    fail(t, "%s", message);
}
static void escape(Toml *t, Buf *b) {
    if (++t->i >= t->n)
        fail_buf(t, b, "unterminated escape");
    char c = t->s[t->i++];
    static const char from[] = "btnfre\"\\", to[] = "\b\t\n\f\r\x1b\"\\";
    const char *p = strchr(from, c);
    if (p && c) {
        buf_addc(b, to[p - from]);
        return;
    }
    if (c != 'u' && c != 'U') {
        char m[32];
        snprintf(m, sizeof m, "unknown escape \\%c", c);
        fail_buf(t, b, m);
    }
    int k = c == 'u' ? 4 : 8;
    uint32_t v = 0;
    for (; k > 0; k--, t->i++) {
        if (t->i >= t->n)
            fail_buf(t, b, "incomplete Unicode escape");
        unsigned char h = (unsigned char)t->s[t->i];
        if (!isxdigit(h))
            fail_buf(t, b, "invalid Unicode escape");
        v = v * 16 + (uint32_t)(isdigit(h) ? h - '0' : (tolower(h) - 'a' + 10));
    }
    if (v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
        fail_buf(t, b, "invalid Unicode scalar in escape");
    buf_utf8(b, v);
}
static ObjString *basic_string(Toml *t, bool multi) {
    Buf b;
    buf_init(&b);
    if (multi && ahead(t, "\"\"\"")) {
        t->i += 3;
        newline(t);
        for (;;) {
            if (t->i >= t->n)
                fail_buf(t, &b, "unterminated multi-line string");
            if (ahead(t, "\"\"\"")) {
                t->i += 3;
                /* up to two extra quotes belong to the content */
                for (int extra = 0; extra < 2 && at(t, '"'); extra++, t->i++)
                    buf_addc(&b, '"');
                return take(t, &b);
            }
            if (t->s[t->i] == '\\') {
                size_t k = t->i + 1;
                while (k < t->n && (t->s[k] == ' ' || t->s[k] == '\t'))
                    k++;
                if (k < t->n && (t->s[k] == '\n' || t->s[k] == '\r')) {
                    /* a line-ending backslash trims the newline and following whitespace */
                    for (t->i = k; t->i < t->n && isspace((unsigned char)t->s[t->i]); t->i++)
                        t->line += t->s[t->i] == '\n';
                    continue;
                }
                escape(t, &b);
                continue;
            }
            t->line += t->s[t->i] == '\n';
            buf_addc(&b, t->s[t->i++]);
        }
    }
    for (t->i++;;) {
        if (t->i >= t->n || t->s[t->i] == '\n')
            fail_buf(t, &b, "unterminated string");
        if (t->s[t->i] == '"') {
            t->i++;
            return take(t, &b);
        }
        if (t->s[t->i] == '\\')
            escape(t, &b);
        else
            buf_addc(&b, t->s[t->i++]);
    }
}
static ObjString *literal_string(Toml *t, bool multi) {
    size_t start;
    if (multi && ahead(t, "'''")) {
        t->i += 3;
        newline(t);
        for (start = t->i; !ahead(t, "'''"); t->i++) {
            if (t->i >= t->n)
                fail(t, "unterminated multi-line string%s", "");
            t->line += t->s[t->i] == '\n';
        }
        while (t->i + 3 < t->n && t->s[t->i + 3] == '\'')
            t->i++;
        t->i += 3;
        return str_copy(t->vm, t->s + start, t->i - 3 - start);
    }
    for (start = ++t->i; !at(t, '\''); t->i++)
        if (t->i >= t->n || t->s[t->i] == '\n')
            fail(t, "unterminated string%s", "");
    return str_copy(t->vm, t->s + start, t->i++ - start);
}
static Path dotted_key(Toml *t) {
    Path p;
    p.n = 0;
    for (;;) {
        spaces(t);
        ObjString *k;
        if (at(t, '"'))
            k = basic_string(t, false);
        else if (at(t, '\''))
            k = literal_string(t, false);
        else {
            size_t start = t->i;
            while (t->i < t->n && (isalnum((unsigned char)t->s[t->i]) || at(t, '_') || at(t, '-')))
                t->i++;
            if (start == t->i)
                fail(t, "expected a key%s", "");
            k = str_copy(t->vm, t->s + start, t->i - start);
        }
        if (p.n == 64)
            fail(t, "key too deep%s", "");
        p.parts[p.n++] = k;
        spaces(t);
        if (!at(t, '.'))
            return p;
        t->i++;
    }
}
static const char *digits(const char *p, int n) {
    for (; p && n; n--, p++)
        if (!isdigit((unsigned char)*p))
            return NULL;
    return p;
}
/* hh:mm(:ss(.f+)?)? ; the end, or NULL */
static const char *clock_text(const char *p) {
    p = digits(p, 2);
    if (!p || *p != ':' || !(p = digits(p + 1, 2)))
        return NULL;
    if (*p != ':')
        return p;
    if (!(p = digits(p + 1, 2)))
        return NULL;
    if (*p == '.' && isdigit((unsigned char)p[1]))
        for (p++; isdigit((unsigned char)*p);)
            p++;
    return p;
}
static bool is_date(const char *s) {
    const char *p = clock_text(s);
    if (p && !*p)
        return true;
    p = digits(s, 4);
    if (!p || *p != '-' || !(p = digits(p + 1, 2)) || *p != '-' || !(p = digits(p + 1, 2)))
        return false;
    if (!*p)
        return true;
    if ((*p != 'T' && *p != 't' && *p != ' ') || !(p = clock_text(p + 1)))
        return false;
    if (*p == 'Z' || *p == 'z')
        p++;
    else if (*p == '+' || *p == '-') {
        p = digits(p + 1, 2);
        if (!p || *p != ':' || !(p = digits(p + 1, 2)))
            return false;
    }
    return !*p;
}
/* [+-]?(0|[1-9]\d*) optionally followed by a fraction and an exponent: 1 integer, 2 real, 0 not. */
static int number_kind(const char *p) {
    if (*p == '+' || *p == '-')
        p++;
    if (!isdigit((unsigned char)*p) || (*p == '0' && isdigit((unsigned char)p[1])))
        return 0;
    while (isdigit((unsigned char)*p))
        p++;
    if (!*p)
        return 1;
    if (*p == '.') {
        if (!isdigit((unsigned char)*++p))
            return 0;
        while (isdigit((unsigned char)*p))
            p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-')
            p++;
        if (!isdigit((unsigned char)*p))
            return 0;
        while (isdigit((unsigned char)*p))
            p++;
    }
    return *p ? 0 : 2;
}
static Value scalar(Toml *t) {
    size_t start = t->i;
    while (t->i < t->n && !strchr(",]}\n#\r", t->s[t->i]))
        t->i++;
    size_t len = t->i - start;
    while (len && (t->s[start + len - 1] == ' ' || t->s[start + len - 1] == '\t'))
        len--;
    char tok[200], d[200];
    if (len >= sizeof tok)
        fail(t, "invalid value%s", "");
    memcpy(tok, t->s + start, len);
    tok[len] = 0;
    if (!strcmp(tok, "true") || !strcmp(tok, "false"))
        return val_bool(tok[0] == 't');
    const char *word = tok + (tok[0] == '+' || tok[0] == '-');
    if (!strcmp(word, "inf") || !strcmp(word, "nan"))
        fail(t, "JSON values cannot hold inf or nan ('%s')", tok);
    if (is_date(tok))
        return val_obj(str_copy(t->vm, tok, len));
    size_t n = 0;
    for (size_t k = 0; k < len; k++) {
        if (tok[k] != '_')
            d[n++] = tok[k];
        else if (!k || k + 1 == len || !isalnum((unsigned char)tok[k - 1]) || !isalnum((unsigned char)tok[k + 1]))
            fail(t, "misplaced '_' in number '%s'", tok);
    }
    d[n] = 0;
    if (n > 2 && d[0] == '0' && strchr("xob", d[1])) {
        uint64_t base = d[1] == 'x' ? 16 : d[1] == 'o' ? 8 : 2, v = 0;
        for (size_t k = 2; k < n; k++) {
            unsigned char c = (unsigned char)tolower((unsigned char)d[k]);
            uint64_t x = isdigit(c) ? (uint64_t)(c - '0') : c >= 'a' && c <= 'f' ? (uint64_t)(c - 'a' + 10) : 99;
            if (x >= base || v > ((uint64_t)INT64_MAX - x) / base)
                fail(t, "invalid integer '%s'", tok);
            v = v * base + x;
        }
        return val_int((int64_t)v);
    }
    int kind = number_kind(d);
    if (!kind)
        fail(t, len ? "invalid value '%s'" : "expected a value%s", tok);
    Value out;
    /* The JSON reader keeps the number rules (range, formatting) in one place. */
    const char *num = d[0] == '+' ? d + 1 : d;
    if (!lib_json_parse(t->vm, num, strlen(num), &out)) {
        t->vm->has_error = false;
        fail(t, "integer out of range '%s'", tok);
    }
    if (IS_OBJT(out, OBJ_JSON))
        out = ((ObjJson *)AS_OBJ(out))->v;
    if (kind == 1 && !IS_INT(out))
        fail(t, "integer out of range '%s'", tok);
    return out;
}
static bool listed(ObjMap **list, int n, ObjMap *m) {
    for (int i = 0; i < n; i++)
        if (list[i] == m)
            return true;
    return false;
}
static bool lookup(Toml *t, ObjMap *m, ObjString *k, Value *out) {
    bool found = false;
    map_get(t->vm, m, val_obj(k), out, &found);
    return found;
}
static ObjMap *descend(Toml *t, ObjMap *table, ObjString *name, bool dotted) {
    Value v;
    if (!lookup(t, table, name, &v)) {
        v = val_obj(new_map(t->vm, OBJ_MAP));
        map_set(t->vm, table, val_obj(name), v);
    }
    if (IS_ARRAY(v) && !dotted && AS_LIST(v)->count && IS_MAP(AS_LIST(v)->items[AS_LIST(v)->count - 1]))
        return AS_MAP(AS_LIST(v)->items[AS_LIST(v)->count - 1]); /* [a.b] inside the latest [[a]] */
    if (!IS_MAP(v) || listed(t->sealed, t->nsealed, AS_MAP(v)))
        fail(t, "key '%s' is already defined", name->chars);
    return AS_MAP(v);
}
static void assign(Toml *t, ObjMap *table, Path *p, Value v) {
    for (int k = 0; k + 1 < p->n; k++)
        table = descend(t, table, p->parts[k], true);
    Value old;
    if (lookup(t, table, p->parts[p->n - 1], &old))
        fail(t, "key '%s' is already defined", p->parts[p->n - 1]->chars);
    map_set(t->vm, table, val_obj(p->parts[p->n - 1]), v);
}
static Value value(Toml *t, int depth) {
    if (depth > 200)
        fail(t, "value nested too deeply%s", "");
    spaces(t);
    if (at(t, '"'))
        return val_obj(basic_string(t, true));
    if (at(t, '\''))
        return val_obj(literal_string(t, true));
    if (at(t, '[')) {
        ObjList *list = new_list(t->vm, OBJ_ARRAY, 4);
        for (t->i++;;) {
            blank(t);
            if (at(t, ']'))
                break;
            list_push(t->vm, list, value(t, depth + 1));
            blank(t);
            if (at(t, ','))
                t->i++;
            else if (!at(t, ']'))
                fail(t, "expected ',' or ']' in array%s", "");
        }
        t->i++;
        return val_obj(list);
    }
    if (!at(t, '{'))
        return scalar(t);
    ObjMap *table = new_map(t->vm, OBJ_MAP);
    t->i++;
    spaces(t);
    while (!at(t, '}')) {
        Path p = dotted_key(t);
        if (!at(t, '='))
            fail(t, "expected '=' in inline table%s", "");
        t->i++;
        assign(t, table, &p, value(t, depth + 1));
        spaces(t);
        if (at(t, '}'))
            break;
        if (!at(t, ','))
            fail(t, "expected ',' or '}' in inline table%s", "");
        t->i++;
        spaces(t);
        if (at(t, '}'))
            fail(t, "expected a key%s", "");
    }
    t->i++;
    t->sealed = xrealloc(t->sealed, sizeof(ObjMap *) * (size_t)(t->nsealed + 1));
    t->sealed[t->nsealed++] = table;
    return val_obj(table);
}
static void document(Toml *t, ObjMap *root) {
    ObjMap *current = root;
    for (;;) {
        blank(t);
        if (t->i >= t->n)
            return;
        bool header = at(t, '['), array = ahead(t, "[[");
        if (header)
            t->i += array ? 2 : 1;
        Path p = dotted_key(t);
        if (!header) {
            if (!at(t, '='))
                fail(t, "expected '=' after key%s", "");
            t->i++;
            assign(t, current, &p, value(t, 0));
            end_of_line(t);
            continue;
        }
        if (!ahead(t, array ? "]]" : "]"))
            fail(t, "expected ']' after table name%s", "");
        t->i += array ? 2 : 1;
        ObjMap *table = root;
        for (int k = 0; k + 1 < p.n; k++)
            table = descend(t, table, p.parts[k], false);
        ObjString *last = p.parts[p.n - 1];
        Value slot;
        if (!lookup(t, table, last, &slot)) {
            slot = array ? val_obj(new_list(t->vm, OBJ_ARRAY, 4)) : val_obj(new_map(t->vm, OBJ_MAP));
            map_set(t->vm, table, val_obj(last), slot);
        }
        if (array) {
            if (!IS_ARRAY(slot))
                fail(t, "'%s' is not an array of tables", last->chars);
            current = new_map(t->vm, OBJ_MAP);
            list_push(t->vm, AS_LIST(slot), val_obj(current));
        } else {
            if (!IS_MAP(slot) || listed(t->defined, t->ndefined, AS_MAP(slot)) ||
                listed(t->sealed, t->nsealed, AS_MAP(slot)))
                fail(t, "table '%s' is already defined", last->chars);
            current = AS_MAP(slot);
            t->defined = xrealloc(t->defined, sizeof(ObjMap *) * (size_t)(t->ndefined + 1));
            t->defined[t->ndefined++] = current;
        }
        end_of_line(t);
    }
}

bool lib_toml_parse(VM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    if (!IS_STRING(args[0]))
        return vm_throw(vm, "TypeError", "toml.parse expects a string");
    Toml *t = xmalloc(sizeof *t);
    memset(t, 0, sizeof *t);
    t->vm = vm;
    t->s = AS_CSTR(args[0]);
    t->n = (size_t)AS_STRING(args[0])->len;
    t->line = 1;
    vm->gc_off++; /* the tree under construction is not rooted */
    int off = vm->gc_off;
    ObjMap *root = new_map(vm, OBJ_MAP);
    bool ok = !setjmp(t->jump);
    if (ok)
        document(t, root);
    vm->gc_off = off;
    if (ok)
        *out = val_obj(new_json(vm, val_obj(root)));
    vm->gc_off--;
    if (!ok)
        vm_throw(vm, "ValueError", "%s", t->message);
    free(t->defined);
    free(t->sealed);
    free(t);
    return ok;
}
