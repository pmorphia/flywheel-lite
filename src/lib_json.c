/* JSON: parsing to plain values, encoding any value, typed decoding, and the json module. */
#include "lite.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>

typedef struct {
    VM *vm;
    const char *s;
    size_t n, p;
    int depth;
} JP;

static bool jfail(JP *j, const char *msg) {
    return vm_throw(j->vm, "JsonError", "invalid JSON at offset %zu: %s", j->p, msg);
}
static void jspace(JP *j) {
    while (j->p < j->n && (j->s[j->p] == ' ' || j->s[j->p] == '\t' || j->s[j->p] == '\n' || j->s[j->p] == '\r'))
        j->p++;
}
static int hexv(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static bool jstring(JP *j, Buf *b) {
    j->p++; /* opening quote */
    while (j->p < j->n) {
        unsigned char c = (unsigned char)j->s[j->p];
        if (c == '"') {
            j->p++;
            return true;
        }
        if (c < 0x20)
            return jfail(j, "control character in string");
        if (c == '\\') {
            if (++j->p >= j->n)
                break;
            char e = j->s[j->p++];
            switch (e) {
            case '"': buf_addc(b, '"'); break;
            case '\\': buf_addc(b, '\\'); break;
            case '/': buf_addc(b, '/'); break;
            case 'b': buf_addc(b, '\b'); break;
            case 'f': buf_addc(b, '\f'); break;
            case 'n': buf_addc(b, '\n'); break;
            case 'r': buf_addc(b, '\r'); break;
            case 't': buf_addc(b, '\t'); break;
            case 'u': {
                uint32_t cp = 0;
                for (int i = 0; i < 4; i++) {
                    int h = j->p < j->n ? hexv(j->s[j->p++]) : -1;
                    if (h < 0)
                        return jfail(j, "invalid \\u escape");
                    cp = cp * 16 + (uint32_t)h;
                }
                if (cp >= 0xd800 && cp <= 0xdbff && j->p + 6 <= j->n && j->s[j->p] == '\\' && j->s[j->p + 1] == 'u') {
                    uint32_t lo = 0;
                    bool ok = true;
                    for (int i = 0; i < 4; i++) {
                        int h = hexv(j->s[j->p + 2 + (size_t)i]);
                        if (h < 0)
                            ok = false;
                        lo = lo * 16 + (uint32_t)(h < 0 ? 0 : h);
                    }
                    if (ok && lo >= 0xdc00 && lo <= 0xdfff) {
                        j->p += 6;
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    }
                }
                if (cp >= 0xd800 && cp <= 0xdfff)
                    cp = 0xfffd;
                buf_utf8(b, cp);
                break;
            }
            default:
                return jfail(j, "invalid escape in string");
            }
            continue;
        }
        buf_addc(b, (char)c);
        j->p++;
    }
    return jfail(j, "unterminated string");
}
static bool jvalue(JP *j, Value *out) {
    VM *vm = j->vm;
    if (++j->depth > 512)
        return jfail(j, "nesting is too deep");
    jspace(j);
    if (j->p >= j->n)
        return jfail(j, "expected a value");
    char c = j->s[j->p];
    bool ok = true;
    if (c == '{') {
        j->p++;
        ObjMap *m = new_map(vm, OBJ_MAP);
        *out = val_obj(m);
        push_root(vm, *out);
        jspace(j);
        if (j->p < j->n && j->s[j->p] == '}') {
            j->p++;
        } else
            for (;;) {
                jspace(j);
                if (j->p >= j->n || j->s[j->p] != '"') {
                    ok = jfail(j, "expected a string key");
                    break;
                }
                Buf k;
                buf_init(&k);
                if (!jstring(j, &k)) {
                    buf_free(&k);
                    ok = false;
                    break;
                }
                Value key = val_obj(str_copy(vm, k.data ? k.data : "", k.len));
                buf_free(&k);
                push_root(vm, key);
                jspace(j);
                if (j->p >= j->n || j->s[j->p] != ':') {
                    pop_root(vm, 1);
                    ok = jfail(j, "expected ':' after key");
                    break;
                }
                j->p++;
                Value v;
                if (!jvalue(j, &v)) {
                    pop_root(vm, 1);
                    ok = false;
                    break;
                }
                push_root(vm, v);
                map_set(vm, m, key, v);
                pop_root(vm, 2);
                jspace(j);
                if (j->p < j->n && j->s[j->p] == ',') {
                    j->p++;
                    continue;
                }
                if (j->p < j->n && j->s[j->p] == '}') {
                    j->p++;
                    break;
                }
                ok = jfail(j, "expected ',' or '}'");
                break;
            }
        pop_root(vm, 1);
    } else if (c == '[') {
        j->p++;
        ObjList *l = new_list(vm, OBJ_ARRAY, 0);
        *out = val_obj(l);
        push_root(vm, *out);
        jspace(j);
        if (j->p < j->n && j->s[j->p] == ']') {
            j->p++;
        } else
            for (;;) {
                Value v;
                if (!jvalue(j, &v)) {
                    ok = false;
                    break;
                }
                list_push(vm, l, v);
                jspace(j);
                if (j->p < j->n && j->s[j->p] == ',') {
                    j->p++;
                    continue;
                }
                if (j->p < j->n && j->s[j->p] == ']') {
                    j->p++;
                    break;
                }
                ok = jfail(j, "expected ',' or ']'");
                break;
            }
        pop_root(vm, 1);
    } else if (c == '"') {
        Buf b;
        buf_init(&b);
        ok = jstring(j, &b);
        if (ok)
            *out = val_obj(str_copy(vm, b.data ? b.data : "", b.len));
        buf_free(&b);
    } else if (j->n - j->p >= 4 && !memcmp(j->s + j->p, "true", 4)) {
        j->p += 4;
        *out = val_bool(true);
    } else if (j->n - j->p >= 5 && !memcmp(j->s + j->p, "false", 5)) {
        j->p += 5;
        *out = val_bool(false);
    } else if (j->n - j->p >= 4 && !memcmp(j->s + j->p, "null", 4)) {
        j->p += 4;
        *out = val_null();
    } else if (c == '-' || isdigit((unsigned char)c)) {
        size_t b = j->p;
        bool real = false;
        if (j->s[j->p] == '-')
            j->p++;
        if (j->p >= j->n || !isdigit((unsigned char)j->s[j->p]))
            return jfail(j, "invalid number");
        if (j->s[j->p] == '0')
            j->p++;
        else
            while (j->p < j->n && isdigit((unsigned char)j->s[j->p]))
                j->p++;
        if (j->p < j->n && j->s[j->p] == '.') {
            real = true;
            j->p++;
            if (j->p >= j->n || !isdigit((unsigned char)j->s[j->p]))
                return jfail(j, "invalid number");
            while (j->p < j->n && isdigit((unsigned char)j->s[j->p]))
                j->p++;
        }
        if (j->p < j->n && (j->s[j->p] == 'e' || j->s[j->p] == 'E')) {
            real = true;
            j->p++;
            if (j->p < j->n && (j->s[j->p] == '+' || j->s[j->p] == '-'))
                j->p++;
            if (j->p >= j->n || !isdigit((unsigned char)j->s[j->p]))
                return jfail(j, "invalid number");
            while (j->p < j->n && isdigit((unsigned char)j->s[j->p]))
                j->p++;
        }
        char tmp[64];
        size_t len = j->p - b < 63 ? j->p - b : 63;
        memcpy(tmp, j->s + b, len);
        tmp[len] = 0;
        if (!real) {
            char *end;
            errno = 0;
            long long v = strtoll(tmp, &end, 10);
            if (errno == ERANGE)
                *out = val_double(strtod(tmp, NULL));
            else
                *out = val_int(v);
        } else
            *out = val_double(strtod(tmp, NULL));
    } else {
        char msg[64];
        snprintf(msg, sizeof msg, "unexpected character '%c'", c);
        ok = jfail(j, msg);
    }
    j->depth--;
    return ok;
}
bool lib_json_parse(VM *vm, const char *s, size_t n, Value *out) {
    JP j = {vm, s, n, 0, 0};
    if (!jvalue(&j, out))
        return false;
    jspace(&j);
    if (j.p != n)
        return jfail(&j, "unexpected text after the value");
    return true;
}

/* ---------------------------------------------------------------- encoding */

static void jquote(Buf *b, const char *s, size_t n) {
    buf_addc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': buf_adds(b, "\\\""); break;
        case '\\': buf_adds(b, "\\\\"); break;
        case '\n': buf_adds(b, "\\n"); break;
        case '\r': buf_adds(b, "\\r"); break;
        case '\t': buf_adds(b, "\\t"); break;
        case '\b': buf_adds(b, "\\b"); break;
        case '\f': buf_adds(b, "\\f"); break;
        default:
            if (c < 0x20)
                buf_printf(b, "\\u%04x", c);
            else
                buf_addc(b, (char)c);
        }
    }
    buf_addc(b, '"');
}
static void jnumber(Buf *b, double d) {
    if (!isfinite(d)) {
        buf_adds(b, "null");
        return;
    }
    char tmp[40];
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(tmp, sizeof tmp, "%.*g", prec, d);
        if (strtod(tmp, NULL) == d)
            break;
    }
    buf_adds(b, tmp);
}
static void newline(Buf *b, int indent, int level) {
    if (!indent)
        return;
    buf_addc(b, '\n');
    for (int i = 0; i < indent * level; i++)
        buf_addc(b, ' ');
}
static int cmp_keys(const void *a, const void *b) {
    const MapEntry *x = *(const MapEntry *const *)a, *y = *(const MapEntry *const *)b;
    ObjString *s = AS_STRING(x->key), *t = AS_STRING(y->key);
    int n = s->len < t->len ? s->len : t->len;
    int c = memcmp(s->chars, t->chars, (size_t)n);
    return c ? c : s->len - t->len;
}
bool lib_json_encode(VM *vm, Value v, Buf *b, int indent, int level, bool sorted) {
    if (level > 256)
        return vm_throw(vm, "JsonError", "value nested too deeply (or a cycle)");
    switch (v.type) {
    case VAL_NULL: buf_adds(b, "null"); return true;
    case VAL_BOOL: buf_adds(b, v.as.b ? "true" : "false"); return true;
    case VAL_INT: buf_printf(b, "%lld", (long long)v.as.i); return true;
    case VAL_DOUBLE: jnumber(b, v.as.d); return true;
    case VAL_CHAR: {
        Buf c;
        buf_init(&c);
        buf_utf8(&c, v.as.c);
        jquote(b, c.data, c.len);
        buf_free(&c);
        return true;
    }
    case VAL_OBJ: break;
    default: return vm_throw(vm, "JsonError", "cannot encode an uninitialized value");
    }
    Obj *o = AS_OBJ(v);
    switch ((ObjType)o->type) {
    case OBJ_STRING: jquote(b, ((ObjString *)o)->chars, (size_t)((ObjString *)o)->len); return true;
    case OBJ_JSON: return lib_json_encode(vm, ((ObjJson *)o)->v, b, indent, level, true);
    case OBJ_ARRAY:
    case OBJ_TUPLE:
    case OBJ_SET: {
        ObjList *l = NULL;
        if (o->type == OBJ_SET)
            l = map_keys(vm, (ObjMap *)o);
        else
            l = (ObjList *)o;
        buf_addc(b, '[');
        for (int i = 0; i < l->count; i++) {
            if (i)
                buf_addc(b, ',');
            newline(b, indent, level + 1);
            if (!lib_json_encode(vm, l->items[i], b, indent, level + 1, sorted))
                return false;
        }
        if (l->count)
            newline(b, indent, level);
        buf_addc(b, ']');
        return true;
    }
    case OBJ_MAP: {
        ObjMap *m = (ObjMap *)o;
        MapEntry **es = xmalloc(sizeof(MapEntry *) * (size_t)(m->live + 1));
        int n = 0;
        bool all_strings = true;
        for (int i = 0; i < m->used; i++)
            if (m->entries[i].live) {
                es[n++] = &m->entries[i];
                if (!IS_STRING(m->entries[i].key))
                    all_strings = false;
            }
        if (sorted && all_strings)
            qsort(es, (size_t)n, sizeof *es, cmp_keys);
        buf_addc(b, '{');
        for (int i = 0; i < n; i++) {
            if (i)
                buf_addc(b, ',');
            newline(b, indent, level + 1);
            if (IS_STRING(es[i]->key))
                jquote(b, AS_CSTR(es[i]->key), (size_t)AS_STRING(es[i]->key)->len);
            else {
                ObjString *k = value_to_string(vm, es[i]->key);
                if (!k) {
                    free(es);
                    return false;
                }
                jquote(b, k->chars, (size_t)k->len);
            }
            buf_addc(b, ':');
            if (indent)
                buf_addc(b, ' ');
            if (!lib_json_encode(vm, es[i]->value, b, indent, level + 1, sorted)) {
                free(es);
                return false;
            }
        }
        if (n)
            newline(b, indent, level);
        buf_addc(b, '}');
        free(es);
        return true;
    }
    case OBJ_INSTANCE: {
        ObjInstance *in = (ObjInstance *)o;
        buf_addc(b, '{');
        for (int i = 0; i < in->nfields; i++) {
            if (i)
                buf_addc(b, ',');
            newline(b, indent, level + 1);
            ObjString *k = in->klass->field_names[i];
            jquote(b, k->chars, (size_t)k->len);
            buf_addc(b, ':');
            if (indent)
                buf_addc(b, ' ');
            if (!lib_json_encode(vm, in->fields[i], b, indent, level + 1, sorted))
                return false;
        }
        if (in->nfields)
            newline(b, indent, level);
        buf_addc(b, '}');
        return true;
    }
    case OBJ_ENUMVAL: {
        ObjString *n = ((ObjEnumVal *)o)->name;
        jquote(b, n->chars, (size_t)n->len);
        return true;
    }
    case OBJ_RESULT: {
        ObjResult *r = (ObjResult *)o;
        buf_adds(b, r->ok ? "{\"ok\":" : "{\"error\":");
        if (indent)
            buf_addc(b, ' ');
        if (!lib_json_encode(vm, r->value, b, indent, level + 1, sorted))
            return false;
        buf_addc(b, '}');
        return true;
    }
    case OBJ_ERROR: {
        ObjError *e = (ObjError *)o;
        buf_adds(b, "{\"type\":");
        jquote(b, e->type->chars, (size_t)e->type->len);
        buf_adds(b, ",\"message\":");
        jquote(b, e->message->chars, (size_t)e->message->len);
        buf_addc(b, '}');
        return true;
    }
    case OBJ_ELEMENT: {
        Buf h;
        buf_init(&h);
        if (!element_render(vm, v, &h)) {
            buf_free(&h);
            return false;
        }
        jquote(b, h.data ? h.data : "", h.len);
        buf_free(&h);
        return true;
    }
    default: {
        return vm_throw(vm, "JsonError", "cannot encode a %s as JSON", obj_kind_name(v));
    }
    }
}

