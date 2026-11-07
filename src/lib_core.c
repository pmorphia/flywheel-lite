/* Core library: builtin functions, casts, and methods of builtin types. */
#include "lite.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>

#define NATIVE(name) static bool name(VM *vm, int argc, Value *args, Value *out)
#define UNUSED_ARGS (void)argc

static bool type_err(VM *vm, const char *fn, const char *want, Value got) {
    Buf b;
    buf_init(&b);
    type_name_of(vm, got, &b);
    bool r = vm_throw(vm, "TypeError", "%s expects %s, got %s", fn, want, b.data ? b.data : "?");
    buf_free(&b);
    return r;
}
#define WANT_STR(i, fn)                                                                           \
    if (!IS_STRING(args[i]))                                                                      \
    return type_err(vm, fn, "a string", args[i])
#define WANT_INT(i, fn)                                                                           \
    if (!IS_INT(args[i]))                                                                         \
    return type_err(vm, fn, "an int", args[i])
#define WANT_BOOL_RESULT(v, fn)                                                                   \
    if (!IS_BOOL(v))                                                                              \
    return type_err(vm, fn, "a function returning bool", v)

static Value sv(VM *vm, const char *s, size_t n) { return val_obj(str_copy(vm, s, n)); }
static Value svc(VM *vm, const char *s) { return val_obj(str_copy(vm, s, strlen(s))); }

/* ---------------------------------------------------------------- output */

bool lib_print_value(VM *vm, FILE *f, Value v) {
    if (IS_STRING(v)) {
        fwrite(AS_CSTR(v), 1, (size_t)AS_STRING(v)->len, f);
        return true;
    }
    Buf b;
    buf_init(&b);
    if (!value_to_buf(vm, v, &b, false, 0)) {
        buf_free(&b);
        return false;
    }
    if (b.len)
        fwrite(b.data, 1, b.len, f);
    buf_free(&b);
    return true;
}
NATIVE(n_print) {
    for (int i = 0; i < argc; i++) {
        if (i)
            fputc(' ', stdout);
        if (!lib_print_value(vm, stdout, args[i]))
            return false;
    }
    fputc('\n', stdout);
    *out = val_null();
    return true;
}
static bool read_line(FILE *f, Buf *b, bool *eof) {
    int c;
    *eof = false;
    bool any = false;
    while ((c = fgetc(f)) != EOF) {
        any = true;
        if (c == '\n')
            break;
        buf_addc(b, (char)c);
    }
    if (!any)
        *eof = true;
    if (b->len && b->data[b->len - 1] == '\r')
        b->data[--b->len] = 0;
    return true;
}
NATIVE(n_input) {
    if (argc > 0) {
        if (!lib_print_value(vm, stdout, args[0]))
            return false;
    }
    fflush(stdout);
    Buf b;
    buf_init(&b);
    bool eof;
    read_line(stdin, &b, &eof);
    if (eof) {
        buf_free(&b);
        return vm_throw(vm, "InputError", "end of input");
    }
    *out = sv(vm, b.data ? b.data : "", b.len);
    buf_free(&b);
    return true;
}
static bool length_of(VM *vm, Value v, int64_t *n) {
    if (IS_STRING(v))
        *n = AS_STRING(v)->clen;
    else if (IS_LIST(v))
        *n = AS_LIST(v)->count;
    else if (IS_MAP(v) || IS_SET(v))
        *n = AS_MAP(v)->live;
    else if (IS_OBJT(v, OBJ_ELEMENT))
        *n = AS_ELEMENT(v)->children.count;
    else if (IS_OBJT(v, OBJ_JSON)) {
        Value in = ((ObjJson *)AS_OBJ(v))->v;
        if (IS_STRING(in) || IS_LIST(in) || IS_MAP(in))
            return length_of(vm, in, n);
        return vm_throw(vm, "JsonError", "this JSON value has no length");
    } else
        return type_err(vm, "len", "a string or collection", v);
    return true;
}
NATIVE(n_len) {
    UNUSED_ARGS;
    int64_t n;
    if (!length_of(vm, args[0], &n))
        return false;
    *out = val_int(n);
    return true;
}
NATIVE(n_typeof) {
    UNUSED_ARGS;
    Buf b;
    buf_init(&b);
    type_name_of(vm, args[0], &b);
    *out = sv(vm, b.data ? b.data : "", b.len);
    buf_free(&b);
    return true;
}
NATIVE(n_weak) {
    UNUSED_ARGS;
    if (IS_OBJT(args[0], OBJ_WEAK)) {
        *out = args[0];
        return true;
    }
    if (!IS_OBJ(args[0]))
        return type_err(vm, "weak", "a reference value", args[0]);
    *out = val_obj(new_weak(vm, AS_OBJ(args[0])));
    return true;
}

/* ---------------------------------------------------------------- casts */

