/* HTML tools: element methods and properties, CSS selectors, queries, mutation, and a
 * lenient HTML parser. */
#include "turbine.h"
#include <ctype.h>

#define NATIVE(name) static bool name(VM *vm, int argc, Value *args, Value *out)
static Value sv(VM *vm, const char *s, size_t n) { return val_obj(str_copy(vm, s, n)); }
static Value svc(VM *vm, const char *s) { return val_obj(str_copy(vm, s, strlen(s))); }
static ObjElement *E(Value v) { return AS_ELEMENT(v); }
static int strcasecmp_ascii(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x)
            return x - y;
    }
}

/* ---------------------------------------------------------------- tree helpers */

typedef struct {
    ObjElement **items;
    int count, cap;
} Nodes;
static void nodes_push(Nodes *n, ObjElement *e) {
    if (n->count == n->cap) {
        n->cap = GROW_CAP(n->cap);
        n->items = xrealloc(n->items, sizeof(ObjElement *) * (size_t)n->cap);
    }
    n->items[n->count++] = e;
}
/* Named children, looking through nested fragments. */
static void logical_children(ObjElement *e, Nodes *out, int depth) {
    if (depth > 256)
        return;
    for (int i = 0; i < e->children.count; i++) {
        Value c = e->children.items[i];
        if (!IS_OBJT(c, OBJ_ELEMENT))
            continue;
        if (AS_ELEMENT(c)->tag)
            nodes_push(out, AS_ELEMENT(c));
        else
            logical_children(AS_ELEMENT(c), out, depth + 1);
    }
}
static bool child_of(ObjElement *parent, ObjElement *child) {
    for (int i = 0; i < parent->children.count; i++)
        if (IS_OBJ(parent->children.items[i]) && AS_OBJ(parent->children.items[i]) == (Obj *)child)
            return true;
    return false;
}
/* The live parent link: the parent the node was last added to, while it is still there. */
static ObjElement *parent_of(ObjElement *e) {
    ObjElement *p = e->parent;
    if (p && !child_of(p, e))
        return NULL;
    return p;
}
/* Nearest named ancestor (fragments are transparent), or the topmost fragment. */
static ObjElement *container_of(ObjElement *e) {
    ObjElement *p = parent_of(e);
    while (p && !p->tag) {
        ObjElement *up = parent_of(p);
        if (!up)
            return p;
        p = up;
    }
    return p;
}
static ObjElement *named_parent(ObjElement *e) {
    ObjElement *p = container_of(e);
    return p && p->tag ? p : NULL;
}
static void siblings_of(ObjElement *e, Nodes *out) {
    ObjElement *c = container_of(e);
    if (c)
        logical_children(c, out, 0);
}
static bool contains_node(ObjElement *root, ObjElement *target, int depth) {
    if (root == target)
        return true;
    if (depth > 256)
        return false;
    for (int i = 0; i < root->children.count; i++) {
        Value c = root->children.items[i];
        if (IS_OBJT(c, OBJ_ELEMENT) && contains_node(AS_ELEMENT(c), target, depth + 1))
            return true;
    }
    return false;
}
static int attr_index(ObjElement *e, const char *name) {
    for (int i = 0; i < e->attr_names.count; i++)
        if (!strcasecmp_ascii(AS_CSTR(e->attr_names.items[i]), name))
            return i;
    return -1;
}
static bool attr_text(VM *vm, ObjElement *e, int i, ObjString **out) {
    Value v = e->attr_values.items[i];
    if (IS_STRING(v)) {
        *out = AS_STRING(v);
        return true;
    }
    if (IS_BOOL(v)) {
        *out = str_copy(vm, v.as.b ? "" : "false", v.as.b ? 0 : 5);
        return true;
    }
    *out = value_to_string(vm, v);
    return *out != NULL;
}
/* Attribute present for selector purposes (false booleans and null are absent). */
static bool attr_present(ObjElement *e, int i) {
    Value v = e->attr_values.items[i];
    return !IS_NULL(v) && !(IS_BOOL(v) && !v.as.b);
}

/* ---------------------------------------------------------------- selectors */

typedef enum { SIMPLE_ID, SIMPLE_CLASS, SIMPLE_ATTR, SIMPLE_PSEUDO } SimpleKind;
typedef enum {
    PS_FIRST_CHILD,
    PS_LAST_CHILD,
    PS_ONLY_CHILD,
    PS_NTH_CHILD,
    PS_NTH_LAST_CHILD,
    PS_FIRST_OF_TYPE,
    PS_LAST_OF_TYPE,
    PS_ONLY_OF_TYPE,
    PS_NTH_OF_TYPE,
    PS_NTH_LAST_OF_TYPE,
    PS_EMPTY,
    PS_NOT,
    PS_CHECKED,
    PS_DISABLED,
    PS_ENABLED
} Pseudo;
struct SelList;
typedef struct {
    SimpleKind kind;
    char *name;  /* id, class, attribute name */
    char op;     /* attribute operator: 0 presence, '=', '~', '^', '$', '*', '|' */
    char *value;
    Pseudo pseudo;
    int a, b;    /* an+b */
    struct SelList *inner;
} Simple;
typedef struct {
    char *tag; /* NULL or "*" for any */
    Simple *simples;
    int nsimples;
    char combinator; /* combinator to the previous compound: ' ', '>', '+', '~', or 0 */
} Compound;
typedef struct {
    Compound *parts;
    int nparts;
} Complex;
typedef struct SelList {
    Complex *items;
    int count;
} SelList;

static void free_list(SelList *l);
static void free_compound(Compound *c) {
    free(c->tag);
    for (int i = 0; i < c->nsimples; i++) {
        free(c->simples[i].name);
        free(c->simples[i].value);
        if (c->simples[i].inner) {
            free_list(c->simples[i].inner);
            free(c->simples[i].inner);
        }
    }
    free(c->simples);
}
static void free_list(SelList *l) {
    for (int i = 0; i < l->count; i++) {
        for (int j = 0; j < l->items[i].nparts; j++)
            free_compound(&l->items[i].parts[j]);
        free(l->items[i].parts);
    }
    free(l->items);
    l->items = NULL;
    l->count = 0;
}