/* ---------------------------------------------------------------- typed decoding */

static Value plain(Value v) { return IS_OBJT(v, OBJ_JSON) ? ((ObjJson *)AS_OBJ(v))->v : v; }
static bool jtype_err(VM *vm, const char *want, const char *path) {
    return vm_throw(vm, "JsonError", "expected %s%s%s", want, path[0] ? " at " : "", path);
}

bool json_decode_into(VM *vm, Value v, ObjTypeDesc *t, Value *out, const char *path);

static bool decode_instance(VM *vm, ObjClass *k, Value v, Value *out, const char *path) {
    if (!IS_MAP(v))
        return jtype_err(vm, "an object", path);
    ObjInstance *in = new_instance(vm, k);
    push_root(vm, val_obj(in));
    for (int i = 0; i < k->nfields; i++) {
        ObjString *name = k->field_names[i];
        Value fv;
        bool found;
        if (!map_get(vm, AS_MAP(v), val_obj(name), &fv, &found)) {
            pop_root(vm, 1);
            return false;
        }
        Value ft = k->field_types[i];
        if (found) {
            char sub[256];
            snprintf(sub, sizeof sub, "%s%s%s", path, path[0] ? "." : "", name->chars);
            if (IS_OBJ(ft)) {
                if (!json_decode_into(vm, fv, AS_TYPE(ft), &in->fields[i], sub)) {
                    pop_root(vm, 1);
                    return false;
                }
            } else
                in->fields[i] = fv;
        } else if (!IS_UNDEF(k->field_consts[i])) {
            in->fields[i] = k->field_consts[i];
        } else if (IS_OBJ(ft) && AS_TYPE(ft)->kind == TY_NULLABLE) {
            in->fields[i] = val_null();
        } else if (!k->field_has_default[i]) {
            pop_root(vm, 1);
            return vm_throw(vm, "JsonError", "missing JSON field '%s' for %s", name->chars, k->name->chars);
        }
    }
    extern bool vm_run_defaults(VM * vm, ObjClass * k, ObjInstance * inst);
    bool ok = vm_run_defaults(vm, k, in);
    pop_root(vm, 1);
    *out = val_obj(in);
    return ok;
}