bool to_int64(VM *vm, Value v, int64_t *out, const char *what) {
    if (IS_INT(v)) {
        *out = v.as.i;
        return true;
    }
    if (IS_DOUBLE(v)) {
        double d = v.as.d;
        if (isnan(d) || !(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
            return vm_throw(vm, "NumericError", "floating to integer cast out of range (%g)", d);
        *out = (int64_t)trunc(d);
        return true;
    }
    if (IS_BOOL(v)) {
        *out = v.as.b;
        return true;
    }
    if (IS_CHAR(v)) {
        *out = v.as.c;
        return true;
    }
    if (IS_OBJT(v, OBJ_ENUMVAL)) {
        *out = ((ObjEnumVal *)AS_OBJ(v))->index;
        return true;
    }
    if (IS_STRING(v)) {
        const char *s = AS_CSTR(v);
        while (isspace((unsigned char)*s))
            s++;
        char *end;
        errno = 0;
        long long r = strtoll(s, &end, 10);
        while (isspace((unsigned char)*end))
            end++;
        if (end == s || *end || errno == ERANGE)
            return vm_throw(vm, "NumericError", "invalid or out-of-range integer string \"%s\"", AS_CSTR(v));
        *out = r;
        return true;
    }
    if (IS_LIST(v) && AS_LIST(v)->count == 1)
        return to_int64(vm, AS_LIST(v)->items[0], out, what);
    if (IS_OBJT(v, OBJ_JSON))
        return to_int64(vm, ((ObjJson *)AS_OBJ(v))->v, out, what);
    if (IS_NULL(v))
        return vm_throw(vm, "CastError", "cannot cast null to %s", what);
    if (IS_LIST(v))
        return vm_throw(vm, "CastError", "%s: cannot convert a sequence of %d items to a number", what, AS_LIST(v)->count);
    return type_err(vm, what, "a number", v);
}
bool to_double(VM *vm, Value v, double *out, const char *what) {
    if (IS_DOUBLE(v)) {
        *out = v.as.d;
        return true;
    }
    if (IS_INT(v)) {
        *out = (double)v.as.i;
        return true;
    }
    if (IS_BOOL(v)) {
        *out = v.as.b;
        return true;
    }
    if (IS_STRING(v)) {
        const char *s = AS_CSTR(v);
        while (isspace((unsigned char)*s))
            s++;
        char *end;
        double d = strtod(s, &end);
        while (isspace((unsigned char)*end))
            end++;
        if (end == s || *end)
            return vm_throw(vm, "NumericError", "invalid or out-of-range real string \"%s\"", AS_CSTR(v));
        *out = d;
        return true;
    }
    if (IS_LIST(v) && AS_LIST(v)->count == 1)
        return to_double(vm, AS_LIST(v)->items[0], out, what);
    if (IS_OBJT(v, OBJ_JSON))
        return to_double(vm, ((ObjJson *)AS_OBJ(v))->v, out, what);
    if (IS_NULL(v))
        return vm_throw(vm, "CastError", "cannot cast null to %s", what);
    return type_err(vm, what, "a number", v);
}
bool seq_items(VM *vm, Value v, ObjList **out) {
    if (IS_LIST(v)) {
        *out = AS_LIST(v);
        return true;
    }
    if (IS_MAP(v) || IS_SET(v)) {
        *out = map_keys(vm, AS_MAP(v));
        return true;
    }
    if (IS_STRING(v)) {
        ObjString *s = AS_STRING(v);
        ObjList *l = new_list(vm, OBJ_ARRAY, s->clen);
        for (int i = 0; i < s->len;) {
            int adv;
            l->items[l->count++] = val_char(utf8_decode(s->chars + i, s->len - i, &adv));
            i += adv;
        }
        *out = l;
        return true;
    }
    if (IS_OBJT(v, OBJ_JSON)) {
        Value in = ((ObjJson *)AS_OBJ(v))->v;
        if (IS_ARRAY(in)) {
            ObjList *src = AS_LIST(in), *l = new_list(vm, OBJ_ARRAY, src->count);
            for (int i = 0; i < src->count; i++)
                l->items[l->count++] = val_obj(new_json(vm, src->items[i]));
            *out = l;
            return true;
        }
    }
    return type_err(vm, "iteration", "a sequence", v);
}

static bool cast_list(VM *vm, ObjTypeDesc *t, Value v, ObjType kind, Value *out) {
    ObjList *src = NULL;
    ObjList *l = NULL;
    if (IS_OBJT(v, OBJ_JSON) && IS_ARRAY(((ObjJson *)AS_OBJ(v))->v)) {
        if (!seq_items(vm, v, &src))
            return false;
        l = src;
        if (kind == OBJ_TUPLE)
            l->obj.type = OBJ_TUPLE;
    } else if (IS_STRING(v) || !IS_OBJ(v) || IS_OBJT(v, OBJ_INSTANCE) || IS_OBJT(v, OBJ_ELEMENT) ||
               IS_OBJT(v, OBJ_ENUMVAL) || IS_OBJT(v, OBJ_JSON) || IS_OBJT(v, OBJ_RESULT)) {
        l = new_list(vm, kind, 1);
        l->items[l->count++] = v;
    } else if (IS_MAP(v)) {
        ObjMap *m = AS_MAP(v);
        l = new_list(vm, kind, m->live);
        push_root(vm, val_obj(l));
        for (int i = 0; i < m->used; i++)
            if (m->entries[i].live) {
                ObjList *pair = new_list(vm, OBJ_TUPLE, 2);
                pair->items[0] = m->entries[i].key;
                pair->items[1] = m->entries[i].value;
                pair->count = 2;
                l->items[l->count++] = val_obj(pair);
            }
        pop_root(vm, 1);
    } else {
        if (!seq_items(vm, v, &src))
            return false;
        l = new_list(vm, kind, src->count);
        for (int i = 0; i < src->count; i++)
            l->items[l->count++] = src->items[i];
    }
    *out = val_obj(l);
    if (t && t->nargs == 1 && t->args[0]->kind != TY_ANY) {
        push_root(vm, *out);
        for (int i = 0; i < l->count; i++)
            if (!vm_cast(vm, t->args[0], l->items[i], &l->items[i])) {
                pop_root(vm, 1);
                return false;
            }
        pop_root(vm, 1);
    } else if (t && t->kind == TY_TUPLE) {
        if (l->count != t->nargs)
            return vm_throw(vm, "CastError", "expected %d items, got %d", t->nargs, l->count);
        push_root(vm, *out);
        for (int i = 0; i < l->count; i++)
            if (!vm_cast(vm, t->args[i], l->items[i], &l->items[i])) {
                pop_root(vm, 1);
                return false;
            }
        pop_root(vm, 1);
    }
    return true;
}

bool vm_cast(VM *vm, ObjTypeDesc *t, Value v, Value *out) {
    switch (t->kind) {
    case TY_ANY:
    case TY_FUNCTION:
        *out = v;
        return true;
    case TY_NULLABLE:
        if (IS_NULL(v)) {
            *out = v;
            return true;
        }
        if (t->args[0]->kind == TY_NAMED || t->args[0]->kind == TY_ERROR) {
            *out = vm_type_test(vm, v, t->args[0]) ? v : val_null();
            return true;
        }
        return vm_cast(vm, t->args[0], v, out);
    case TY_INT: {
        int64_t i;
        if (!to_int64(vm, v, &i, "int"))
            return false;
        Value r = val_int(i);
        if (!vm_coerce(vm, r, t, out, ""))
            return false;
        return true;
    }
    case TY_DOUBLE:
    case TY_FLOAT: {
        double d;
        if (!to_double(vm, v, &d, t->kind == TY_FLOAT ? "float" : "double"))
            return false;
        if (t->kind == TY_FLOAT && isfinite(d) && fabs(d) > 3.4028234663852886e38)
            return vm_throw(vm, "NumericError", "floating cast out of range (%g)", d);
        *out = val_double(t->kind == TY_FLOAT ? (double)(float)d : d);
        return true;
    }
    case TY_BOOL:
        if (IS_BOOL(v))
            *out = v;
        else if (IS_NUM(v))
            *out = val_bool(AS_NUM(v) != 0);
        else if (IS_NULL(v))
            *out = val_bool(false);
        else if (IS_STRING(v)) {
            const char *s = AS_CSTR(v);
            if (!strcmp(s, "true") || !strcmp(s, "1"))
                *out = val_bool(true);
            else if (!strcmp(s, "false") || !strcmp(s, "0"))
                *out = val_bool(false);
            else
                return vm_throw(vm, "CastError", "cannot cast \"%s\" to bool", s);
        } else if (IS_LIST(v))
            *out = val_bool(AS_LIST(v)->count > 0);
        else if (IS_MAP(v) || IS_SET(v))
            *out = val_bool(AS_MAP(v)->live > 0);
        else
            *out = val_bool(true);
        return true;
    case TY_CHAR:
        if (IS_CHAR(v)) {
            *out = v;
            return true;
        }
        if (IS_INT(v)) {
            if (v.as.i < 0 || v.as.i > 0x10ffff || (v.as.i >= 0xd800 && v.as.i <= 0xdfff))
                return vm_throw(vm, "NumericError", "%lld is not a Unicode scalar value", (long long)v.as.i);
            *out = val_char((uint32_t)v.as.i);
            return true;
        }
        if (IS_STRING(v) && AS_STRING(v)->clen == 1) {
            *out = val_char(str_char_at(AS_STRING(v), 0));
            return true;
        }
        return vm_throw(vm, "CastError", "cannot convert to char");
    case TY_STRING: {
        ObjString *s = value_to_string(vm, v);
        if (!s)
            return false;
        *out = val_obj(s);
        return true;
    }
    case TY_ARRAY:
        return cast_list(vm, t, v, OBJ_ARRAY, out);
    case TY_HTUPLE:
    case TY_TUPLE:
        return cast_list(vm, t, v, OBJ_TUPLE, out);
    case TY_SET: {
        ObjList *items = NULL;
        if (IS_STRING(v) || IS_LIST(v) || IS_MAP(v) || IS_SET(v)) {
            if (!seq_items(vm, v, &items))
                return false;
        } else {
            items = new_list(vm, OBJ_ARRAY, 1);
            items->items[items->count++] = v;
        }
        push_root(vm, val_obj(items));
        ObjMap *m = new_map(vm, OBJ_SET);
        push_root(vm, val_obj(m));
        for (int i = 0; i < items->count; i++) {
            Value item = items->items[i];
            if (t->nargs == 1 && t->args[0]->kind != TY_ANY && !vm_cast(vm, t->args[0], item, &item)) {
                pop_root(vm, 2);
                return false;
            }
            if (!map_set(vm, m, item, val_null())) {
                pop_root(vm, 2);
                return false;
            }
        }
        pop_root(vm, 2);
        *out = val_obj(m);
        return true;
    }
    case TY_MAP: {
        ObjMap *m = new_map(vm, OBJ_MAP);
        push_root(vm, val_obj(m));
        bool ok = true;
        ObjTypeDesc *kt = t->nargs == 2 ? t->args[0] : NULL, *vt = t->nargs == 2 ? t->args[1] : NULL;
        if (IS_MAP(v)) {
            ObjMap *src = AS_MAP(v);
            for (int i = 0; i < src->used && ok; i++) {
                if (!src->entries[i].live)
                    continue;
                Value k = src->entries[i].key, val = src->entries[i].value;
                if (kt && !vm_cast(vm, kt, k, &k))
                    ok = false;
                else if (vt && !vm_cast(vm, vt, val, &val))
                    ok = false;
                else
                    ok = map_set(vm, m, k, val);
            }
        } else if (IS_LIST(v)) {
            ObjList *src = AS_LIST(v);
            for (int i = 0; i < src->count && ok; i++) {
                Value p = src->items[i];
                if (!IS_LIST(p) || AS_LIST(p)->count != 2) {
                    ok = vm_throw(vm, "CastError", "map conversion needs [key, value] pairs");
                    break;
                }
                Value k = AS_LIST(p)->items[0], val = AS_LIST(p)->items[1];
                if (kt && !vm_cast(vm, kt, k, &k))
                    ok = false;
                else if (vt && !vm_cast(vm, vt, val, &val))
                    ok = false;
                else
                    ok = map_set(vm, m, k, val);
            }
        } else if (IS_OBJT(v, OBJ_JSON) && IS_MAP(((ObjJson *)AS_OBJ(v))->v)) {
            pop_root(vm, 1);
            return vm_cast(vm, t, ((ObjJson *)AS_OBJ(v))->v, out);
        } else
            ok = vm_throw(vm, "CastError", "cannot convert this value to a map");
        pop_root(vm, 1);
        *out = val_obj(m);
        return ok;
    }
    case TY_JSON:
        *out = IS_OBJT(v, OBJ_JSON) ? v : val_obj(new_json(vm, v));
        return true;
    default:
        if (vm_type_test(vm, v, t)) {
            *out = v;
            return true;
        }
        {
            Buf b;
            buf_init(&b);
            type_name_of(vm, v, &b);
            bool r = vm_throw(vm, "CastError", "cannot cast %s to %s", b.data ? b.data : "value",
                              t->name ? t->name->chars : "that type");
            buf_free(&b);
            return r;
        }
    }
}

/* Cast natives carry their target type in `data`, published through vm->desc_args[0]. */
NATIVE(n_cast) {
    Value tv = vm->desc_args[0];
    ObjTypeDesc *t = AS_TYPE(tv);
    if (argc == 0) {
        switch (t->kind) {
        case TY_ARRAY: *out = val_obj(new_list(vm, OBJ_ARRAY, 0)); return true;
        case TY_TUPLE:
        case TY_HTUPLE: *out = val_obj(new_list(vm, OBJ_TUPLE, 0)); return true;
        case TY_MAP: *out = val_obj(new_map(vm, OBJ_MAP)); return true;
        case TY_SET: *out = val_obj(new_map(vm, OBJ_SET)); return true;
        case TY_STRING: *out = svc(vm, ""); return true;
        case TY_INT: *out = val_int(0); return true;
        case TY_DOUBLE:
        case TY_FLOAT: *out = val_double(0); return true;
        case TY_BOOL: *out = val_bool(false); return true;
        default: return vm_throw(vm, "TypeError", "this conversion needs a value");
        }
    }
    return vm_cast(vm, t, args[0], out);
}

/* ---------------------------------------------------------------- errors and Result */

/* ---------------------------------------------------------------- type(x), unit(x), unitns(x) */

static bool quantity_method(VM *vm, Value v, const char *method, Value *out, bool *found) {
    *found = false;
    char ns[96];
    bool real;
    if (!IS_OBJT(v, OBJ_INSTANCE) || !quantity_class_name(AS_INSTANCE(v)->klass->name->chars, ns, sizeof ns, &real))
        return true;
    Value m = vm_find_method(AS_INSTANCE(v)->klass, str_intern_c(vm, method));
    if (IS_NULL(m))
        return true;
    *found = true;
    ObjBound *bound = new_bound(vm, v, m);
    push_root(vm, val_obj(bound));
    bool ok = vm_call(vm, val_obj(bound), 0, NULL, out);
    pop_root(vm, 1);
    return ok;
}
NATIVE(n_type) {
    bool found;
    if (!quantity_method(vm, args[0], "__fwu_type", out, &found))
        return false;
    if (found)
        return true;
    Buf b;
    buf_init(&b);
    type_name_of(vm, args[0], &b);
    *out = val_obj(str_copy(vm, b.data ? b.data : "", b.len));
    buf_free(&b);
    return true;
}
static bool quantity_info(VM *vm, Value v, const char *method, const char *what, Value *out) {
    bool found;
    if (!quantity_method(vm, v, method, out, &found))
        return false;
    if (found)
        return true;
    Buf b;
    buf_init(&b);
    type_name_of(vm, v, &b);
    bool r = vm_throw(vm, "TypeError", "%s(x) needs a quantity such as 15s, got %s", what, b.data ? b.data : "value");
    buf_free(&b);
    return r;
}
NATIVE(n_unit) { return quantity_info(vm, args[0], "__fwu_unit_name", "unit", out); }
NATIVE(n_unitns) { return quantity_info(vm, args[0], "__fwu_ns", "unitns", out); }
NATIVE(n_quantity_in) { return vm_quantity_in(vm, args[0], args[1], out); }

NATIVE(n_error_ctor) {
    ObjString *type = AS_STRING(vm->desc_args[0]);
    ObjString *msg;
    if (argc == 0)
        msg = str_copy(vm, "", 0);
    else {
        msg = value_to_string(vm, args[0]);
        if (!msg)
            return false;
    }
    *out = val_obj(new_error(vm, type, msg));
    return true;
}
NATIVE(n_result_ok) {
    *out = val_obj(new_result(vm, true, argc ? args[0] : val_null()));
    return true;
}
NATIVE(n_result_error) {
    *out = val_obj(new_result(vm, false, argc ? args[0] : val_null()));
    return true;
}

/* ---------------------------------------------------------------- strings */

static uint32_t case_map(uint32_t c, bool upper) {
    if (c < 0x80)
        return upper ? (uint32_t)toupper((int)c) : (uint32_t)tolower((int)c);
    if (upper) {
        if ((c >= 0xe0 && c <= 0xfe && c != 0xf7))
            return c - 0x20;
        if (c == 0xff)
            return 0x178;
        if (c >= 0x100 && c <= 0x137 && (c & 1))
            return c - 1;
        if (c >= 0x139 && c <= 0x148 && !(c & 1))
            return c - 1;
        if (c >= 0x14a && c <= 0x177 && (c & 1))
            return c - 1;
        if ((c == 0x17a || c == 0x17c || c == 0x17e))
            return c - 1;
        if (c >= 0x3b1 && c <= 0x3c9 && c != 0x3c2)
            return c - 0x20;
        if (c == 0x3c2)
            return 0x3a3;
        if (c >= 0x430 && c <= 0x44f)
            return c - 0x20;
        if (c >= 0x450 && c <= 0x45f)
            return c - 0x50;
    } else {
        if ((c >= 0xc0 && c <= 0xde && c != 0xd7))
            return c + 0x20;
        if (c == 0x178)
            return 0xff;
        if (c >= 0x100 && c <= 0x137 && !(c & 1))
            return c + 1;
        if (c >= 0x139 && c <= 0x148 && (c & 1))
            return c + 1;
        if (c >= 0x14a && c <= 0x177 && !(c & 1))
            return c + 1;
        if ((c == 0x179 || c == 0x17b || c == 0x17d))
            return c + 1;
        if (c >= 0x391 && c <= 0x3a9 && c != 0x3a2)
            return c + 0x20;
        if (c >= 0x410 && c <= 0x42f)
            return c + 0x20;
        if (c >= 0x400 && c <= 0x40f)
            return c + 0x50;
    }
    return c;
}
static bool uni_space(uint32_t c) {
    return c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f ||
           c == 0x3000;
}
static ObjString *S(Value v) { return AS_STRING(v); }

static bool needle(VM *vm, Value v, Buf *b, const char *fn) {
    buf_init(b);
    if (IS_STRING(v)) {
        buf_add(b, AS_CSTR(v), (size_t)S(v)->len);
        return true;
    }
    if (IS_CHAR(v)) {
        buf_utf8(b, v.as.c);
        return true;
    }
    return type_err(vm, fn, "a string or char", v);
}
static const char *memfind(const char *h, size_t hn, const char *n, size_t nn) {
    if (nn == 0)
        return h;
    if (nn > hn)
        return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (h[i] == n[0] && !memcmp(h + i, n, nn))
            return h + i;
    return NULL;
}
static int char_index_of_byte(ObjString *s, int byte) { return utf8_count(s->chars, (size_t)byte); }

NATIVE(s_upper) {
    UNUSED_ARGS;
    ObjString *s = S(args[0]);
    Buf b;
    buf_init(&b);
    for (int i = 0; i < s->len;) {
        int adv;
        uint32_t c = utf8_decode(s->chars + i, s->len - i, &adv);
        buf_utf8(&b, case_map(c, true));
        i += adv;
    }
    *out = sv(vm, b.data ? b.data : "", b.len);
    buf_free(&b);
    return true;
}
NATIVE(s_lower) {
    UNUSED_ARGS;
    ObjString *s = S(args[0]);
    Buf b;
    buf_init(&b);
    for (int i = 0; i < s->len;) {
        int adv;
        uint32_t c = utf8_decode(s->chars + i, s->len - i, &adv);
        buf_utf8(&b, case_map(c, false));
        i += adv;
    }
    *out = sv(vm, b.data ? b.data : "", b.len);
    buf_free(&b);
    return true;
}
static bool trim_impl(VM *vm, Value self, bool start, bool end, Value *out) {
    ObjString *s = S(self);
    int a = 0, z = s->len;
    if (start)
        while (a < z) {
            int adv;
            uint32_t c = utf8_decode(s->chars + a, z - a, &adv);
            if (!uni_space(c))
                break;
            a += adv;
        }
    if (end)
        while (z > a) {
            int p = z - 1;
            while (p > a && ((unsigned char)s->chars[p] & 0xc0) == 0x80)
                p--;
            int adv;
            uint32_t c = utf8_decode(s->chars + p, z - p, &adv);
            if (!uni_space(c))
                break;
            z = p;
        }
    *out = sv(vm, s->chars + a, (size_t)(z - a));
    return true;
}
NATIVE(s_trim) {
    UNUSED_ARGS;
    return trim_impl(vm, args[0], true, true, out);
}
NATIVE(s_trim_start) {
    UNUSED_ARGS;
    return trim_impl(vm, args[0], true, false, out);
}
NATIVE(s_trim_end) {
    UNUSED_ARGS;
    return trim_impl(vm, args[0], false, true, out);
}
NATIVE(s_split) {
    ObjString *s = S(args[0]);
    ObjList *l = new_list(vm, OBJ_ARRAY, 0);
    push_root(vm, val_obj(l));
    if (argc < 2) {
        int i = 0;
        while (i < s->len) {
            int adv;
            while (i < s->len && uni_space(utf8_decode(s->chars + i, s->len - i, &adv)))
                i += adv;
            int start = i;
            while (i < s->len && !uni_space(utf8_decode(s->chars + i, s->len - i, &adv)))
                i += adv;
            if (i > start)
                list_push(vm, l, sv(vm, s->chars + start, (size_t)(i - start)));
        }
    } else {
        Buf sep;
        if (!needle(vm, args[1], &sep, "split")) {
            pop_root(vm, 1);
            return false;
        }
        if (sep.len == 0) {
            for (int i = 0; i < s->len;) {
                int adv;
                utf8_decode(s->chars + i, s->len - i, &adv);
                list_push(vm, l, sv(vm, s->chars + i, (size_t)adv));
                i += adv;
            }
        } else {
            const char *p = s->chars, *end = s->chars + s->len;
            for (;;) {
                const char *f = memfind(p, (size_t)(end - p), sep.data, sep.len);
                if (!f) {
                    list_push(vm, l, sv(vm, p, (size_t)(end - p)));
                    break;
                }
                list_push(vm, l, sv(vm, p, (size_t)(f - p)));
                p = f + sep.len;
            }
        }
        buf_free(&sep);
    }
    pop_root(vm, 1);
    *out = val_obj(l);
    return true;
}
NATIVE(s_lines) {
    UNUSED_ARGS;
    ObjString *s = S(args[0]);
    ObjList *l = new_list(vm, OBJ_ARRAY, 0);
    push_root(vm, val_obj(l));
    int start = 0;
    for (int i = 0; i < s->len; i++) {
        char c = s->chars[i];
        if (c == '\n' || c == '\r') {
            list_push(vm, l, sv(vm, s->chars + start, (size_t)(i - start)));
            if (c == '\r' && i + 1 < s->len && s->chars[i + 1] == '\n')
                i++;
            start = i + 1;
        }
    }
    if (start < s->len)
        list_push(vm, l, sv(vm, s->chars + start, (size_t)(s->len - start)));
    pop_root(vm, 1);
    *out = val_obj(l);
    return true;
}
NATIVE(s_chars) {
    UNUSED_ARGS;
    ObjList *l = NULL;
    if (!seq_items(vm, args[0], &l))
        return false;
    *out = val_obj(l);
    return true;
}
NATIVE(s_find) {
    UNUSED_ARGS;
    ObjString *s = S(args[0]);
    Buf n;
    if (!needle(vm, args[1], &n, "find"))
        return false;
    const char *f = memfind(s->chars, (size_t)s->len, n.data ? n.data : "", n.len);
    *out = val_int(f ? char_index_of_byte(s, (int)(f - s->chars)) : -1);
    buf_free(&n);
    return true;
}
NATIVE(s_find_last) {
    UNUSED_ARGS;
    ObjString *s = S(args[0]);
    Buf n;
    if (!needle(vm, args[1], &n, "find_last"))
        return false;
    int found = -1;
    for (int i = s->len - (int)n.len; i >= 0; i--)
        if (!memcmp(s->chars + i, n.data ? n.data : "", n.len)) {
            found = char_index_of_byte(s, i);
            break;
        }
    *out = val_int(found);
    buf_free(&n);
    return true;
}
NATIVE(s_contains) {
    UNUSED_ARGS;
    ObjString *s = S(args[0]);
    Buf n;
    if (!needle(vm, args[1], &n, "contains"))
        return false;
    *out = val_bool(memfind(s->chars, (size_t)s->len, n.data ? n.data : "", n.len) != NULL);
    buf_free(&n);
    return true;
}
NATIVE(s_starts_with) {
    UNUSED_ARGS;
    ObjString *s = S(args[0]);
    Buf n;
    if (!needle(vm, args[1], &n, "starts_with"))
        return false;
    *out = val_bool(n.len <= (size_t)s->len && !memcmp(s->chars, n.data ? n.data : "", n.len));
    buf_free(&n);
    return true;
}
NATIVE(s_ends_with) {
    UNUSED_ARGS;
    ObjString *s = S(args[0]);
    Buf n;
    if (!needle(vm, args[1], &n, "ends_with"))
        return false;
    *out = val_bool(n.len <= (size_t)s->len && !memcmp(s->chars + s->len - n.len, n.data ? n.data : "", n.len));
    buf_free(&n);
    return true;
}
NATIVE(s_count) {
    UNUSED_ARGS;
    ObjString *s = S(args[0]);
    Buf n;
    if (!needle(vm, args[1], &n, "count"))
        return false;
    int64_t c = 0;
    if (n.len) {
        const char *p = s->chars, *end = s->chars + s->len;
        const char *f;
        while ((f = memfind(p, (size_t)(end - p), n.data, n.len))) {
            c++;
            p = f + n.len;
        }
    }
    *out = val_int(c);
    buf_free(&n);
    return true;
}
NATIVE(s_replace) {
    UNUSED_ARGS;
    ObjString *s = S(args[0]);
    Buf from, to;
    if (!needle(vm, args[1], &from, "replace"))
        return false;
    if (!needle(vm, args[2], &to, "replace")) {
        buf_free(&from);
        return false;
    }
    if (!from.len) {
        *out = args[0];
        buf_free(&from);
        buf_free(&to);
        return true;
    }
    Buf r;
    buf_init(&r);
    const char *p = s->chars, *end = s->chars + s->len, *f;
    while ((f = memfind(p, (size_t)(end - p), from.data, from.len))) {
        buf_add(&r, p, (size_t)(f - p));
        buf_add(&r, to.data ? to.data : "", to.len);
        p = f + from.len;
    }
    buf_add(&r, p, (size_t)(end - p));
    *out = sv(vm, r.data ? r.data : "", r.len);
    buf_free(&r);
    buf_free(&from);
    buf_free(&to);
    return true;
}
static bool pad_impl(VM *vm, int argc, Value *args, Value *out, bool start) {
    WANT_INT(1, start ? "pad_start" : "pad_end");
    ObjString *s = S(args[0]);
    Buf fill;
    buf_init(&fill);
    if (argc > 2) {
        buf_free(&fill);
        if (!needle(vm, args[2], &fill, "pad"))
            return false;
    } else
        buf_addc(&fill, ' ');
    int64_t width = args[1].as.i;
    if (width <= s->clen || !fill.len) {
        *out = args[0];
        buf_free(&fill);
        return true;
    }
    int fill_chars = utf8_count(fill.data, fill.len);
    int64_t need = width - s->clen;
    Buf pad;
    buf_init(&pad);
    int64_t added = 0;
    while (added < need) {
        /* add one char of fill at a time */
        size_t off = 0;
        for (int k = 0; k < fill_chars && added < need; k++) {
            int adv;
            utf8_decode(fill.data + off, (int)(fill.len - off), &adv);
            buf_add(&pad, fill.data + off, (size_t)adv);
            off += (size_t)adv;
            added++;
        }
    }
    Buf r;
    buf_init(&r);
    if (start) {
        buf_add(&r, pad.data, pad.len);
        buf_add(&r, s->chars, (size_t)s->len);
    } else {
        buf_add(&r, s->chars, (size_t)s->len);
        buf_add(&r, pad.data, pad.len);
    }
    *out = sv(vm, r.data, r.len);
    buf_free(&r);
    buf_free(&pad);
    buf_free(&fill);
    return true;
}
NATIVE(s_pad_start) { return pad_impl(vm, argc, args, out, true); }
NATIVE(s_pad_end) { return pad_impl(vm, argc, args, out, false); }
NATIVE(s_reverse) {
    UNUSED_ARGS;
    ObjString *s = S(args[0]);
    char *r = xmalloc((size_t)s->len + 1);
    int w = s->len;
    for (int i = 0; i < s->len;) {
        int adv;
        utf8_decode(s->chars + i, s->len - i, &adv);
        w -= adv;
        memcpy(r + w, s->chars + i, (size_t)adv);
        i += adv;
    }
    *out = val_obj(str_take_buf(vm, r, (size_t)s->len));
    return true;
}
NATIVE(s_repeat) {
    UNUSED_ARGS;
    WANT_INT(1, "repeat");
    if (args[1].as.i < 0)
        return vm_throw(vm, "ValueError", "negative repetition count");
    ObjString *s = S(args[0]);
    size_t total = (size_t)s->len * (size_t)args[1].as.i;
    char *r = xmalloc(total + 1);
    for (int64_t k = 0; k < args[1].as.i; k++)
        memcpy(r + (size_t)k * (size_t)s->len, s->chars, (size_t)s->len);
    *out = val_obj(str_take_buf(vm, r, total));
    return true;
}
NATIVE(s_is_empty) {
    UNUSED_ARGS;
    *out = val_bool(S(args[0])->len == 0);
    return true;
}
static bool join_items(VM *vm, Value seq, const char *sep, size_t sepn, Value *out) {
    ObjList *items = NULL;
    if (!seq_items(vm, seq, &items))
        return false;
    push_root(vm, val_obj(items));
    Buf b;
    buf_init(&b);
    for (int i = 0; i < items->count; i++) {
        if (i)
            buf_add(&b, sep, sepn);
        if (!value_to_buf(vm, items->items[i], &b, false, 0)) {
            buf_free(&b);
            pop_root(vm, 1);
            return false;
        }
    }
    pop_root(vm, 1);
    *out = sv(vm, b.data ? b.data : "", b.len);
    buf_free(&b);
    return true;
}
NATIVE(s_join) {
    UNUSED_ARGS;
    return join_items(vm, args[1], AS_CSTR(args[0]), (size_t)S(args[0])->len, out);
}
static bool slice_bounds(VM *vm, int argc, Value *args, int64_t n, int64_t *a, int64_t *b) {
    WANT_INT(1, "slice");
    *a = args[1].as.i;
    *b = n;
    if (argc > 2) {
        WANT_INT(2, "slice");
        *b = args[2].as.i;
    }
    if (*a < 0)
        *a += n;
    if (*b < 0)
        *b += n;
    if (*a < 0)
        *a = 0;
    if (*b > n)
        *b = n;
    if (*a > n)
        *a = n;
    if (*b < *a)
        *b = *a;
    return true;
}
NATIVE(s_slice) {
    ObjString *s = S(args[0]);
    int64_t a = 0, b = 0;
    if (!slice_bounds(vm, argc, args, s->clen, &a, &b))
        return false;
    int ba = utf8_offset(s, (int)a), bb = utf8_offset(s, (int)b);
    *out = sv(vm, s->chars + ba, (size_t)(bb - ba));
    return true;
}
NATIVE(s_bytes) {
    UNUSED_ARGS;
    ObjString *s = S(args[0]);
    ObjList *l = new_list(vm, OBJ_ARRAY, s->len);
    for (int i = 0; i < s->len; i++)
        l->items[l->count++] = val_int((unsigned char)s->chars[i]);
    *out = val_obj(l);
    return true;
}
NATIVE(any_to_string) {
    UNUSED_ARGS;
    ObjString *s = value_to_string(vm, args[0]);
    if (!s)
        return false;
    *out = val_obj(s);
    return true;
}

/* chars */
NATIVE(c_is_digit) {
    UNUSED_ARGS;
    *out = val_bool(args[0].as.c >= '0' && args[0].as.c <= '9');
    return true;
}
NATIVE(c_is_letter) {
    UNUSED_ARGS;
    uint32_t c = args[0].as.c;
    *out = val_bool(c < 0x80 ? isalpha((int)c) != 0 : case_map(c, true) != c || case_map(c, false) != c || c >= 0x4e00);
    return true;
}
NATIVE(c_is_space) {
    UNUSED_ARGS;
    *out = val_bool(uni_space(args[0].as.c));
    return true;
}
NATIVE(c_is_upper) {
    UNUSED_ARGS;
    uint32_t c = args[0].as.c;
    *out = val_bool(case_map(c, false) != c);
    return true;
}
NATIVE(c_is_lower) {
    UNUSED_ARGS;
    uint32_t c = args[0].as.c;
    *out = val_bool(case_map(c, true) != c);
    return true;
}
NATIVE(c_upper) {
    UNUSED_ARGS;
    *out = val_char(case_map(args[0].as.c, true));
    return true;
}
NATIVE(c_lower) {
    UNUSED_ARGS;
    *out = val_char(case_map(args[0].as.c, false));
    return true;
}
NATIVE(c_code) {
    UNUSED_ARGS;
    *out = val_int(args[0].as.c);
    return true;
}

/* ---------------------------------------------------------------- arrays */

static bool mutable_list(VM *vm, Value v, const char *fn) {
    if (IS_TUPLE(v))
        return vm_throw(vm, "TypeError", "%s needs an array; tuples have fixed length", fn);
    if (AS_LIST(v)->frozen)
        return vm_throw(vm, "ValueError", "cannot modify a frozen array");
    return true;
}
#define MUTABLE(fn)                                                                               \
    if (!mutable_list(vm, args[0], fn))                                                           \
    return false
static ObjList *L(Value v) { return AS_LIST(v); }

NATIVE(a_push) {
    MUTABLE("push");
    for (int i = 1; i < argc; i++)
        list_push(vm, L(args[0]), args[i]);
    *out = val_null();
    return true;
}
NATIVE(a_pop) {
    UNUSED_ARGS;
    MUTABLE("pop");
    ObjList *l = L(args[0]);
    if (!l->count)
        return vm_throw(vm, "IndexError", "pop from an empty array");
    *out = l->items[--l->count];
    return true;
}
NATIVE(a_insert) {
    UNUSED_ARGS;
    MUTABLE("insert");
    WANT_INT(1, "insert");
    ObjList *l = L(args[0]);
    int64_t i = args[1].as.i;
    if (i < 0 || i > l->count)
        return vm_throw(vm, "IndexError", "insert position %lld out of range for length %d", (long long)i, l->count);
    list_push(vm, l, val_null());
    memmove(l->items + i + 1, l->items + i, sizeof(Value) * (size_t)(l->count - 1 - i));
    l->items[i] = args[2];
    *out = val_null();
    return true;
}
NATIVE(a_remove_at) {
    UNUSED_ARGS;
    MUTABLE("remove_at");
    WANT_INT(1, "remove_at");
    ObjList *l = L(args[0]);
    int64_t i = args[1].as.i;
    if (i < 0 || i >= l->count)
        return vm_throw(vm, "IndexError", "remove_at index out of bounds (index %lld, length %d)", (long long)i, l->count);
    *out = l->items[i];
    memmove(l->items + i, l->items + i + 1, sizeof(Value) * (size_t)(l->count - 1 - i));
    l->count--;
    return true;
}
NATIVE(a_remove) {
    UNUSED_ARGS;
    MUTABLE("remove");
    ObjList *l = L(args[0]);
    for (int i = 0; i < l->count; i++) {
        bool eq;
        if (!values_equal(vm, l->items[i], args[1], &eq))
            return false;
        if (eq) {
            memmove(l->items + i, l->items + i + 1, sizeof(Value) * (size_t)(l->count - 1 - i));
            l->count--;
            *out = val_bool(true);
            return true;
        }
    }
    *out = val_bool(false);
    return true;
}
NATIVE(a_clear) {
    UNUSED_ARGS;
    MUTABLE("clear");
    L(args[0])->count = 0;
    *out = val_null();
    return true;
}
NATIVE(a_extend) {
    UNUSED_ARGS;
    MUTABLE("extend");
    ObjList *items = NULL;
    if (!seq_items(vm, args[1], &items))
        return false;
    int n = items->count;
    for (int i = 0; i < n; i++)
        list_push(vm, L(args[0]), items->items[i]);
    *out = val_null();
    return true;
}
static bool sort_less(VM *vm, Value f, Value a, Value b, bool *less) {
    if (IS_NULL(f)) {
        int c;
        if (!value_compare(vm, a, b, &c))
            return false;
        *less = c < 0;
        return true;
    }
    Value pair[2] = {a, b}, r;
    if (!vm_call(vm, f, 2, pair, &r))
        return false;
    if (!IS_BOOL(r))
        return vm_throw(vm, "TypeError", "sort comparators must return bool");
    *less = r.as.b;
    return true;
}
static bool merge_sort(VM *vm, Value *a, Value *tmp, int n, Value f) {
    if (n < 2)
        return true;
    if (n <= 12) {
        for (int i = 1; i < n; i++) {
            Value x = a[i];
            int j = i - 1;
            while (j >= 0) {
                bool less = false;
                if (!sort_less(vm, f, x, a[j], &less))
                    return false;
                if (!less)
                    break;
                a[j + 1] = a[j];
                j--;
            }
            a[j + 1] = x;
        }
        return true;
    }
    int m = n / 2;
    if (!merge_sort(vm, a, tmp, m, f) || !merge_sort(vm, a + m, tmp, n - m, f))
        return false;
    int i = 0, j = m, k = 0;
    while (i < m && j < n) {
        bool less = false;
        if (!sort_less(vm, f, a[j], a[i], &less))
            return false;
        tmp[k++] = less ? a[j++] : a[i++];
    }
    while (i < m)
        tmp[k++] = a[i++];
    while (j < n)
        tmp[k++] = a[j++];
    memcpy(a, tmp, sizeof(Value) * (size_t)n);
    return true;
}
NATIVE(a_sort) {
    MUTABLE("sort");
    ObjList *l = L(args[0]);
    int n = l->count;
    /* sort a rooted copy so callbacks cannot invalidate it */
    ObjList *copy = new_list(vm, OBJ_ARRAY, n);
    memcpy(copy->items, l->items, sizeof(Value) * (size_t)n);
    copy->count = n;
    push_root(vm, val_obj(copy));
    ObjList *tmp = new_list(vm, OBJ_ARRAY, n);
    tmp->count = n;
    for (int i = 0; i < n; i++)
        tmp->items[i] = val_null();
    push_root(vm, val_obj(tmp));
    bool ok = merge_sort(vm, copy->items, tmp->items, n, argc > 1 ? args[1] : val_null());
    pop_root(vm, 2);
    if (!ok)
        return false;
    if (l->count == n)
        memcpy(l->items, copy->items, sizeof(Value) * (size_t)n);
    *out = val_null();
    return true;
}
NATIVE(a_reverse) {
    UNUSED_ARGS;
    MUTABLE("reverse");
    ObjList *l = L(args[0]);
    for (int i = 0, j = l->count - 1; i < j; i++, j--) {
        Value t = l->items[i];
        l->items[i] = l->items[j];
        l->items[j] = t;
    }
    *out = val_null();
    return true;
}
NATIVE(a_slice) {
    ObjList *l = L(args[0]);
    int64_t a = 0, b = 0;
    if (!slice_bounds(vm, argc, args, l->count, &a, &b))
        return false;
    ObjList *r = new_list(vm, (ObjType)OBJ_TYPE(args[0]), (int)(b - a));
    for (int64_t i = a; i < b; i++)
        r->items[r->count++] = l->items[i];
    *out = val_obj(r);
    return true;
}
NATIVE(a_index_of) {
    UNUSED_ARGS;
    ObjList *l = L(args[0]);
    for (int i = 0; i < l->count; i++) {
        bool eq;
        if (!values_equal(vm, l->items[i], args[1], &eq))
            return false;
        if (eq) {
            *out = val_int(i);
            return true;
        }
    }
    *out = val_int(-1);
    return true;
}
NATIVE(a_contains) {
    Value idx;
    if (!a_index_of(vm, argc, args, &idx))
        return false;
    *out = val_bool(idx.as.i >= 0);
    return true;
}
NATIVE(a_copy) {
    UNUSED_ARGS;
    ObjList *l = L(args[0]);
    ObjList *r = new_list(vm, (ObjType)OBJ_TYPE(args[0]), l->count);
    memcpy(r->items, l->items, sizeof(Value) * (size_t)l->count);
    r->count = l->count;
    *out = val_obj(r);
    return true;
}
NATIVE(a_join) {
    if (argc > 1)
        WANT_STR(1, "join");
    const char *sep = argc > 1 ? AS_CSTR(args[1]) : "";
    return join_items(vm, args[0], sep, argc > 1 ? (size_t)S(args[1])->len : 0, out);
}
NATIVE(a_map) {
    UNUSED_ARGS;
    ObjList *l = L(args[0]);
    ObjList *r = new_list(vm, OBJ_ARRAY, l->count);
    push_root(vm, val_obj(r));
    for (int i = 0; i < l->count; i++) {
        Value v;
        if (!vm_call(vm, args[1], 1, &l->items[i], &v)) {
            pop_root(vm, 1);
            return false;
        }
        list_push(vm, r, v);
    }
    pop_root(vm, 1);
    *out = val_obj(r);
    return true;
}
NATIVE(a_filter) {
    UNUSED_ARGS;
    ObjList *l = L(args[0]);
    ObjList *r = new_list(vm, (ObjType)OBJ_TYPE(args[0]), 0);
    push_root(vm, val_obj(r));
    for (int i = 0; i < l->count; i++) {
        Value v, item = l->items[i];
        if (!vm_call(vm, args[1], 1, &item, &v)) {
            pop_root(vm, 1);
            return false;
        }
        if (!IS_BOOL(v)) {
            pop_root(vm, 1);
            return vm_throw(vm, "TypeError", "filter functions must return bool");
        }
        if (v.as.b)
            list_push(vm, r, item);
    }
    pop_root(vm, 1);
    *out = val_obj(r);
    return true;
}
NATIVE(a_reduce) {
    UNUSED_ARGS;
    ObjList *l = L(args[0]);
    Value acc = args[1];
    for (int i = 0; i < l->count; i++) {
        Value pair[2] = {acc, l->items[i]};
        push_root(vm, acc);
        bool ok = vm_call(vm, args[2], 2, pair, &acc);
        pop_root(vm, 1);
        if (!ok)
            return false;
    }
    *out = acc;
    return true;
}
static bool any_all(VM *vm, Value *args, Value *out, bool want_any) {
    ObjList *l = L(args[0]);
    for (int i = 0; i < l->count; i++) {
        Value v;
        if (!vm_call(vm, args[1], 1, &l->items[i], &v))
            return false;
        if (!IS_BOOL(v))
            return vm_throw(vm, "TypeError", "predicates must return bool");
        if (v.as.b == want_any) {
            *out = val_bool(want_any);
            return true;
        }
    }
    *out = val_bool(!want_any);
    return true;
}
NATIVE(a_any) {
    UNUSED_ARGS;
    return any_all(vm, args, out, true);
}
NATIVE(a_all) {
    UNUSED_ARGS;
    return any_all(vm, args, out, false);
}
/* `items which condition`: the parser passes the condition as a one-parameter lambda. Arrays,
 * tuples and sets filter to a new array. */
NATIVE(a_which) {
    UNUSED_ARGS;
    Value src = args[0];
    if (!IS_OBJ(src) || (OBJ_TYPE(src) != OBJ_ARRAY && OBJ_TYPE(src) != OBJ_TUPLE &&
                         OBJ_TYPE(src) != OBJ_SET))
        return vm_throw(vm, "TypeError", "'which' filters an array, tuple, or set");
    ObjList *l = OBJ_TYPE(src) == OBJ_SET ? map_keys(vm, AS_MAP(src)) : L(src);
    push_root(vm, val_obj(l));
    ObjList *r = new_list(vm, OBJ_ARRAY, 0);
    push_root(vm, val_obj(r));
    for (int i = 0; i < l->count; i++) {
        Value v, item = l->items[i];
        if (!vm_call(vm, args[1], 1, &item, &v)) {
            pop_root(vm, 2);
            return false;
        }
        if (!IS_BOOL(v)) {
            pop_root(vm, 2);
            return vm_throw(vm, "TypeError", "'which' condition must be bool");
        }
        if (v.as.b)
            list_push(vm, r, item);
    }
    pop_root(vm, 2);
    *out = val_obj(r);
    return true;
}
NATIVE(a_each) {
    UNUSED_ARGS;
    ObjList *l = L(args[0]);
    for (int i = 0; i < l->count; i++) {
        Value v;
        if (!vm_call(vm, args[1], 1, &l->items[i], &v))
            return false;
    }
    *out = val_null();
    return true;
}
NATIVE(a_freeze) {
    UNUSED_ARGS;
    if (IS_LIST(args[0]))
        L(args[0])->frozen = true;
    else
        AS_MAP(args[0])->frozen = true;
    *out = args[0];
    return true;
}
NATIVE(a_is_frozen) {
    UNUSED_ARGS;
    *out = val_bool(IS_LIST(args[0]) ? L(args[0])->frozen : AS_MAP(args[0])->frozen);
    return true;
}
NATIVE(a_is_empty) {
    UNUSED_ARGS;
    *out = val_bool(IS_LIST(args[0]) ? L(args[0])->count == 0 : AS_MAP(args[0])->live == 0);
    return true;
}
NATIVE(a_first) {
    UNUSED_ARGS;
    if (!L(args[0])->count)
        return vm_throw(vm, "IndexError", "first of an empty sequence");
    *out = L(args[0])->items[0];
    return true;
}
NATIVE(a_last) {
    UNUSED_ARGS;
    if (!L(args[0])->count)
        return vm_throw(vm, "IndexError", "last of an empty sequence");
    *out = L(args[0])->items[L(args[0])->count - 1];
    return true;
}

/* ---------------------------------------------------------------- maps and sets */

static bool mutable_map(VM *vm, Value v) {
    if (AS_MAP(v)->frozen)
        return vm_throw(vm, "ValueError", "cannot modify a frozen %s", IS_SET(v) ? "set" : "map");
    return true;
}
NATIVE(m_keys) {
    UNUSED_ARGS;
    *out = val_obj(map_keys(vm, AS_MAP(args[0])));
    return true;
}
NATIVE(m_values) {
    UNUSED_ARGS;
    *out = val_obj(map_values(vm, AS_MAP(args[0])));
    return true;
}
NATIVE(m_items) {
    UNUSED_ARGS;
    ObjMap *m = AS_MAP(args[0]);
    if (IS_SET(args[0])) {
        *out = val_obj(map_keys(vm, m));
        return true;
    }
    ObjList *l = new_list(vm, OBJ_ARRAY, m->live);
    push_root(vm, val_obj(l));
    for (int i = 0; i < m->used; i++)
        if (m->entries[i].live) {
            ObjList *p = new_list(vm, OBJ_TUPLE, 2);
            p->items[0] = m->entries[i].key;
            p->items[1] = m->entries[i].value;
            p->count = 2;
            l->items[l->count++] = val_obj(p);
        }
    pop_root(vm, 1);
    *out = val_obj(l);
    return true;
}
NATIVE(m_remove) {
    UNUSED_ARGS;
    if (!mutable_map(vm, args[0]))
        return false;
    bool found;
    if (!map_remove(vm, AS_MAP(args[0]), args[1], &found))
        return false;
    *out = val_bool(found);
    return true;
}
NATIVE(m_contains) {
    UNUSED_ARGS;
    bool found;
    if (!map_has(vm, AS_MAP(args[0]), args[1], &found))
        return false;
    *out = val_bool(found);
    return true;
}
NATIVE(m_get) {
    bool found;
    if (!map_get(vm, AS_MAP(args[0]), args[1], out, &found))
        return false;
    if (!found)
        *out = argc > 2 ? args[2] : val_null();
    return true;
}
NATIVE(m_clear) {
    UNUSED_ARGS;
    if (!mutable_map(vm, args[0]))
        return false;
    map_clear(vm, AS_MAP(args[0]));
    *out = val_null();
    return true;
}
NATIVE(m_copy) {
    UNUSED_ARGS;
    ObjMap *src = AS_MAP(args[0]);
    ObjMap *m = new_map(vm, (ObjType)OBJ_TYPE(args[0]));
    push_root(vm, val_obj(m));
    for (int i = 0; i < src->used; i++)
        if (src->entries[i].live && !map_set(vm, m, src->entries[i].key, src->entries[i].value)) {
            pop_root(vm, 1);
            return false;
        }
    pop_root(vm, 1);
    *out = val_obj(m);
    return true;
}
NATIVE(set_add) {
    UNUSED_ARGS;
    if (!mutable_map(vm, args[0]))
        return false;
    bool found;
    if (!map_has(vm, AS_MAP(args[0]), args[1], &found))
        return false;
    if (!found && !map_set(vm, AS_MAP(args[0]), args[1], val_null()))
        return false;
    *out = val_bool(!found);
    return true;
}
static bool set_op(VM *vm, Value *args, Value *out, int op) {
    ObjMap *a = AS_MAP(args[0]);
    ObjList *bl = NULL;
    if (!seq_items(vm, args[1], &bl))
        return false;
    push_root(vm, val_obj(bl));
    ObjMap *b = new_map(vm, OBJ_SET);
    push_root(vm, val_obj(b));
    for (int i = 0; i < bl->count; i++)
        if (!map_set(vm, b, bl->items[i], val_null())) {
            pop_root(vm, 2);
            return false;
        }
    ObjMap *r = new_map(vm, OBJ_SET);
    push_root(vm, val_obj(r));
    bool ok = true;
    if (op == 3) { /* subset */
        bool all = true;
        for (int i = 0; i < a->used && ok && all; i++)
            if (a->entries[i].live) {
                bool f;
                ok = map_has(vm, b, a->entries[i].key, &f);
                all = f;
            }
        pop_root(vm, 3);
        *out = val_bool(all);
        return ok;
    }
    for (int i = 0; i < a->used && ok; i++) {
        if (!a->entries[i].live)
            continue;
        bool f;
        ok = map_has(vm, b, a->entries[i].key, &f);
        if (ok && (op == 0 || (op == 1 && f) || (op == 2 && !f)))
            ok = map_set(vm, r, a->entries[i].key, val_null());
    }
    if (op == 0)
        for (int i = 0; i < bl->count && ok; i++)
            ok = map_set(vm, r, bl->items[i], val_null());
    pop_root(vm, 3);
    *out = val_obj(r);
    return ok;
}
NATIVE(set_union) {
    UNUSED_ARGS;
    return set_op(vm, args, out, 0);
}
NATIVE(set_intersection) {
    UNUSED_ARGS;
    return set_op(vm, args, out, 1);
}
NATIVE(set_difference) {
    UNUSED_ARGS;
    return set_op(vm, args, out, 2);
}
NATIVE(set_is_subset) {
    UNUSED_ARGS;
    return set_op(vm, args, out, 3);
}

/* ---------------------------------------------------------------- Result, errors, weak */

static ObjResult *R(Value v) { return (ObjResult *)AS_OBJ(v); }
NATIVE(r_is_ok) {
    UNUSED_ARGS;
    *out = val_bool(R(args[0])->ok);
    return true;
}
NATIVE(r_is_error) {
    UNUSED_ARGS;
    *out = val_bool(!R(args[0])->ok);
    return true;
}
NATIVE(r_unwrap) {
    UNUSED_ARGS;
    if (!R(args[0])->ok) {
        ObjString *s = value_to_string(vm, R(args[0])->value);
        return vm_throw(vm, "ResultError", "unwrap of Result.error(%s)", s ? s->chars : "?");
    }
    *out = R(args[0])->value;
    return true;
}
NATIVE(r_error) {
    UNUSED_ARGS;
    if (R(args[0])->ok)
        return vm_throw(vm, "ResultError", "error() of an ok Result");
    *out = R(args[0])->value;
    return true;
}
NATIVE(r_unwrap_or) {
    UNUSED_ARGS;
    *out = R(args[0])->ok ? R(args[0])->value : args[1];
    return true;
}
NATIVE(w_get) {
    UNUSED_ARGS;
    ObjWeak *w = (ObjWeak *)AS_OBJ(args[0]);
    *out = w->target ? val_obj(w->target) : val_null();
    return true;
}
NATIVE(w_alive) {
    UNUSED_ARGS;
    *out = val_bool(((ObjWeak *)AS_OBJ(args[0]))->target != NULL);
    return true;
}

/* ---------------------------------------------------------------- elements */

static bool add_child(VM *vm, ObjElement *e, Value v, int depth) {
    if (depth > 64)
        return vm_throw(vm, "ValueError", "markup children nested too deeply");
    if (IS_NULL(v))
        return true;
    if (IS_STRING(v)) {
        if (AS_STRING(v)->len)
            va_push(vm, &e->children, v);
        return true;
    }
    if (IS_OBJT(v, OBJ_ELEMENT)) {
        ObjElement *c = AS_ELEMENT(v);
        if (!c->tag) {
            for (int i = 0; i < c->children.count; i++) {
                Value gc = c->children.items[i];
                if (IS_OBJT(gc, OBJ_ELEMENT))
                    AS_ELEMENT(gc)->parent = e;
                va_push(vm, &e->children, gc);
            }
            return true;
        }
        c->parent = e;
        va_push(vm, &e->children, v);
        return true;
    }
    if (IS_LIST(v)) {
        ObjList *l = AS_LIST(v);
        for (int i = 0; i < l->count; i++)
            if (!add_child(vm, e, l->items[i], depth + 1))
                return false;
        return true;
    }
    ObjString *s = value_to_string(vm, v);
    if (!s)
        return false;
    if (s->len)
        va_push(vm, &e->children, val_obj(s));
    return true;
}
bool lib_element_new(VM *vm, ObjString *tag, ObjList *names, Value *attrs, int nchildren,
                     Value *children, Value *out) {
    ObjElement *e = new_element(vm, tag);
    push_root(vm, val_obj(e));
    for (int i = 0; i < names->count; i++) {
        va_push(vm, &e->attr_names, names->items[i]);
        va_push(vm, &e->attr_values, attrs[i]);
    }
    for (int i = 0; i < nchildren; i++)
        if (!add_child(vm, e, children[i], 0)) {
            pop_root(vm, 1);
            return false;
        }
    pop_root(vm, 1);
    *out = val_obj(e);
    return true;
}
NATIVE(e_render) {
    UNUSED_ARGS;
    Buf b;
    buf_init(&b);
    if (!element_render(vm, args[0], &b)) {
        buf_free(&b);
        return false;
    }
    *out = sv(vm, b.data ? b.data : "", b.len);
    buf_free(&b);
    return true;
}
static void element_text(ObjElement *e, Buf *b) {
    for (int i = 0; i < e->children.count; i++) {
        Value c = e->children.items[i];
        if (IS_STRING(c))
            buf_add(b, AS_CSTR(c), (size_t)AS_STRING(c)->len);
        else if (IS_OBJT(c, OBJ_ELEMENT))
            element_text(AS_ELEMENT(c), b);
    }
}
NATIVE(e_text) {
    UNUSED_ARGS;
    Buf b;
    buf_init(&b);
    element_text(AS_ELEMENT(args[0]), &b);
    *out = sv(vm, b.data ? b.data : "", b.len);
    buf_free(&b);
    return true;
}
NATIVE(e_get) {
    ObjElement *e = AS_ELEMENT(args[0]);
    WANT_STR(1, "get");
    for (int i = 0; i < e->attr_names.count; i++)
        if (!strcmp(AS_CSTR(e->attr_names.items[i]), AS_CSTR(args[1]))) {
            *out = e->attr_values.items[i];
            if (!IS_STRING(*out)) {
                ObjString *s = value_to_string(vm, *out);
                if (!s)
                    return false;
                *out = val_obj(s);
            }
            return true;
        }
    *out = argc > 2 ? args[2] : val_null();
    return true;
}
NATIVE(e_set) {
    UNUSED_ARGS;
    ObjElement *e = AS_ELEMENT(args[0]);
    WANT_STR(1, "set");
    for (int i = 0; i < e->attr_names.count; i++)
        if (!strcmp(AS_CSTR(e->attr_names.items[i]), AS_CSTR(args[1]))) {
            e->attr_values.items[i] = args[2];
            *out = val_null();
            return true;
        }
    va_push(vm, &e->attr_names, val_obj(str_intern(vm, AS_CSTR(args[1]), (size_t)S(args[1])->len)));
    va_push(vm, &e->attr_values, args[2]);
    *out = val_null();
    return true;
}
NATIVE(e_append) {
    for (int i = 1; i < argc; i++)
        if (!add_child(vm, AS_ELEMENT(args[0]), args[i], 0))
            return false;
    *out = val_null();
    return true;
}

/* ---------------------------------------------------------------- files */

static ObjFile *F(Value v) { return (ObjFile *)AS_OBJ(v); }
static bool open_file(VM *vm, Value v) {
    if (F(v)->closed || !F(v)->fp)
        return vm_throw(vm, "FileError", "file is closed");
    return true;
}
NATIVE(f_read_line) {
    UNUSED_ARGS;
    if (!open_file(vm, args[0]))
        return false;
    Buf b;
    buf_init(&b);
    bool eof;
    read_line(F(args[0])->fp, &b, &eof);
    *out = eof ? val_null() : sv(vm, b.data ? b.data : "", b.len);
    buf_free(&b);
    return true;
}
NATIVE(f_read_all) {
    UNUSED_ARGS;
    if (!open_file(vm, args[0]))
        return false;
    Buf b;
    buf_init(&b);
    char tmp[8192];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, F(args[0])->fp)) > 0)
        buf_add(&b, tmp, n);
    *out = sv(vm, b.data ? b.data : "", b.len);
    buf_free(&b);
    return true;
}
NATIVE(f_lines) {
    Value all;
    if (!f_read_all(vm, argc, args, &all))
        return false;
    push_root(vm, all);
    bool ok = s_lines(vm, 1, &all, out);
    pop_root(vm, 1);
    return ok;
}
NATIVE(f_write) {
    UNUSED_ARGS;
    if (!open_file(vm, args[0]))
        return false;
    if (!lib_print_value(vm, F(args[0])->fp, args[1]))
        return false;
    *out = val_null();
    return true;
}
NATIVE(f_close) {
    UNUSED_ARGS;
    ObjFile *f = F(args[0]);
    if (!f->closed && f->fp && !f->is_std)
        fclose(f->fp);
    else if (f->is_std && f->fp)
        fflush(f->fp);
    f->closed = true;
    *out = val_null();
    return true;
}
NATIVE(f_flush) {
    UNUSED_ARGS;
    if (F(args[0])->fp)
        fflush(F(args[0])->fp);
    *out = val_null();
    return true;
}