typedef struct {
    const char *s;
    size_t p, n;
    bool bad;
    char why[128];
} SP;
static void sp_fail(SP *p, const char *why) {
    if (!p->bad)
        snprintf(p->why, sizeof p->why, "%s", why);
    p->bad = true;
}
static void sp_ws(SP *p) {
    while (p->p < p->n && isspace((unsigned char)p->s[p->p]))
        p->p++;
}
static bool ident_char(char c) { return isalnum((unsigned char)c) || c == '-' || c == '_' || (unsigned char)c >= 0x80; }
static char *sp_ident(SP *p) {
    size_t b = p->p;
    while (p->p < p->n && ident_char(p->s[p->p]))
        p->p++;
    if (b == p->p) {
        sp_fail(p, "expected a name");
        return NULL;
    }
    return xstrndup(p->s + b, p->p - b);
}
static bool parse_list(SP *p, SelList *out, char end);
static bool parse_anb(SP *p, int *a, int *b) {
    sp_ws(p);
    size_t start = p->p;
    while (p->p < p->n && p->s[p->p] != ')')
        p->p++;
    char buf[64];
    size_t len = p->p - start;
    if (len == 0 || len >= sizeof buf) {
        sp_fail(p, "invalid an+b");
        return false;
    }
    size_t w = 0;
    for (size_t i = 0; i < len; i++)
        if (!isspace((unsigned char)p->s[start + i]))
            buf[w++] = (char)tolower((unsigned char)p->s[start + i]);
    buf[w] = 0;
    if (!strcmp(buf, "odd")) {
        *a = 2;
        *b = 1;
        return true;
    }
    if (!strcmp(buf, "even")) {
        *a = 2;
        *b = 0;
        return true;
    }
    char *n = strchr(buf, 'n');
    if (!n) {
        char *end;
        long v = strtol(buf, &end, 10);
        if (*end || end == buf) {
            sp_fail(p, "invalid an+b");
            return false;
        }
        *a = 0;
        *b = (int)v;
        return true;
    }
    *n = 0;
    if (!buf[0] || !strcmp(buf, "+"))
        *a = 1;
    else if (!strcmp(buf, "-"))
        *a = -1;
    else {
        char *end;
        long v = strtol(buf, &end, 10);
        if (*end) {
            sp_fail(p, "invalid an+b");
            return false;
        }
        *a = (int)v;
    }
    const char *rest = n + 1;
    if (!*rest)
        *b = 0;
    else {
        char *end;
        long v = strtol(rest, &end, 10);
        if (*end || (rest[0] != '+' && rest[0] != '-')) {
            sp_fail(p, "invalid an+b");
            return false;
        }
        *b = (int)v;
    }
    return true;
}
static bool parse_compound(SP *p, Compound *c) {
    memset(c, 0, sizeof *c);
    int cap = 0;
    bool any = false;
    if (p->p < p->n && p->s[p->p] == '*') {
        c->tag = xstrdup("*");
        p->p++;
        any = true;
    } else if (p->p < p->n && ident_char(p->s[p->p])) {
        c->tag = sp_ident(p);
        for (char *q = c->tag; q && *q; q++)
            *q = (char)tolower((unsigned char)*q);
        any = true;
    }
    for (;;) {
        if (p->p >= p->n)
            break;
        char ch = p->s[p->p];
        if (ch != '#' && ch != '.' && ch != '[' && ch != ':')
            break;
        if (c->nsimples == cap) {
            cap = GROW_CAP(cap);
            c->simples = xrealloc(c->simples, sizeof(Simple) * (size_t)cap);
        }
        Simple *s = &c->simples[c->nsimples];
        memset(s, 0, sizeof *s);
        p->p++;
        if (ch == '#' || ch == '.') {
            s->kind = ch == '#' ? SIMPLE_ID : SIMPLE_CLASS;
            s->name = sp_ident(p);
            if (!s->name)
                return false;
        } else if (ch == '[') {
            s->kind = SIMPLE_ATTR;
            sp_ws(p);
            s->name = sp_ident(p);
            if (!s->name)
                return false;
            for (char *q = s->name; *q; q++)
                *q = (char)tolower((unsigned char)*q);
            sp_ws(p);
            if (p->p < p->n && p->s[p->p] == ']') {
                p->p++;
            } else {
                char op = p->p < p->n ? p->s[p->p] : 0;
                if (op == '=') {
                    s->op = '=';
                    p->p++;
                } else if (op && strchr("~^$*|", op) && p->p + 1 < p->n && p->s[p->p + 1] == '=') {
                    s->op = op;
                    p->p += 2;
                } else {
                    sp_fail(p, "invalid attribute selector");
                    return false;
                }
                sp_ws(p);
                if (p->p < p->n && (p->s[p->p] == '"' || p->s[p->p] == '\'')) {
                    char q = p->s[p->p++];
                    Buf b;
                    buf_init(&b);
                    while (p->p < p->n && p->s[p->p] != q) {
                        if (p->s[p->p] == '\\' && p->p + 1 < p->n)
                            p->p++;
                        buf_addc(&b, p->s[p->p++]);
                    }
                    if (p->p >= p->n) {
                        buf_free(&b);
                        sp_fail(p, "unterminated attribute value");
                        return false;
                    }
                    p->p++;
                    s->value = b.data ? b.data : xstrdup("");
                } else {
                    s->value = sp_ident(p);
                    if (!s->value)
                        return false;
                }
                sp_ws(p);
                if (p->p >= p->n || p->s[p->p] != ']') {
                    sp_fail(p, "expected ']'");
                    return false;
                }
                p->p++;
            }
        } else {
            s->kind = SIMPLE_PSEUDO;
            if (p->p < p->n && p->s[p->p] == ':') {
                sp_fail(p, "pseudo-elements are not supported");
                return false;
            }
            char *name = sp_ident(p);
            if (!name)
                return false;
            for (char *q = name; *q; q++)
                *q = (char)tolower((unsigned char)*q);
            static const struct {
                const char *n;
                Pseudo p;
                int arg; /* 0 none, 1 an+b, 2 selector list */
            } ps[] = {{"first-child", PS_FIRST_CHILD, 0},       {"last-child", PS_LAST_CHILD, 0},
                      {"only-child", PS_ONLY_CHILD, 0},         {"nth-child", PS_NTH_CHILD, 1},
                      {"nth-last-child", PS_NTH_LAST_CHILD, 1}, {"first-of-type", PS_FIRST_OF_TYPE, 0},
                      {"last-of-type", PS_LAST_OF_TYPE, 0},     {"only-of-type", PS_ONLY_OF_TYPE, 0},
                      {"nth-of-type", PS_NTH_OF_TYPE, 1},       {"nth-last-of-type", PS_NTH_LAST_OF_TYPE, 1},
                      {"empty", PS_EMPTY, 0},                   {"not", PS_NOT, 2},
                      {"checked", PS_CHECKED, 0},               {"disabled", PS_DISABLED, 0},
                      {"enabled", PS_ENABLED, 0},               {NULL, 0, 0}};
            int found = -1;
            for (int i = 0; ps[i].n; i++)
                if (!strcmp(ps[i].n, name))
                    found = i;
            if (found < 0) {
                char msg[128];
                snprintf(msg, sizeof msg, "unsupported pseudo-class ':%s'", name);
                free(name);
                sp_fail(p, msg);
                return false;
            }
            free(name);
            s->pseudo = ps[found].p;
            if (ps[found].arg) {
                if (p->p >= p->n || p->s[p->p] != '(') {
                    sp_fail(p, "expected '('");
                    return false;
                }
                p->p++;
                if (ps[found].arg == 1) {
                    if (!parse_anb(p, &s->a, &s->b))
                        return false;
                } else {
                    s->inner = xmalloc(sizeof(SelList));
                    memset(s->inner, 0, sizeof(SelList));
                    if (!parse_list(p, s->inner, ')'))
                        return false;
                }
                sp_ws(p);
                if (p->p >= p->n || p->s[p->p] != ')') {
                    sp_fail(p, "expected ')'");
                    return false;
                }
                p->p++;
            }
        }
        c->nsimples++;
        any = true;
    }
    if (!any)
        sp_fail(p, "expected a selector");
    return any;
}
static bool parse_complex(SP *p, Complex *out, char end) {
    memset(out, 0, sizeof *out);
    int cap = 0;
    char comb = 0;
    for (;;) {
        sp_ws(p);
        if (out->nparts == cap) {
            cap = GROW_CAP(cap);
            out->parts = xrealloc(out->parts, sizeof(Compound) * (size_t)cap);
        }
        if (!parse_compound(p, &out->parts[out->nparts]))
            return false;
        out->parts[out->nparts].combinator = out->nparts ? comb : 0;
        out->nparts++;
        size_t before = p->p;
        sp_ws(p);
        if (p->p >= p->n || p->s[p->p] == ',' || p->s[p->p] == end)
            return true;
        char c = p->s[p->p];
        if (c == '>' || c == '+' || c == '~') {
            comb = c;
            p->p++;
        } else if (p->p > before)
            comb = ' ';
        else {
            sp_fail(p, "unexpected character in selector");
            return false;
        }
    }
}
static bool parse_list(SP *p, SelList *out, char end) {
    int cap = 0;
    for (;;) {
        if (out->count == cap) {
            cap = GROW_CAP(cap);
            out->items = xrealloc(out->items, sizeof(Complex) * (size_t)cap);
        }
        if (!parse_complex(p, &out->items[out->count], end)) {
            out->count++;
            return false;
        }
        out->count++;
        sp_ws(p);
        if (p->p < p->n && p->s[p->p] == ',') {
            p->p++;
            continue;
        }
        return true;
    }
}
static bool compile_selector(VM *vm, Value text, SelList *out) {
    memset(out, 0, sizeof *out);
    if (!IS_STRING(text))
        return vm_throw(vm, "TypeError", "selectors are strings");
    SP p = {AS_CSTR(text), 0, (size_t)AS_STRING(text)->len, false, ""};
    bool ok = parse_list(&p, out, 0);
    sp_ws(&p);
    if (ok && p.p != p.n)
        sp_fail(&p, "unexpected text after the selector");
    if (!ok || p.bad) {
        free_list(out);
        return vm_throw(vm, "SelectorError", "invalid selector \"%s\": %s", AS_CSTR(text), p.why[0] ? p.why : "syntax error");
    }
    return true;
}