bool json_decode_into(VM *vm, Value v, ObjTypeDesc *t, Value *out, const char *path) {
    v = plain(v);
    switch (t->kind) {
    case TY_ANY:
    case TY_FUNCTION:
        *out = v;
        return true;
    case TY_JSON:
        *out = val_obj(new_json(vm, v));
        return true;
    case TY_NULLABLE:
        if (IS_NULL(v)) {
            *out = v;
            return true;
        }
        return json_decode_into(vm, v, t->args[0], out, path);
    case TY_INT:
        if (IS_INT(v)) {
            *out = v;
            return true;
        }
        if (IS_DOUBLE(v) && v.as.d == floor(v.as.d) && fabs(v.as.d) < 9e18) {
            *out = val_int((int64_t)v.as.d);
            return true;
        }
        return jtype_err(vm, "an integer", path);
    case TY_DOUBLE:
    case TY_FLOAT:
        if (IS_NUM(v)) {
            *out = val_double(AS_NUM(v));
            return true;
        }
        return jtype_err(vm, "a number", path);
    case TY_BOOL:
        if (IS_BOOL(v)) {
            *out = v;
            return true;
        }
        return jtype_err(vm, "a bool", path);
    case TY_STRING:
        if (IS_STRING(v)) {
            *out = v;
            return true;
        }
        return jtype_err(vm, "a string", path);
    case TY_CHAR:
        if (IS_STRING(v) && AS_STRING(v)->clen == 1) {
            *out = val_char(str_char_at(AS_STRING(v), 0));
            return true;
        }
        return jtype_err(vm, "a one-character string", path);
    case TY_ARRAY:
    case TY_HTUPLE:
    case TY_TUPLE:
    case TY_SET: {
        if (!IS_ARRAY(v))
            return jtype_err(vm, "an array", path);
        ObjList *src = AS_LIST(v);
        if (t->kind == TY_TUPLE && src->count != t->nargs)
            return vm_throw(vm, "JsonError", "expected %d items%s%s", t->nargs, path[0] ? " at " : "", path);
        ObjList *l = new_list(vm, t->kind == TY_ARRAY || t->kind == TY_SET ? OBJ_ARRAY : OBJ_TUPLE, src->count);
        push_root(vm, val_obj(l));
        for (int i = 0; i < src->count; i++) {
            ObjTypeDesc *et = t->kind == TY_TUPLE ? t->args[i] : t->nargs ? t->args[0] : NULL;
            char sub[256];
            snprintf(sub, sizeof sub, "%s[%d]", path, i);
            Value item = src->items[i];
            if (et && !json_decode_into(vm, item, et, &item, sub)) {
                pop_root(vm, 1);
                return false;
            }
            l->items[l->count++] = item;
        }
        if (t->kind == TY_SET) {
            ObjMap *m = new_map(vm, OBJ_SET);
            push_root(vm, val_obj(m));
            for (int i = 0; i < l->count; i++)
                if (!map_set(vm, m, l->items[i], val_null())) {
                    pop_root(vm, 2);
                    return false;
                }
            pop_root(vm, 2);
            *out = val_obj(m);
            return true;
        }
        pop_root(vm, 1);
        *out = val_obj(l);
        return true;
    }
    case TY_MAP: {
        if (!IS_MAP(v))
            return jtype_err(vm, "an object", path);
        ObjMap *src = AS_MAP(v);
        ObjMap *m = new_map(vm, OBJ_MAP);
        push_root(vm, val_obj(m));
        for (int i = 0; i < src->used; i++) {
            if (!src->entries[i].live)
                continue;
            Value k = src->entries[i].key, val = src->entries[i].value;
            if (t->nargs == 2 && t->args[0]->kind != TY_STRING && t->args[0]->kind != TY_ANY) {
                if (t->args[0]->kind == TY_NAMED) {
                    if (!json_decode_into(vm, k, t->args[0], &k, path)) {
                        pop_root(vm, 1);
                        return false;
                    }
                } else if (!vm_cast(vm, t->args[0], k, &k)) {
                    pop_root(vm, 1);
                    return false;
                }
            }
            if (t->nargs == 2 && !json_decode_into(vm, val, t->args[1], &val, path)) {
                pop_root(vm, 1);
                return false;
            }
            if (!map_set(vm, m, k, val)) {
                pop_root(vm, 1);
                return false;
            }
        }
        pop_root(vm, 1);
        *out = val_obj(m);
        return true;
    }
    case TY_NAMED: {
        Value k = vm_resolve_named(vm, t);
        if (IS_OBJT(k, OBJ_CLASS))
            return decode_instance(vm, AS_CLASS(k), v, out, path);
        if (IS_OBJT(k, OBJ_ENUM)) {
            ObjEnum *e = (ObjEnum *)AS_OBJ(k);
            if (IS_STRING(v))
                for (int i = 0; i < e->count; i++)
                    if (!strcmp(((ObjEnumVal *)AS_OBJ(e->members[i]))->name->chars, AS_CSTR(v))) {
                        *out = e->members[i];
                        return true;
                    }
            return jtype_err(vm, "an enum member name", path);
        }
        *out = val_obj(new_json(vm, v));
        return true;
    }
    default:
        *out = v;
        return true;
    }
}