/* ---------------------------------------------------------------- dispatch tables */

static int tm_of(Value v) {
    switch (v.type) {
    case VAL_CHAR: return TM_CHAR;
    case VAL_INT: return TM_INT;
    case VAL_DOUBLE: return TM_DOUBLE;
    case VAL_OBJ: break;
    default: return TM_ANY;
    }
    switch ((ObjType)OBJ_TYPE(v)) {
    case OBJ_STRING: return TM_STRING;
    case OBJ_ARRAY: return TM_ARRAY;
    case OBJ_TUPLE: return TM_TUPLE;
    case OBJ_MAP: return TM_MAP;
    case OBJ_SET: return TM_SET;
    case OBJ_RESULT: return TM_RESULT;
    case OBJ_ERROR: return TM_ERROR;
    case OBJ_ELEMENT: return TM_ELEMENT;
    case OBJ_JSON: return TM_JSON;
    case OBJ_FILE: return TM_FILE;
    case OBJ_ENUMVAL: return TM_ENUMVAL;
    case OBJ_WEAK: return TM_WEAK;
    default: return TM_ANY;
    }
}
static bool find_method(VM *vm, Value self, ObjString *name, Value *m) {
    int tm = tm_of(self);
    if (table_get(&vm->type_methods[tm], name, m))
        return true;
    return table_get(&vm->type_methods[TM_ANY], name, m);
}
bool lib_has_builtin_method(VM *vm, Value self, ObjString *name) {
    Value m;
    return find_method(vm, self, name, &m);
}
bool lib_bound_builtin(VM *vm, Value self, ObjString *name, Value *out) {
    Value m;
    if (!find_method(vm, self, name, &m))
        return vm_throw(vm, "TypeError", "no method '%s'", name->chars);
    *out = val_obj(new_bound(vm, self, m));
    return true;
}
bool lib_builtin_method(VM *vm, Value self, ObjString *name, int argc, Value *args, Value *out,
                        bool *found) {
    Value m;
    *found = false;
    if (!find_method(vm, self, name, &m))
        return true;
    *found = true;
    ObjNative *n = (ObjNative *)AS_OBJ(m);
    if (argc < n->min_args || (n->max_args >= 0 && argc > n->max_args)) {
        if (n->min_args == n->max_args)
            return vm_throw(vm, "TypeError", "%s expects %d argument%s, got %d", n->name, n->min_args - 1,
                            n->min_args == 2 ? "" : "s", argc - 1);
        return vm_throw(vm, "TypeError", "%s got %d arguments", n->name, argc - 1);
    }
    return n->fn(vm, argc, args, out);
}