static bool nth_match(int a, int b, int index /* 1-based */) {
    if (a == 0)
        return index == b;
    int d = index - b;
    return d % a == 0 && d / a >= 0;
}
static bool tag_equal(const char *a, const char *b) { return !strcasecmp_ascii(a, b); }
static bool match_list(VM *vm, ObjElement *e, SelList *l, int depth);
static bool match_compound(VM *vm, ObjElement *e, Compound *c, int depth) {
    if (!e->tag)
        return false;
    if (c->tag && strcmp(c->tag, "*") && !tag_equal(c->tag, e->tag->chars))
        return false;
    for (int i = 0; i < c->nsimples; i++) {
        Simple *s = &c->simples[i];
        switch (s->kind) {
        case SIMPLE_ID: {
            int k = attr_index(e, "id");
            ObjString *v;
            if (k < 0 || !attr_text(vm, e, k, &v) || strcmp(v->chars, s->name))
                return false;
            break;
        }
        case SIMPLE_CLASS: {
            int k = attr_index(e, "class");
            ObjString *v;
            if (k < 0 || !attr_text(vm, e, k, &v))
                return false;
            bool found = false;
            size_t nl = strlen(s->name);
            for (const char *q = v->chars; *q && !found;) {
                while (*q && isspace((unsigned char)*q))
                    q++;
                const char *st = q;
                while (*q && !isspace((unsigned char)*q))
                    q++;
                if ((size_t)(q - st) == nl && !memcmp(st, s->name, nl))
                    found = true;
            }
            if (!found)
                return false;
            break;
        }
        case SIMPLE_ATTR: {
            int k = attr_index(e, s->name);
            if (k < 0 || !attr_present(e, k))
                return false;
            if (!s->op)
                break;
            ObjString *v;
            if (!attr_text(vm, e, k, &v))
                return false;
            const char *val = v->chars, *want = s->value;
            size_t vl = strlen(val), wl = strlen(want);
            bool ok;
            switch (s->op) {
            case '=': ok = !strcmp(val, want); break;
            case '^': ok = wl && !strncmp(val, want, wl); break;
            case '$': ok = wl && vl >= wl && !strcmp(val + vl - wl, want); break;
            case '*': ok = wl && strstr(val, want) != NULL; break;
            case '|': ok = !strcmp(val, want) || (!strncmp(val, want, wl) && val[wl] == '-'); break;
            default: { /* ~= */
                ok = false;
                for (const char *q = val; *q && !ok;) {
                    while (*q && isspace((unsigned char)*q))
                        q++;
                    const char *st = q;
                    while (*q && !isspace((unsigned char)*q))
                        q++;
                    if (wl && (size_t)(q - st) == wl && !memcmp(st, want, wl))
                        ok = true;
                }
            }
            }
            if (!ok)
                return false;
            break;
        }
        case SIMPLE_PSEUDO: {
            if (s->pseudo == PS_NOT) {
                if (match_list(vm, e, s->inner, depth + 1))
                    return false;
                break;
            }
            if (s->pseudo == PS_EMPTY) {
                for (int k = 0; k < e->children.count; k++) {
                    Value ch = e->children.items[k];
                    if (IS_OBJT(ch, OBJ_ELEMENT) || (IS_STRING(ch) && AS_STRING(ch)->len))
                        return false;
                }
                break;
            }
            if (s->pseudo == PS_CHECKED || s->pseudo == PS_DISABLED || s->pseudo == PS_ENABLED) {
                const char *an = s->pseudo == PS_CHECKED ? "checked" : "disabled";
                int k = attr_index(e, an);
                bool has = k >= 0 && attr_present(e, k);
                if (s->pseudo == PS_CHECKED && k < 0) {
                    int sel = attr_index(e, "selected");
                    has = sel >= 0 && attr_present(e, sel);
                }
                bool formish = tag_equal(e->tag->chars, "input") || tag_equal(e->tag->chars, "button") ||
                               tag_equal(e->tag->chars, "select") || tag_equal(e->tag->chars, "textarea") ||
                               tag_equal(e->tag->chars, "option") || tag_equal(e->tag->chars, "fieldset");
                if (s->pseudo == PS_ENABLED ? !(formish && !has) : !has)
                    return false;
                break;
            }
            Nodes sib = {0};
            siblings_of(e, &sib);
            int index = 0, total = 0, tindex = 0, ttotal = 0;
            for (int k = 0; k < sib.count; k++) {
                bool same = tag_equal(sib.items[k]->tag->chars, e->tag->chars);
                total++;
                if (same)
                    ttotal++;
                if (sib.items[k] == e) {
                    index = total;
                    tindex = ttotal;
                }
            }
            free(sib.items);
            if (!index) {
                index = total = tindex = ttotal = 1;
            }
            bool ok;
            switch (s->pseudo) {
            case PS_FIRST_CHILD: ok = index == 1; break;
            case PS_LAST_CHILD: ok = index == total; break;
            case PS_ONLY_CHILD: ok = total == 1; break;
            case PS_NTH_CHILD: ok = nth_match(s->a, s->b, index); break;
            case PS_NTH_LAST_CHILD: ok = nth_match(s->a, s->b, total - index + 1); break;
            case PS_FIRST_OF_TYPE: ok = tindex == 1; break;
            case PS_LAST_OF_TYPE: ok = tindex == ttotal; break;
            case PS_ONLY_OF_TYPE: ok = ttotal == 1; break;
            case PS_NTH_OF_TYPE: ok = nth_match(s->a, s->b, tindex); break;
            case PS_NTH_LAST_OF_TYPE: ok = nth_match(s->a, s->b, ttotal - tindex + 1); break;
            default: ok = false;
            }
            if (!ok)
                return false;
            break;
        }
        }
    }
    return true;
}
static bool match_from(VM *vm, ObjElement *e, Complex *c, int idx, int depth) {
    if (depth > 256)
        return false;
    if (!match_compound(vm, e, &c->parts[idx], depth))
        return false;
    if (idx == 0)
        return true;
    char comb = c->parts[idx].combinator;
    if (comb == '>') {
        ObjElement *p = named_parent(e);
        return p && match_from(vm, p, c, idx - 1, depth + 1);
    }
    if (comb == ' ') {
        for (ObjElement *p = named_parent(e); p; p = named_parent(p))
            if (match_from(vm, p, c, idx - 1, depth + 1))
                return true;
        return false;
    }
    Nodes sib = {0};
    siblings_of(e, &sib);
    int pos = -1;
    for (int k = 0; k < sib.count; k++)
        if (sib.items[k] == e)
            pos = k;
    bool r = false;
    if (comb == '+')
        r = pos > 0 && match_from(vm, sib.items[pos - 1], c, idx - 1, depth + 1);
    else
        for (int k = pos - 1; k >= 0 && !r; k--)
            r = match_from(vm, sib.items[k], c, idx - 1, depth + 1);
    free(sib.items);
    return r;
}
static bool match_list(VM *vm, ObjElement *e, SelList *l, int depth) {
    for (int i = 0; i < l->count; i++)
        if (match_from(vm, e, &l->items[i], l->items[i].nparts - 1, depth))
            return true;
    return false;
}