bool lib_class_from_json(VM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    Value v = args[1];
    if (IS_STRING(v)) {
        if (!lib_json_parse(vm, AS_CSTR(v), (size_t)AS_STRING(v)->len, &v))
            return false;
    }
    push_root(vm, v);
    bool ok = decode_instance(vm, AS_CLASS(args[0]), plain(v), out, "");
    pop_root(vm, 1);
    return ok;
}

/* ---------------------------------------------------------------- module functions */

#define NATIVE(name) static bool name(VM *vm, int argc, Value *args, Value *out)

static bool want_json(VM *vm, Value v, Value *inner, const char *fn) {
    if (IS_OBJT(v, OBJ_JSON)) {
        *inner = ((ObjJson *)AS_OBJ(v))->v;
        return true;
    }
    if (IS_MAP(v) || IS_ARRAY(v) || IS_STRING(v) || IS_NUM(v) || IS_BOOL(v) || IS_NULL(v)) {
        *inner = v;
        return true;
    }
    return vm_throw(vm, "TypeError", "json.%s expects a json.value", fn);
}
static Value wrap(VM *vm, Value v) { return val_obj(new_json(vm, v)); }

NATIVE(j_parse) {
    (void)argc;
    if (!IS_STRING(args[0]))
        return vm_throw(vm, "TypeError", "json.parse expects a string");
    Value v;
    if (!lib_json_parse(vm, AS_CSTR(args[0]), (size_t)AS_STRING(args[0])->len, &v))
        return false;
    *out = wrap(vm, v);
    return true;
}
NATIVE(j_stringify) {
    (void)argc;
    Buf b;
    buf_init(&b);
    if (!lib_json_encode(vm, args[0], &b, 0, 0, true)) {
        buf_free(&b);
        return false;
    }
    *out = val_obj(str_copy(vm, b.data ? b.data : "", b.len));
    buf_free(&b);
    return true;
}
NATIVE(j_encode) {
    int indent = 0;
    if (argc > 1) {
        if (!IS_INT(args[1]))
            return vm_throw(vm, "TypeError", "indent must be an int");
        indent = (int)args[1].as.i;
    }
    Buf b;
    buf_init(&b);
    if (!lib_json_encode(vm, args[0], &b, indent, 0, false)) {
        buf_free(&b);
        return false;
    }
    *out = val_obj(str_copy(vm, b.data ? b.data : "", b.len));
    buf_free(&b);
    return true;
}
NATIVE(j_get) {
    (void)argc;
    Value in = val_null();
    if (!want_json(vm, args[0], &in, "get"))
        return false;
    if (!IS_MAP(in) || !IS_STRING(args[1]))
        return vm_throw(vm, "JsonError", "json.get needs an object and a string key");
    bool found;
    Value v;
    if (!map_get(vm, AS_MAP(in), args[1], &v, &found))
        return false;
    if (!found)
        return vm_throw(vm, "JsonError", "missing JSON field '%s'", AS_CSTR(args[1]));
    *out = wrap(vm, v);
    return true;
}
NATIVE(j_at) {
    (void)argc;
    Value in = val_null();
    if (!want_json(vm, args[0], &in, "at"))
        return false;
    if (!IS_ARRAY(in) || !IS_INT(args[1]))
        return vm_throw(vm, "JsonError", "json.at needs an array and an int index");
    ObjList *l = AS_LIST(in);
    if (args[1].as.i < 0 || args[1].as.i >= l->count)
        return vm_throw(vm, "JsonError", "JSON index %lld out of range", (long long)args[1].as.i);
    *out = wrap(vm, l->items[args[1].as.i]);
    return true;
}
NATIVE(j_has) {
    (void)argc;
    Value in = val_null();
    if (!want_json(vm, args[0], &in, "has"))
        return false;
    bool found = false;
    if (IS_MAP(in) && IS_STRING(args[1]) && !map_has(vm, AS_MAP(in), args[1], &found))
        return false;
    *out = val_bool(found);
    return true;
}
NATIVE(j_length) {
    (void)argc;
    Value in = val_null();
    if (!want_json(vm, args[0], &in, "length"))
        return false;
    if (IS_MAP(in))
        *out = val_int(AS_MAP(in)->live);
    else if (IS_ARRAY(in))
        *out = val_int(AS_LIST(in)->count);
    else if (IS_STRING(in))
        *out = val_int(AS_STRING(in)->clen);
    else
        return vm_throw(vm, "JsonError", "this JSON value has no length");
    return true;
}
NATIVE(j_keys) {
    (void)argc;
    Value in = val_null();
    if (!want_json(vm, args[0], &in, "keys"))
        return false;
    if (!IS_MAP(in))
        return vm_throw(vm, "JsonError", "json.keys needs an object");
    *out = val_obj(map_keys(vm, AS_MAP(in)));
    return true;
}
NATIVE(j_items) {
    (void)argc;
    Value in = val_null();
    if (!want_json(vm, args[0], &in, "items"))
        return false;
    ObjList *src = NULL;
    if (IS_ARRAY(in))
        src = AS_LIST(in);
    else if (IS_MAP(in))
        src = map_values(vm, AS_MAP(in));
    else
        return vm_throw(vm, "JsonError", "json.items needs an array or object");
    push_root(vm, val_obj(src));
    ObjList *l = new_list(vm, OBJ_ARRAY, src->count);
    push_root(vm, val_obj(l));
    for (int i = 0; i < src->count; i++)
        l->items[l->count++] = wrap(vm, src->items[i]);
    pop_root(vm, 2);
    *out = val_obj(l);
    return true;
}
NATIVE(j_is_null) {
    (void)argc;
    Value in = val_null();
    if (!want_json(vm, args[0], &in, "is_null"))
        return false;
    *out = val_bool(IS_NULL(in));
    return true;
}
NATIVE(j_kind) {
    (void)argc;
    Value in = val_null();
    if (!want_json(vm, args[0], &in, "kind"))
        return false;
    const char *k = IS_NULL(in) ? "null" : IS_BOOL(in) ? "bool" : IS_NUM(in) ? "number" : IS_STRING(in) ? "string" : IS_ARRAY(in) ? "array" : "object";
    *out = val_obj(str_intern_c(vm, k));
    return true;
}
NATIVE(j_as_string) {
    (void)argc;
    Value in = val_null();
    if (!want_json(vm, args[0], &in, "as_string"))
        return false;
    if (!IS_STRING(in))
        return vm_throw(vm, "JsonError", "expected JSON string");
    *out = in;
    return true;
}
NATIVE(j_as_int) {
    (void)argc;
    Value in = val_null();
    if (!want_json(vm, args[0], &in, "as_int"))
        return false;
    if (IS_INT(in))
        *out = in;
    else if (IS_DOUBLE(in) && in.as.d == floor(in.as.d) && fabs(in.as.d) < 9e18)
        *out = val_int((int64_t)in.as.d);
    else
        return vm_throw(vm, "JsonError", "expected JSON integer");
    return true;
}
NATIVE(j_as_double) {
    (void)argc;
    Value in = val_null();
    if (!want_json(vm, args[0], &in, "as_double"))
        return false;
    if (!IS_NUM(in))
        return vm_throw(vm, "JsonError", "expected JSON number");
    *out = val_double(AS_NUM(in));
    return true;
}
NATIVE(j_as_bool) {
    (void)argc;
    Value in = val_null();
    if (!want_json(vm, args[0], &in, "as_bool"))
        return false;
    if (!IS_BOOL(in))
        return vm_throw(vm, "JsonError", "expected JSON bool");
    *out = in;
    return true;
}
static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    Buf b;
    buf_init(&b);
    char tmp[8192];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0)
        buf_add(&b, tmp, n);
    fclose(f);
    if (!b.data)
        buf_add(&b, "", 0);
    *len = b.len;
    return b.data;
}
NATIVE(j_load) {
    (void)argc;
    if (!IS_STRING(args[0]))
        return vm_throw(vm, "TypeError", "json.load expects a path");
    size_t len;
    char *text = slurp(AS_CSTR(args[0]), &len);
    if (!text)
        return vm_throw(vm, "FileError", "cannot read %s", AS_CSTR(args[0]));
    Value v;
    bool ok = lib_json_parse(vm, text, len, &v);
    free(text);
    if (!ok)
        return false;
    *out = wrap(vm, v);
    return true;
}
NATIVE(j_dump) {
    if (!IS_STRING(args[1]))
        return vm_throw(vm, "TypeError", "json.dump expects a path");
    int indent = argc > 2 && IS_INT(args[2]) ? (int)args[2].as.i : 0;
    Buf b;
    buf_init(&b);
    if (!lib_json_encode(vm, args[0], &b, indent, 0, false)) {
        buf_free(&b);
        return false;
    }
    FILE *f = fopen(AS_CSTR(args[1]), "wb");
    if (!f) {
        buf_free(&b);
        return vm_throw(vm, "FileError", "cannot write %s", AS_CSTR(args[1]));
    }
    fwrite(b.data ? b.data : "", 1, b.len, f);
    fclose(f);
    buf_free(&b);
    *out = val_null();
    return true;
}
NATIVE(j_decode) {
    (void)argc;
    Value t = vm->desc_args[0];
    Value v = args[0];
    if (IS_STRING(v)) {
        if (!lib_json_parse(vm, AS_CSTR(v), (size_t)AS_STRING(v)->len, &v))
            return false;
    } else
        v = plain(v);
    if (IS_OBJT(t, OBJ_TYPE)) {
        push_root(vm, v);
        bool ok = json_decode_into(vm, v, AS_TYPE(t), out, "");
        pop_root(vm, 1);
        return ok;
    }
    /* untyped: a json.value that converts when assigned to a typed binding */
    *out = wrap(vm, v);
    return true;
}