bool lib_builtin_property(VM *vm, Value self, ObjString *name, Value *out, bool *found) {
    *found = true;
    const char *n = name->chars;
    if (name == vm->s_length) {
        if (IS_STRING(self) || IS_LIST(self) || IS_MAP(self) || IS_SET(self) || IS_OBJT(self, OBJ_ELEMENT) ||
            IS_OBJT(self, OBJ_JSON)) {
            int64_t len;
            if (!length_of(vm, self, &len))
                return false;
            *out = val_int(len);
            return true;
        }
    }
    if (IS_OBJT(self, OBJ_ERROR)) {
        ObjError *e = (ObjError *)AS_OBJ(self);
        if (!strcmp(n, "message")) {
            *out = val_obj(e->message);
            return true;
        }
        if (!strcmp(n, "name") || !strcmp(n, "type")) {
            *out = val_obj(e->type);
            return true;
        }
    }
    if (IS_OBJT(self, OBJ_RESULT) && !strcmp(n, "is_ok")) {
        *out = val_bool(R(self)->ok);
        return true;
    }
    if (IS_OBJT(self, OBJ_ENUMVAL)) {
        ObjEnumVal *ev = (ObjEnumVal *)AS_OBJ(self);
        if (!strcmp(n, "name")) {
            *out = val_obj(ev->name);
            return true;
        }
        if (!strcmp(n, "value")) {
            *out = val_int(ev->index);
            return true;
        }
    }
    if (IS_OBJT(self, OBJ_ELEMENT))
        return lib_element_property(vm, self, name, out, found);
    if (IS_OBJT(self, OBJ_ELEMENT)) {
        ObjElement *e = AS_ELEMENT(self);
        if (!strcmp(n, "tag")) {
            *out = e->tag ? val_obj(e->tag) : svc(vm, "");
            return true;
        }
        if (!strcmp(n, "children")) {
            ObjList *l = new_list(vm, OBJ_ARRAY, e->children.count);
            memcpy(l->items, e->children.items, sizeof(Value) * (size_t)e->children.count);
            l->count = e->children.count;
            *out = val_obj(l);
            return true;
        }
        if (!strcmp(n, "attributes")) {
            ObjMap *m = new_map(vm, OBJ_MAP);
            push_root(vm, val_obj(m));
            for (int i = 0; i < e->attr_names.count; i++) {
                Value v = e->attr_values.items[i];
                if (!IS_STRING(v)) {
                    ObjString *s = value_to_string(vm, v);
                    if (!s) {
                        pop_root(vm, 1);
                        return false;
                    }
                    v = val_obj(s);
                }
                if (!map_set(vm, m, e->attr_names.items[i], v)) {
                    pop_root(vm, 1);
                    return false;
                }
            }
            pop_root(vm, 1);
            *out = val_obj(m);
            return true;
        }
        if (!strcmp(n, "parent")) {
            *out = e->parent ? val_obj(e->parent) : val_null();
            return true;
        }
    }
    *found = false;
    return true;
}
bool lib_set_property(VM *vm, Value obj, ObjString *name, Value v, bool *found) {
    *found = false;
    if (IS_OBJT(obj, OBJ_ELEMENT))
        return lib_element_set_property(vm, obj, name, v, found);
    if (IS_NULL(obj))
        return vm_throw(vm, "CastError", "cannot cast null to a non-null type (assigning '%s')", name->chars);
    return true;
}