/* Preorder descendants (excluding the root), each identity once. */
static void descendants(ObjElement *e, Nodes *out, int depth, int *visited) {
    if (depth > 256 || *visited > 1000000)
        return;
    for (int i = 0; i < e->children.count; i++) {
        Value c = e->children.items[i];
        if (!IS_OBJT(c, OBJ_ELEMENT))
            continue;
        ObjElement *ce = AS_ELEMENT(c);
        (*visited)++;
        if (ce->tag) {
            bool seen = false;
            for (int k = 0; k < out->count && !seen; k++)
                seen = out->items[k] == ce;
            if (!seen)
                nodes_push(out, ce);
        }
        descendants(ce, out, depth + 1, visited);
    }
}

/* ---------------------------------------------------------------- queries */

static bool want_element(VM *vm, Value v, const char *fn) {
    if (IS_OBJT(v, OBJ_ELEMENT))
        return true;
    if (IS_NULL(v))
        return vm_throw(vm, "CastError", "cannot cast null to element");
    return vm_throw(vm, "TypeError", "%s expects an element", fn);
}
static bool query_impl(VM *vm, Value root, Value sel, bool all, Value *out) {
    if (!want_element(vm, root, all ? "query_all" : "query"))
        return false;
    SelList l;
    if (!compile_selector(vm, sel, &l))
        return false;
    Nodes d = {0};
    int visited = 0;
    descendants(E(root), &d, 0, &visited);
    ObjList *res = all ? new_list(vm, OBJ_ARRAY, 0) : NULL;
    if (res)
        push_root(vm, val_obj(res));
    *out = val_null();
    for (int i = 0; i < d.count; i++)
        if (match_list(vm, d.items[i], &l, 0)) {
            if (!all) {
                *out = val_obj(d.items[i]);
                break;
            }
            list_push(vm, res, val_obj(d.items[i]));
        }
    if (res) {
        pop_root(vm, 1);
        *out = val_obj(res);
    }
    free(d.items);
    free_list(&l);
    return true;
}
NATIVE(el_query) {
    (void)argc;
    return query_impl(vm, args[0], args[1], false, out);
}
NATIVE(el_query_all) {
    (void)argc;
    return query_impl(vm, args[0], args[1], true, out);
}
NATIVE(el_get_by_id) {
    (void)argc;
    if (!want_element(vm, args[0], "get_by_id"))
        return false;
    if (!IS_STRING(args[1]))
        return vm_throw(vm, "TypeError", "get_by_id expects a string");
    Nodes d = {0};
    int visited = 0;
    descendants(E(args[0]), &d, 0, &visited);
    *out = val_null();
    for (int i = 0; i < d.count; i++) {
        int k = attr_index(d.items[i], "id");
        ObjString *v;
        if (k >= 0 && attr_text(vm, d.items[i], k, &v) && !strcmp(v->chars, AS_CSTR(args[1]))) {
            *out = val_obj(d.items[i]);
            break;
        }
    }
    free(d.items);
    return true;
}
NATIVE(el_matches) {
    (void)argc;
    if (!want_element(vm, args[0], "matches"))
        return false;
    SelList l;
    if (!compile_selector(vm, args[1], &l))
        return false;
    *out = val_bool(E(args[0])->tag && match_list(vm, E(args[0]), &l, 0));
    free_list(&l);
    return true;
}
NATIVE(el_closest) {
    (void)argc;
    if (!want_element(vm, args[0], "closest"))
        return false;
    SelList l;
    if (!compile_selector(vm, args[1], &l))
        return false;
    *out = val_null();
    for (ObjElement *e = E(args[0]); e; e = e->tag ? named_parent(e) : container_of(e))
        if (e->tag && match_list(vm, e, &l, 0)) {
            *out = val_obj(e);
            break;
        }
    free_list(&l);
    return true;
}

/* ---------------------------------------------------------------- text, attributes, classes */