void lib_json_module(VM *vm, ObjModule *m) {
    module_set_native(vm, m, "parse", j_parse, 1, 1);
    module_set_native(vm, m, "loads", j_parse, 1, 1);
    module_set_native(vm, m, "stringify", j_stringify, 1, 1);
    module_set_native(vm, m, "encode", j_encode, 1, 2);
    module_set_native(vm, m, "dumps", j_encode, 1, 2);
    module_set_native(vm, m, "get", j_get, 2, 2);
    module_set_native(vm, m, "at", j_at, 2, 2);
    module_set_native(vm, m, "has", j_has, 2, 2);
    module_set_native(vm, m, "length", j_length, 1, 1);
    module_set_native(vm, m, "keys", j_keys, 1, 1);
    module_set_native(vm, m, "items", j_items, 1, 1);
    module_set_native(vm, m, "is_null", j_is_null, 1, 1);
    module_set_native(vm, m, "kind", j_kind, 1, 1);
    module_set_native(vm, m, "as_string", j_as_string, 1, 1);
    module_set_native(vm, m, "as_int", j_as_int, 1, 1);
    module_set_native(vm, m, "as_double", j_as_double, 1, 1);
    module_set_native(vm, m, "as_bool", j_as_bool, 1, 1);
    module_set_native(vm, m, "load", j_load, 1, 1);
    module_set_native(vm, m, "dump", j_dump, 2, 3);
    module_set_native(vm, m, "decode", j_decode, 1, 1);
}