static void reg(VM *vm, int tm, const char *name, NativeFn fn, int min, int max) {
    ObjNative *n = new_native(vm, name, fn, min + 1, max < 0 ? -1 : max + 1);
    n->is_method = true;
    table_set(vm, &vm->type_methods[tm], str_intern_c(vm, name), val_obj(n));
}
static void builtin(VM *vm, const char *name, NativeFn fn, int min, int max) {
    ObjNative *n = new_native(vm, name, fn, min, max);
    table_set(vm, &vm->builtins, str_intern_c(vm, name), val_obj(n));
}
static ObjTypeDesc *simple_type(VM *vm, TypeKind k, int bits, bool is_unsigned) {
    ObjTypeDesc *t = new_type(vm, k);
    t->bits = bits;
    t->is_unsigned = is_unsigned;
    t->slot = -1;
    return t;
}
static void cast_builtin(VM *vm, const char *name, ObjTypeDesc *t) {
    ObjNative *n = new_native(vm, name, n_cast, 0, 1);
    n->data = val_obj(t);
    table_set(vm, &vm->builtins, str_intern_c(vm, name), val_obj(n));
}

void lib_core_init(VM *vm) {
    builtin(vm, "print", n_print, 0, -1);
    builtin(vm, "input", n_input, 0, 1);
    builtin(vm, "len", n_len, 1, 1);
    builtin(vm, "typeof", n_typeof, 1, 1);
    builtin(vm, "weak", n_weak, 1, 1);
    cast_builtin(vm, "int", simple_type(vm, TY_INT, 64, false));
    cast_builtin(vm, "i64", simple_type(vm, TY_INT, 64, false));
    cast_builtin(vm, "i32", simple_type(vm, TY_INT, 32, false));
    cast_builtin(vm, "i16", simple_type(vm, TY_INT, 16, false));
    cast_builtin(vm, "i8", simple_type(vm, TY_INT, 8, false));
    cast_builtin(vm, "u64", simple_type(vm, TY_INT, 64, true));
    cast_builtin(vm, "u32", simple_type(vm, TY_INT, 32, true));
    cast_builtin(vm, "u16", simple_type(vm, TY_INT, 16, true));
    cast_builtin(vm, "u8", simple_type(vm, TY_INT, 8, true));
    cast_builtin(vm, "double", simple_type(vm, TY_DOUBLE, 0, false));
    cast_builtin(vm, "float", simple_type(vm, TY_FLOAT, 0, false));
    cast_builtin(vm, "bool", simple_type(vm, TY_BOOL, 0, false));
    cast_builtin(vm, "char", simple_type(vm, TY_CHAR, 0, false));
    cast_builtin(vm, "string", simple_type(vm, TY_STRING, 0, false));
    cast_builtin(vm, "array", simple_type(vm, TY_ARRAY, 0, false));
    cast_builtin(vm, "tuple", simple_type(vm, TY_HTUPLE, 0, false));
    cast_builtin(vm, "map", simple_type(vm, TY_MAP, 0, false));
    cast_builtin(vm, "set", simple_type(vm, TY_SET, 0, false));

    static const char *errors[] = {
        "Error", "FileError", "IndexError", "KeyError", "NumericError", "AssertionError", "JsonError",
        "CastError", "SelectorError", "InputError", "ProcessError", "DesktopError", "RegexError",
        "NetworkError", "ValueError", "HttpError", "DatabaseError", "NativeError", "ResultError",
        "RuntimeError", "TypeError", "ImportError", "MatchError", NULL};
    for (int i = 0; errors[i]; i++) {
        ObjNative *n = new_native(vm, errors[i], n_error_ctor, 0, 1);
        n->data = val_obj(str_intern_c(vm, errors[i]));
        table_set(vm, &vm->builtins, str_intern_c(vm, errors[i]), val_obj(n));
    }
    /* type(x), unit(x), unitns(x); quantities answer through their hidden class (lower.c) */
    static const struct {
        const char *name;
        NativeFn fn;
    } introspection[] = {{"type", n_type}, {"unit", n_unit}, {"unitns", n_unitns}};
    for (size_t i = 0; i < sizeof introspection / sizeof introspection[0]; i++)
        table_set(vm, &vm->builtins, str_intern_c(vm, introspection[i].name),
                  val_obj(new_native(vm, introspection[i].name, introspection[i].fn, 1, 1)));
    table_set(vm, &vm->builtins, str_intern_c(vm, "__fwu_in"),
              val_obj(new_native(vm, "__fwu_in", n_quantity_in, 2, 2)));
    ObjModule *result = new_module(vm, str_intern_c(vm, "Result"));
    result->native = true;
    module_set_native(vm, result, "ok", n_result_ok, 0, 1);
    module_set_native(vm, result, "error", n_result_error, 0, 1);
    table_set(vm, &vm->builtins, str_intern_c(vm, "Result"), val_obj(result));

    reg(vm, TM_ANY, "to_string", any_to_string, 0, 0);

    reg(vm, TM_STRING, "upper", s_upper, 0, 0);
    reg(vm, TM_STRING, "lower", s_lower, 0, 0);
    reg(vm, TM_STRING, "trim", s_trim, 0, 0);
    reg(vm, TM_STRING, "trim_start", s_trim_start, 0, 0);
    reg(vm, TM_STRING, "trim_end", s_trim_end, 0, 0);
    reg(vm, TM_STRING, "split", s_split, 0, 1);
    reg(vm, TM_STRING, "lines", s_lines, 0, 0);
    reg(vm, TM_STRING, "chars", s_chars, 0, 0);
    reg(vm, TM_STRING, "find", s_find, 1, 1);
    reg(vm, TM_STRING, "index_of", s_find, 1, 1);
    reg(vm, TM_STRING, "find_last", s_find_last, 1, 1);
    reg(vm, TM_STRING, "contains", s_contains, 1, 1);
    reg(vm, TM_STRING, "starts_with", s_starts_with, 1, 1);
    reg(vm, TM_STRING, "ends_with", s_ends_with, 1, 1);
    reg(vm, TM_STRING, "count", s_count, 1, 1);
    reg(vm, TM_STRING, "replace", s_replace, 2, 2);
    reg(vm, TM_STRING, "pad_start", s_pad_start, 1, 2);
    reg(vm, TM_STRING, "pad_end", s_pad_end, 1, 2);
    reg(vm, TM_STRING, "reverse", s_reverse, 0, 0);
    reg(vm, TM_STRING, "repeat", s_repeat, 1, 1);
    reg(vm, TM_STRING, "is_empty", s_is_empty, 0, 0);
    reg(vm, TM_STRING, "join", s_join, 1, 1);
    reg(vm, TM_STRING, "slice", s_slice, 1, 2);
    reg(vm, TM_STRING, "bytes", s_bytes, 0, 0);

    reg(vm, TM_CHAR, "is_digit", c_is_digit, 0, 0);
    reg(vm, TM_CHAR, "is_letter", c_is_letter, 0, 0);
    reg(vm, TM_CHAR, "is_alpha", c_is_letter, 0, 0);
    reg(vm, TM_CHAR, "is_space", c_is_space, 0, 0);
    reg(vm, TM_CHAR, "is_whitespace", c_is_space, 0, 0);
    reg(vm, TM_CHAR, "is_upper", c_is_upper, 0, 0);
    reg(vm, TM_CHAR, "is_lower", c_is_lower, 0, 0);
    reg(vm, TM_CHAR, "upper", c_upper, 0, 0);
    reg(vm, TM_CHAR, "lower", c_lower, 0, 0);
    reg(vm, TM_CHAR, "code", c_code, 0, 0);

    for (int t = TM_ARRAY; t <= TM_TUPLE; t++) {
        reg(vm, t, "slice", a_slice, 1, 2);
        reg(vm, t, "index_of", a_index_of, 1, 1);
        reg(vm, t, "contains", a_contains, 1, 1);
        reg(vm, t, "copy", a_copy, 0, 0);
        reg(vm, t, "join", a_join, 0, 1);
        reg(vm, t, "map", a_map, 1, 1);
        reg(vm, t, "filter", a_filter, 1, 1);
        reg(vm, t, "reduce", a_reduce, 2, 2);
        reg(vm, t, "any", a_any, 1, 1);
        reg(vm, t, "all", a_all, 1, 1);
        reg(vm, t, "each", a_each, 1, 1);
        reg(vm, t, "is_empty", a_is_empty, 0, 0);
        reg(vm, t, "first", a_first, 0, 0);
        reg(vm, t, "last", a_last, 0, 0);
        reg(vm, t, "freeze", a_freeze, 0, 0);
        reg(vm, t, "is_frozen", a_is_frozen, 0, 0);
    }
    reg(vm, TM_ANY, "$which", a_which, 1, 1);
    reg(vm, TM_ARRAY, "push", a_push, 1, -1);
    reg(vm, TM_ARRAY, "append", a_push, 1, -1);
    reg(vm, TM_ARRAY, "pop", a_pop, 0, 0);
    reg(vm, TM_ARRAY, "insert", a_insert, 2, 2);
    reg(vm, TM_ARRAY, "remove_at", a_remove_at, 1, 1);
    reg(vm, TM_ARRAY, "remove", a_remove, 1, 1);
    reg(vm, TM_ARRAY, "clear", a_clear, 0, 0);
    reg(vm, TM_ARRAY, "extend", a_extend, 1, 1);
    reg(vm, TM_ARRAY, "sort", a_sort, 0, 1);
    reg(vm, TM_ARRAY, "reverse", a_reverse, 0, 0);

    for (int t = TM_MAP; t <= TM_SET; t++) {
        reg(vm, t, "remove", m_remove, 1, 1);
        reg(vm, t, "contains", m_contains, 1, 1);
        reg(vm, t, "clear", m_clear, 0, 0);
        reg(vm, t, "copy", m_copy, 0, 0);
        reg(vm, t, "items", m_items, 0, 0);
        reg(vm, t, "freeze", a_freeze, 0, 0);
        reg(vm, t, "is_frozen", a_is_frozen, 0, 0);
        reg(vm, t, "is_empty", a_is_empty, 0, 0);
    }
    reg(vm, TM_MAP, "keys", m_keys, 0, 0);
    reg(vm, TM_MAP, "values", m_values, 0, 0);
    reg(vm, TM_MAP, "has", m_contains, 1, 1);
    reg(vm, TM_MAP, "get", m_get, 1, 2);
    reg(vm, TM_SET, "add", set_add, 1, 1);
    reg(vm, TM_SET, "union", set_union, 1, 1);
    reg(vm, TM_SET, "intersection", set_intersection, 1, 1);
    reg(vm, TM_SET, "difference", set_difference, 1, 1);
    reg(vm, TM_SET, "is_subset", set_is_subset, 1, 1);

    reg(vm, TM_RESULT, "is_ok", r_is_ok, 0, 0);
    reg(vm, TM_RESULT, "is_error", r_is_error, 0, 0);
    reg(vm, TM_RESULT, "unwrap", r_unwrap, 0, 0);
    reg(vm, TM_RESULT, "error", r_error, 0, 0);
    reg(vm, TM_RESULT, "unwrap_or", r_unwrap_or, 1, 1);

    reg(vm, TM_WEAK, "get", w_get, 0, 0);
    reg(vm, TM_WEAK, "alive", w_alive, 0, 0);

    (void)e_render;
    (void)e_text;
    (void)e_get;
    (void)e_set;
    (void)e_append;
    lib_html_init(vm);

    reg(vm, TM_FILE, "read_line", f_read_line, 0, 0);
    reg(vm, TM_FILE, "read_all", f_read_all, 0, 0);
    reg(vm, TM_FILE, "lines", f_lines, 0, 0);
    reg(vm, TM_FILE, "write", f_write, 1, 1);
    reg(vm, TM_FILE, "close", f_close, 0, 0);
    reg(vm, TM_FILE, "flush", f_flush, 0, 0);
}