static void text_of(ObjElement *e, Buf *b, int depth) {
    if (depth > 256)
        return;
    for (int i = 0; i < e->children.count; i++) {
        Value c = e->children.items[i];
        if (IS_STRING(c))
            buf_add(b, AS_CSTR(c), (size_t)AS_STRING(c)->len);
        else if (IS_OBJT(c, OBJ_ELEMENT))
            text_of(AS_ELEMENT(c), b, depth + 1);
    }
}
static Value text_value(VM *vm, ObjElement *e) {
    Buf b;
    buf_init(&b);
    text_of(e, &b, 0);
    Value v = sv(vm, b.data ? b.data : "", b.len);
    buf_free(&b);
    return v;
}
static bool void_tag(ObjElement *e) {
    static const char *tags[] = {"area", "base", "br", "col", "embed", "hr", "img", "input",
                                 "link", "meta", "param", "source", "track", "wbr", NULL};
    if (!e->tag)
        return false;
    for (int i = 0; tags[i]; i++)
        if (tag_equal(tags[i], e->tag->chars))
            return true;
    return false;
}
static void detach_children(ObjElement *e) {
    for (int i = 0; i < e->children.count; i++) {
        Value c = e->children.items[i];
        if (IS_OBJT(c, OBJ_ELEMENT) && AS_ELEMENT(c)->parent == e)
            AS_ELEMENT(c)->parent = NULL;
    }
    e->children.count = 0;
}
static bool set_text(VM *vm, ObjElement *e, Value text) {
    if (void_tag(e))
        return vm_throw(vm, "RuntimeError", "void element <%s> cannot have text", e->tag->chars);
    ObjString *s = value_to_string(vm, text);
    if (!s)
        return false;
    detach_children(e);
    if (s->len)
        va_push(vm, &e->children, val_obj(s));
    return true;
}
NATIVE(el_get_text) {
    (void)argc;
    *out = text_value(vm, E(args[0]));
    return true;
}
NATIVE(el_set_text) {
    (void)argc;
    *out = val_null();
    return set_text(vm, E(args[0]), args[1]);
}
static bool named_only(VM *vm, ObjElement *e, const char *what) {
    if (!e->tag)
        return vm_throw(vm, "RuntimeError", "fragments have no attributes (%s)", what);
    return true;
}
static bool valid_attr_name(const char *s) {
    if (!*s)
        return false;
    for (const char *q = s; *q; q++) {
        unsigned char c = (unsigned char)*q;
        if (isspace(c) || c == '"' || c == '\'' || c == '>' || c == '/' || c == '=' || c == '<' || c < 0x20)
            return false;
    }
    return true;
}
NATIVE(el_get_attribute) {
    (void)argc;
    ObjElement *e = E(args[0]);
    if (!IS_STRING(args[1]))
        return vm_throw(vm, "TypeError", "attribute names are strings");
    int k = e->tag ? attr_index(e, AS_CSTR(args[1])) : -1;
    if (k < 0 || !attr_present(e, k)) {
        *out = val_null();
        return true;
    }
    ObjString *v;
    if (!attr_text(vm, e, k, &v))
        return false;
    *out = val_obj(v);
    return true;
}
NATIVE(el_has_attribute) {
    (void)argc;
    ObjElement *e = E(args[0]);
    if (!IS_STRING(args[1]))
        return vm_throw(vm, "TypeError", "attribute names are strings");
    int k = e->tag ? attr_index(e, AS_CSTR(args[1])) : -1;
    *out = val_bool(k >= 0 && attr_present(e, k));
    return true;
}
static bool set_attr(VM *vm, ObjElement *e, const char *name, Value v) {
    if (!named_only(vm, e, "set_attribute"))
        return false;
    if (!valid_attr_name(name))
        return vm_throw(vm, "RuntimeError", "invalid attribute name \"%s\"", name);
    int k = attr_index(e, name);
    if (k >= 0) {
        e->attr_values.items[k] = v;
        return true;
    }
    va_push(vm, &e->attr_names, val_obj(str_intern_c(vm, name)));
    va_push(vm, &e->attr_values, v);
    return true;
}
NATIVE(el_set_attribute) {
    (void)argc;
    if (!IS_STRING(args[1]))
        return vm_throw(vm, "TypeError", "attribute names are strings");
    ObjString *v = value_to_string(vm, args[2]);
    if (!v)
        return false;
    *out = val_null();
    return set_attr(vm, E(args[0]), AS_CSTR(args[1]), val_obj(v));
}
static bool remove_attr(ObjElement *e, const char *name) {
    int k = attr_index(e, name);
    if (k < 0)
        return false;
    for (int i = k + 1; i < e->attr_names.count; i++) {
        e->attr_names.items[i - 1] = e->attr_names.items[i];
        e->attr_values.items[i - 1] = e->attr_values.items[i];
    }
    e->attr_names.count--;
    e->attr_values.count--;
    return true;
}
NATIVE(el_remove_attribute) {
    (void)argc;
    if (!named_only(vm, E(args[0]), "remove_attribute"))
        return false;
    if (!IS_STRING(args[1]))
        return vm_throw(vm, "TypeError", "attribute names are strings");
    *out = val_bool(remove_attr(E(args[0]), AS_CSTR(args[1])));
    return true;
}
static bool class_tokens(VM *vm, ObjElement *e, Value token, ObjString **cls, const char **tok) {
    if (!named_only(vm, e, "class helpers"))
        return false;
    if (!IS_STRING(token))
        return vm_throw(vm, "TypeError", "class tokens are strings");
    const char *t = AS_CSTR(token);
    if (!*t)
        return vm_throw(vm, "RuntimeError", "class tokens cannot be empty");
    for (const char *q = t; *q; q++)
        if (isspace((unsigned char)*q))
            return vm_throw(vm, "RuntimeError", "class tokens cannot contain whitespace");
    int k = attr_index(e, "class");
    *cls = NULL;
    if (k >= 0 && !attr_text(vm, e, k, cls))
        return false;
    *tok = t;
    return true;
}
static bool has_token(const char *list, const char *t) {
    size_t tl = strlen(t);
    for (const char *q = list; *q;) {
        while (*q && isspace((unsigned char)*q))
            q++;
        const char *st = q;
        while (*q && !isspace((unsigned char)*q))
            q++;
        if ((size_t)(q - st) == tl && !memcmp(st, t, tl))
            return true;
    }
    return false;
}
static bool write_class(VM *vm, ObjElement *e, const char *list, const char *add, const char *remove) {
    Buf b;
    buf_init(&b);
    for (const char *q = list; *q;) {
        while (*q && isspace((unsigned char)*q))
            q++;
        const char *st = q;
        while (*q && !isspace((unsigned char)*q))
            q++;
        if (q == st)
            break;
        if (remove && (size_t)(q - st) == strlen(remove) && !memcmp(st, remove, (size_t)(q - st)))
            continue;
        if (b.len)
            buf_addc(&b, ' ');
        buf_add(&b, st, (size_t)(q - st));
    }
    if (add) {
        if (b.len)
            buf_addc(&b, ' ');
        buf_adds(&b, add);
    }
    bool ok = set_attr(vm, e, "class", sv(vm, b.data ? b.data : "", b.len));
    buf_free(&b);
    return ok;
}
NATIVE(el_has_class) {
    (void)argc;
    ObjString *cls;
    const char *t;
    if (!class_tokens(vm, E(args[0]), args[1], &cls, &t))
        return false;
    *out = val_bool(cls && has_token(cls->chars, t));
    return true;
}
NATIVE(el_add_class) {
    (void)argc;
    ObjString *cls;
    const char *t;
    if (!class_tokens(vm, E(args[0]), args[1], &cls, &t))
        return false;
    bool had = cls && has_token(cls->chars, t);
    *out = val_bool(!had);
    return had ? true : write_class(vm, E(args[0]), cls ? cls->chars : "", t, NULL);
}
NATIVE(el_remove_class) {
    (void)argc;
    ObjString *cls;
    const char *t;
    if (!class_tokens(vm, E(args[0]), args[1], &cls, &t))
        return false;
    bool had = cls && has_token(cls->chars, t);
    *out = val_bool(had);
    return had ? write_class(vm, E(args[0]), cls->chars, NULL, t) : true;
}
NATIVE(el_toggle_class) {
    (void)argc;
    ObjString *cls;
    const char *t;
    if (!class_tokens(vm, E(args[0]), args[1], &cls, &t))
        return false;
    bool had = cls && has_token(cls->chars, t);
    *out = val_bool(!had);
    return had ? write_class(vm, E(args[0]), cls->chars, NULL, t) : write_class(vm, E(args[0]), cls ? cls->chars : "", t, NULL);
}

/* ---------------------------------------------------------------- children */

static bool insert_child(VM *vm, ObjElement *e, Value v, bool front) {
    if (void_tag(e))
        return vm_throw(vm, "RuntimeError", "void element <%s> cannot have children", e->tag->chars);
    Value item = v;
    if (IS_OBJT(v, OBJ_ELEMENT)) {
        if (contains_node(AS_ELEMENT(v), e, 0))
            return vm_throw(vm, "RuntimeError", "cannot insert a node into itself or its descendants");
    } else if (!IS_STRING(v)) {
        ObjString *s = value_to_string(vm, v);
        if (!s)
            return false;
        item = val_obj(s);
    }
    if (IS_STRING(item) && AS_STRING(item)->len == 0)
        return true;
    va_push(vm, &e->children, item);
    if (front) {
        memmove(e->children.items + 1, e->children.items, sizeof(Value) * (size_t)(e->children.count - 1));
        e->children.items[0] = item;
    }
    if (IS_OBJT(item, OBJ_ELEMENT))
        AS_ELEMENT(item)->parent = e;
    return true;
}
NATIVE(el_append) {
    for (int i = 1; i < argc; i++)
        if (!insert_child(vm, E(args[0]), args[i], false))
            return false;
    *out = val_null();
    return true;
}
NATIVE(el_prepend) {
    for (int i = argc - 1; i >= 1; i--)
        if (!insert_child(vm, E(args[0]), args[i], true))
            return false;
    *out = val_null();
    return true;
}
static bool remove_from(ObjElement *e, Obj *target, int depth) {
    for (int i = 0; i < e->children.count; i++) {
        Value c = e->children.items[i];
        if (IS_OBJ(c) && AS_OBJ(c) == target) {
            memmove(e->children.items + i, e->children.items + i + 1, sizeof(Value) * (size_t)(e->children.count - i - 1));
            e->children.count--;
            if (((ObjElement *)target)->parent == e)
                ((ObjElement *)target)->parent = NULL;
            return true;
        }
        if (depth < 256 && IS_OBJT(c, OBJ_ELEMENT) && !AS_ELEMENT(c)->tag && remove_from(AS_ELEMENT(c), target, depth + 1))
            return true;
    }
    return false;
}
NATIVE(el_remove_child) {
    (void)argc;
    if (!IS_OBJT(args[1], OBJ_ELEMENT)) {
        *out = val_bool(false);
        return true;
    }
    *out = val_bool(remove_from(E(args[0]), AS_OBJ(args[1]), 0));
    return true;
}
NATIVE(el_clear) {
    (void)argc;
    detach_children(E(args[0]));
    *out = val_null();
    return true;
}
static ObjElement *clone_node(VM *vm, ObjElement *e, bool deep, int depth) {
    ObjElement *c = new_element(vm, e->tag);
    push_root(vm, val_obj(c));
    for (int i = 0; i < e->attr_names.count; i++) {
        va_push(vm, &c->attr_names, e->attr_names.items[i]);
        va_push(vm, &c->attr_values, e->attr_values.items[i]);
    }
    if (deep && depth < 256)
        for (int i = 0; i < e->children.count; i++) {
            Value ch = e->children.items[i];
            if (IS_OBJT(ch, OBJ_ELEMENT)) {
                ObjElement *cc = clone_node(vm, AS_ELEMENT(ch), true, depth + 1);
                cc->parent = c;
                va_push(vm, &c->children, val_obj(cc));
            } else
                va_push(vm, &c->children, ch);
        }
    pop_root(vm, 1);
    return c;
}
NATIVE(el_clone) {
    bool deep = argc < 2 || !IS_BOOL(args[1]) || args[1].as.b;
    *out = val_obj(clone_node(vm, E(args[0]), deep, 0));
    return true;
}
static ObjList *children_list(VM *vm, ObjElement *e) {
    Nodes n = {0};
    logical_children(e, &n, 0);
    ObjList *l = new_list(vm, OBJ_ARRAY, n.count);
    for (int i = 0; i < n.count; i++)
        l->items[l->count++] = val_obj(n.items[i]);
    free(n.items);
    return l;
}
static bool attributes_map(VM *vm, ObjElement *e, Value *out) {
    ObjMap *m = new_map(vm, OBJ_MAP);
    push_root(vm, val_obj(m));
    for (int i = 0; i < e->attr_names.count; i++) {
        if (!attr_present(e, i))
            continue;
        ObjString *v;
        if (!attr_text(vm, e, i, &v) || !map_set(vm, m, e->attr_names.items[i], val_obj(v))) {
            pop_root(vm, 1);
            return false;
        }
    }
    pop_root(vm, 1);
    *out = val_obj(m);
    return true;
}
NATIVE(el_children) {
    (void)argc;
    *out = val_obj(children_list(vm, E(args[0])));
    return true;
}
NATIVE(el_attributes) {
    (void)argc;
    return attributes_map(vm, E(args[0]), out);
}
static bool serialize(VM *vm, ObjElement *e, bool inner, Value *out) {
    Buf b;
    buf_init(&b);
    bool ok = true;
    if (inner) {
        for (int i = 0; i < e->children.count && ok; i++)
            ok = element_render(vm, e->children.items[i], &b);
    } else
        ok = element_render(vm, val_obj(e), &b);
    if (ok)
        *out = sv(vm, b.data ? b.data : "", b.len);
    buf_free(&b);
    return ok;
}
NATIVE(el_render) {
    (void)argc;
    return serialize(vm, E(args[0]), false, out);
}
static ObjElement *sibling(ObjElement *e, int dir) {
    Nodes sib = {0};
    siblings_of(e, &sib);
    ObjElement *r = NULL;
    for (int k = 0; k < sib.count; k++)
        if (sib.items[k] == e && k + dir >= 0 && k + dir < sib.count)
            r = sib.items[k + dir];
    free(sib.items);
    return r;
}

bool lib_element_property(VM *vm, Value self, ObjString *name, Value *out, bool *found) {
    ObjElement *e = E(self);
    const char *n = name->chars;
    *found = true;
    if (!strcmp(n, "tag")) {
        *out = e->tag ? val_obj(e->tag) : svc(vm, "");
        return true;
    }
    if (!strcmp(n, "children")) {
        *out = val_obj(children_list(vm, e));
        return true;
    }
    if (!strcmp(n, "attributes"))
        return attributes_map(vm, e, out);
    if (!strcmp(n, "text_content")) {
        *out = text_value(vm, e);
        return true;
    }
    if (!strcmp(n, "inner_html"))
        return serialize(vm, e, true, out);
    if (!strcmp(n, "outer_html"))
        return serialize(vm, e, false, out);
    if (!strcmp(n, "id") || !strcmp(n, "class_name")) {
        int k = e->tag ? attr_index(e, n[0] == 'i' ? "id" : "class") : -1;
        ObjString *v = NULL;
        if (k >= 0 && attr_present(e, k) && !attr_text(vm, e, k, &v))
            return false;
        *out = v ? val_obj(v) : svc(vm, "");
        return true;
    }
    if (!strcmp(n, "parent")) {
        ObjElement *p = parent_of(e);
        *out = p ? val_obj(p) : val_null();
        return true;
    }
    if (!strcmp(n, "next_sibling") || !strcmp(n, "previous_sibling")) {
        ObjElement *s = sibling(e, n[0] == 'n' ? 1 : -1);
        *out = s ? val_obj(s) : val_null();
        return true;
    }
    *found = false;
    return true;
}
bool lib_element_set_property(VM *vm, Value self, ObjString *name, Value v, bool *found) {
    ObjElement *e = E(self);
    const char *n = name->chars;
    *found = true;
    if (!strcmp(n, "text_content"))
        return set_text(vm, e, v);
    if (!strcmp(n, "id") || !strcmp(n, "class_name")) {
        ObjString *s = value_to_string(vm, v);
        if (!s)
            return false;
        return set_attr(vm, e, n[0] == 'i' ? "id" : "class", val_obj(s));
    }
    *found = false;
    return true;
}

/* ---------------------------------------------------------------- parser */

typedef struct {
    VM *vm;
    const char *s;
    size_t n, p;
    ObjElement *stack[512];
    int depth;
} HP;

static const struct {
    const char *name;
    uint32_t cp;
} named_entities[] = {{"amp", '&'},      {"lt", '<'},        {"gt", '>'},        {"quot", '"'},     {"apos", '\''},
                      {"nbsp", 0xa0},    {"copy", 0xa9},     {"reg", 0xae},      {"trade", 0x2122}, {"mdash", 0x2014},
                      {"ndash", 0x2013}, {"hellip", 0x2026}, {"laquo", 0xab},    {"raquo", 0xbb},   {"lsquo", 0x2018},
                      {"rsquo", 0x2019}, {"ldquo", 0x201c},  {"rdquo", 0x201d},  {"bull", 0x2022},  {"middot", 0xb7},
                      {"times", 0xd7},   {"divide", 0xf7},   {"deg", 0xb0},      {"euro", 0x20ac},  {"pound", 0xa3},
                      {"yen", 0xa5},     {"cent", 0xa2},     {"sect", 0xa7},     {"para", 0xb6},    {"plusmn", 0xb1},
                      {"frac12", 0xbd},  {"frac14", 0xbc},   {"frac34", 0xbe},   {"larr", 0x2190},  {"rarr", 0x2192},
                      {"uarr", 0x2191},  {"darr", 0x2193},   {"hearts", 0x2665}, {"eacute", 0xe9},  {"egrave", 0xe8},
                      {"aacute", 0xe1},  {"agrave", 0xe0},   {"ouml", 0xf6},     {"uuml", 0xfc},    {"auml", 0xe4},
                      {"szlig", 0xdf},   {"ccedil", 0xe7},   {"ntilde", 0xf1},   {NULL, 0}};

static void decode_entities(Buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') {
            buf_addc(b, s[i]);
            continue;
        }
        size_t e = i + 1;
        while (e < n && e - i < 12 && s[e] != ';' && (isalnum((unsigned char)s[e]) || s[e] == '#'))
            e++;
        if (e < n && s[e] == ';' && e > i + 1) {
            const char *name = s + i + 1;
            size_t nl = e - i - 1;
            if (name[0] == '#') {
                bool hex = nl > 1 && (name[1] == 'x' || name[1] == 'X');
                uint32_t v = (uint32_t)strtoul(name + (hex ? 2 : 1), NULL, hex ? 16 : 10);
                if (v == 0 || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
                    v = 0xfffd;
                buf_utf8(b, v);
                i = e;
                continue;
            }
            bool done = false;
            for (int k = 0; named_entities[k].name; k++)
                if (strlen(named_entities[k].name) == nl && !memcmp(named_entities[k].name, name, nl)) {
                    buf_utf8(b, named_entities[k].cp);
                    i = e;
                    done = true;
                    break;
                }
            if (done)
                continue;
        }
        buf_addc(b, '&');
    }
}
static ObjElement *hp_top(HP *h) { return h->stack[h->depth - 1]; }
static bool whitespace_dropped(ObjElement *parent) {
    static const char *tags[] = {"html", "head", "table", "thead", "tbody", "tfoot", "tr", "ul", "ol", "select", "dl", NULL};
    if (!parent->tag)
        return false;
    for (int i = 0; tags[i]; i++)
        if (!strcmp(tags[i], parent->tag->chars))
            return true;
    return false;
}
static void hp_text(HP *h, const char *s, size_t n, bool raw) {
    if (!n)
        return;
    ObjElement *top = hp_top(h);
    bool blank = true;
    for (size_t i = 0; i < n; i++)
        if (!isspace((unsigned char)s[i]))
            blank = false;
    if (blank && whitespace_dropped(top))
        return;
    Buf b;
    buf_init(&b);
    if (raw)
        buf_add(&b, s, n);
    else
        decode_entities(&b, s, n);
    if (b.len) {
        /* merge with a preceding text node */
        if (top->children.count && IS_STRING(top->children.items[top->children.count - 1])) {
            ObjString *prev = AS_STRING(top->children.items[top->children.count - 1]);
            Buf m;
            buf_init(&m);
            buf_add(&m, prev->chars, (size_t)prev->len);
            buf_add(&m, b.data, b.len);
            top->children.items[top->children.count - 1] = sv(h->vm, m.data, m.len);
            buf_free(&m);
        } else
            va_push(h->vm, &top->children, sv(h->vm, b.data, b.len));
    }
    buf_free(&b);
}
static bool in_list(const char *t, const char **list) {
    for (int i = 0; list[i]; i++)
        if (!strcmp(list[i], t))
            return true;
    return false;
}
static void hp_close_to(HP *h, int index) {
    if (index >= 1)
        h->depth = index;
}
static int hp_find(HP *h, const char *tag) {
    for (int i = h->depth - 1; i >= 1; i--)
        if (h->stack[i]->tag && !strcmp(h->stack[i]->tag->chars, tag))
            return i;
    return -1;
}
/* Implicitly closed elements before opening `tag`. */
static void hp_implicit(HP *h, const char *tag) {
    static const char *closes_p[] = {"address", "article", "aside", "blockquote", "div", "dl", "fieldset", "footer", "form",
                                     "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "main", "nav", "ol", "p",
                                     "pre", "section", "table", "ul", "figure", "details", NULL};
    for (;;) {
        ObjElement *top = hp_top(h);
        if (!top->tag)
            return;
        const char *t = top->tag->chars;
        if (!strcmp(t, "p") && in_list(tag, closes_p)) {
            h->depth--;
            continue;
        }
        if (!strcmp(t, "li") && !strcmp(tag, "li")) {
            h->depth--;
            return;
        }
        if ((!strcmp(t, "dt") || !strcmp(t, "dd")) && (!strcmp(tag, "dt") || !strcmp(tag, "dd"))) {
            h->depth--;
            return;
        }
        if (!strcmp(t, "option") && (!strcmp(tag, "option") || !strcmp(tag, "optgroup"))) {
            h->depth--;
            return;
        }
        if ((!strcmp(t, "td") || !strcmp(t, "th")) && (!strcmp(tag, "td") || !strcmp(tag, "th") || !strcmp(tag, "tr"))) {
            h->depth--;
            continue;
        }
        if (!strcmp(t, "tr") && !strcmp(tag, "tr")) {
            h->depth--;
            return;
        }
        return;
    }
}
static bool hp_void(const char *t) {
    static const char *tags[] = {"area", "base", "br", "col", "embed", "hr", "img", "input",
                                 "link", "meta", "param", "source", "track", "wbr", NULL};
    return in_list(t, tags);
}
static bool hp_raw(const char *t) {
    static const char *tags[] = {"script", "style", "textarea", "title", NULL};
    return in_list(t, tags);
}
static size_t find_ci(const char *s, size_t n, size_t from, const char *needle) {
    size_t nl = strlen(needle);
    for (size_t i = from; i + nl <= n; i++) {
        size_t k = 0;
        while (k < nl && tolower((unsigned char)s[i + k]) == needle[k])
            k++;
        if (k == nl)
            return i;
    }
    return n;
}
static void html_parse(HP *h) {
    const char *s = h->s;
    size_t n = h->n;
    size_t text_start = 0;
    while (h->p < n) {
        if (s[h->p] != '<') {
            h->p++;
            continue;
        }
        size_t lt = h->p;
        if (lt + 1 >= n) {
            h->p++;
            continue;
        }
        char c = s[lt + 1];
        if (c == '!' || c == '?') {
            hp_text(h, s + text_start, lt - text_start, false);
            if (lt + 3 < n && !strncmp(s + lt, "<!--", 4)) {
                size_t e = lt + 4;
                while (e + 2 < n && strncmp(s + e, "-->", 3))
                    e++;
                h->p = e + 2 < n ? e + 3 : n;
            } else {
                size_t e = lt;
                while (e < n && s[e] != '>')
                    e++;
                h->p = e < n ? e + 1 : n;
            }
            text_start = h->p;
            continue;
        }
        bool closing = c == '/';
        size_t q = lt + (closing ? 2 : 1);
        if (q >= n || !isalpha((unsigned char)s[q])) {
            h->p++;
            continue;
        }
        hp_text(h, s + text_start, lt - text_start, false);
        size_t nb = q;
        while (q < n && (isalnum((unsigned char)s[q]) || s[q] == '-' || s[q] == ':' || s[q] == '_' || s[q] == '.'))
            q++;
        char tag[128];
        size_t tl = q - nb < sizeof tag - 1 ? q - nb : sizeof tag - 1;
        for (size_t i = 0; i < tl; i++)
            tag[i] = (char)tolower((unsigned char)s[nb + i]);
        tag[tl] = 0;
        if (closing) {
            while (q < n && s[q] != '>')
                q++;
            h->p = q < n ? q + 1 : n;
            text_start = h->p;
            int idx = hp_find(h, tag);
            if (idx >= 1)
                hp_close_to(h, idx);
            continue;
        }
        ObjElement *el = new_element(h->vm, str_intern_c(h->vm, tag));
        push_root(h->vm, val_obj(el));
        bool self_close = false;
        for (;;) {
            while (q < n && isspace((unsigned char)s[q]))
                q++;
            if (q >= n)
                break;
            if (s[q] == '>') {
                q++;
                break;
            }
            if (s[q] == '/' && q + 1 < n && s[q + 1] == '>') {
                self_close = true;
                q += 2;
                break;
            }
            if (s[q] == '/') {
                q++;
                continue;
            }
            size_t ab = q;
            while (q < n && !isspace((unsigned char)s[q]) && s[q] != '=' && s[q] != '>' && !(s[q] == '/' && q + 1 < n && s[q + 1] == '>'))
                q++;
            char an[128];
            size_t al = q - ab < sizeof an - 1 ? q - ab : sizeof an - 1;
            for (size_t i = 0; i < al; i++)
                an[i] = (char)tolower((unsigned char)s[ab + i]);
            an[al] = 0;
            while (q < n && isspace((unsigned char)s[q]))
                q++;
            Value val = svc(h->vm, "");
            if (q < n && s[q] == '=') {
                q++;
                while (q < n && isspace((unsigned char)s[q]))
                    q++;
                size_t vb, ve;
                if (q < n && (s[q] == '"' || s[q] == '\'')) {
                    char quote = s[q++];
                    vb = q;
                    while (q < n && s[q] != quote)
                        q++;
                    ve = q;
                    if (q < n)
                        q++;
                } else {
                    vb = q;
                    while (q < n && !isspace((unsigned char)s[q]) && s[q] != '>')
                        q++;
                    ve = q;
                }
                Buf b;
                buf_init(&b);
                decode_entities(&b, s + vb, ve - vb);
                val = sv(h->vm, b.data ? b.data : "", b.len);
                buf_free(&b);
            }
            if (al && attr_index(el, an) < 0) {
                va_push(h->vm, &el->attr_names, val_obj(str_intern_c(h->vm, an)));
                va_push(h->vm, &el->attr_values, val);
            }
        }
        h->p = q;
        hp_implicit(h, tag);
        ObjElement *top = hp_top(h);
        el->parent = top;
        va_push(h->vm, &top->children, val_obj(el));
        pop_root(h->vm, 1);
        if (!self_close && !hp_void(tag)) {
            if (hp_raw(tag)) {
                char closer[160];
                snprintf(closer, sizeof closer, "</%s", tag);
                size_t end = find_ci(s, n, h->p, closer);
                if (end > h->p) {
                    Buf b;
                    buf_init(&b);
                    if (!strcmp(tag, "textarea") || !strcmp(tag, "title"))
                        decode_entities(&b, s + h->p, end - h->p);
                    else
                        buf_add(&b, s + h->p, end - h->p);
                    va_push(h->vm, &el->children, sv(h->vm, b.data ? b.data : "", b.len));
                    buf_free(&b);
                }
                size_t gt = end;
                while (gt < n && s[gt] != '>')
                    gt++;
                h->p = gt < n ? gt + 1 : n;
            } else if (h->depth < 511) {
                h->stack[h->depth++] = el;
            }
        }
        text_start = h->p;
    }
    hp_text(h, s + text_start, n - text_start, false);
}
NATIVE(h_parse) {
    (void)argc;
    if (!IS_STRING(args[0]))
        return vm_throw(vm, "TypeError", "html.parse expects a string");
    ObjElement *root = new_element(vm, NULL);
    push_root(vm, val_obj(root));
    HP h;
    memset(&h, 0, sizeof h);
    h.vm = vm;
    h.s = AS_CSTR(args[0]);
    h.n = (size_t)AS_STRING(args[0])->len;
    h.stack[0] = root;
    h.depth = 1;
    html_parse(&h);
    pop_root(vm, 1);
    *out = val_obj(root);
    return true;
}

/* ---------------------------------------------------------------- registration */

static const struct {
    const char *name;
    NativeFn fn;
    int min, max; /* without the receiver */
} element_methods[] = {
    {"query", el_query, 1, 1},
    {"query_all", el_query_all, 1, 1},
    {"get_by_id", el_get_by_id, 1, 1},
    {"matches", el_matches, 1, 1},
    {"closest", el_closest, 1, 1},
    {"get_text", el_get_text, 0, 0},
    {"text", el_get_text, 0, 0},
    {"set_text", el_set_text, 1, 1},
    {"get_attribute", el_get_attribute, 1, 1},
    {"has_attribute", el_has_attribute, 1, 1},
    {"set_attribute", el_set_attribute, 2, 2},
    {"remove_attribute", el_remove_attribute, 1, 1},
    {"has_class", el_has_class, 1, 1},
    {"add_class", el_add_class, 1, 1},
    {"remove_class", el_remove_class, 1, 1},
    {"toggle_class", el_toggle_class, 1, 1},
    {"append", el_append, 1, -1},
    {"prepend", el_prepend, 1, -1},
    {"remove_child", el_remove_child, 1, 1},
    {"clear", el_clear, 0, 0},
    {"clone", el_clone, 0, 1},
    {"children", el_children, 0, 0},
    {"attributes", el_attributes, 0, 0},
    {"render", el_render, 0, 0},
    {NULL, NULL, 0, 0},
};

void lib_html_init(VM *vm) {
    for (int i = 0; element_methods[i].name; i++) {
        ObjNative *n = new_native(vm, element_methods[i].name, element_methods[i].fn, element_methods[i].min + 1,
                                  element_methods[i].max < 0 ? -1 : element_methods[i].max + 1);
        n->is_method = true;
        table_set(vm, &vm->type_methods[TM_ELEMENT], str_intern_c(vm, element_methods[i].name), val_obj(n));
    }
}
/* html.operation(tree, ...) forms forward to the element methods. */
NATIVE(h_forward) {
    Value name = vm->desc_args[0];
    if (argc < 1 || !IS_OBJT(args[0], OBJ_ELEMENT)) {
        if (argc >= 1 && IS_NULL(args[0]))
            return vm_throw(vm, "CastError", "cannot cast null to element");
        return vm_throw(vm, "TypeError", "html.%s expects an element", AS_CSTR(name));
    }
    bool found;
    if (!lib_builtin_method(vm, args[0], AS_STRING(name), argc, args, out, &found))
        return false;
    if (!found)
        return vm_throw(vm, "TypeError", "html has no operation %s", AS_CSTR(name));
    return true;
}
NATIVE(h_property) {
    (void)argc;
    Value name = vm->desc_args[0];
    if (!IS_OBJT(args[0], OBJ_ELEMENT)) {
        if (IS_NULL(args[0]))
            return vm_throw(vm, "CastError", "cannot cast null to element");
        return vm_throw(vm, "TypeError", "html.%s expects an element", AS_CSTR(name));
    }
    bool found;
    return lib_element_property(vm, args[0], AS_STRING(name), out, &found);
}
void lib_html_module(VM *vm, ObjModule *m) {
    static const char *props[] = {"tag", "text_content", "inner_html", "outer_html", "id", "class_name",
                                  "parent", "next_sibling", "previous_sibling", NULL};
    for (int i = 0; props[i]; i++) {
        ObjNative *n = new_native(vm, props[i], h_property, 1, 1);
        n->data = val_obj(str_intern_c(vm, props[i]));
        module_set_value(vm, m, props[i], val_obj(n));
    }
    module_set_native(vm, m, "parse", h_parse, 1, 1);
    for (int i = 0; element_methods[i].name; i++) {
        if (!strcmp(element_methods[i].name, "render"))
            continue;
        ObjNative *n = new_native(vm, element_methods[i].name, h_forward, element_methods[i].min + 1,
                                  element_methods[i].max < 0 ? -1 : element_methods[i].max + 1);
        n->data = val_obj(str_intern_c(vm, element_methods[i].name));
        module_set_value(vm, m, element_methods[i].name, val_obj(n));
    }
}
