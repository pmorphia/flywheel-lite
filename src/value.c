/* Values: equality, hashing, maps, display formatting, format specs, and HTML rendering. */
#include "lite.h"
#include <ctype.h>
#include <math.h>

/* ---------------------------------------------------------------- buffers */

void buf_init(Buf *b) {
    b->data = NULL;
    b->len = b->cap = 0;
}
void buf_free(Buf *b) {
    free(b->data);
    buf_init(b);
}
void buf_add(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        while (nc < b->len + n + 1)
            nc *= 2;
        b->data = xrealloc(b->data, nc);
        b->cap = nc;
    }
    if (n)
        memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}
void buf_adds(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
void buf_addc(Buf *b, char c) { buf_add(b, &c, 1); }
void buf_printf(Buf *b, const char *fmt, ...) {
    char small[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof small) {
        buf_add(b, small, (size_t)n);
        return;
    }
    char *big = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_add(b, big, (size_t)n);
    free(big);
}
void buf_utf8(Buf *b, uint32_t v) {
    char t[4];
    if (v < 0x80) {
        buf_addc(b, (char)v);
    } else if (v < 0x800) {
        t[0] = (char)(0xc0 | (v >> 6));
        t[1] = (char)(0x80 | (v & 63));
        buf_add(b, t, 2);
    } else if (v < 0x10000) {
        t[0] = (char)(0xe0 | (v >> 12));
        t[1] = (char)(0x80 | ((v >> 6) & 63));
        t[2] = (char)(0x80 | (v & 63));
        buf_add(b, t, 3);
    } else {
        t[0] = (char)(0xf0 | (v >> 18));
        t[1] = (char)(0x80 | ((v >> 12) & 63));
        t[2] = (char)(0x80 | ((v >> 6) & 63));
        t[3] = (char)(0x80 | (v & 63));
        buf_add(b, t, 4);
    }
}

/* ---------------------------------------------------------------- UTF-8 */

uint32_t utf8_decode(const char *s, int len, int *adv) {
    const unsigned char *u = (const unsigned char *)s;
    if (len <= 0) {
        *adv = 0;
        return 0;
    }
    unsigned c = u[0];
    int n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if (n > len)
        n = len;
    uint32_t v = n == 1 ? c : (c & (0x3fu >> (n - 1)));
    for (int i = 1; i < n; i++)
        v = (v << 6) | (u[i] & 63);
    *adv = n;
    return v;
}
int utf8_offset(ObjString *s, int index) {
    if (s->clen == s->len)
        return index < 0 ? 0 : index > s->len ? s->len : index;
    int i = 0, c = 0;
    while (i < s->len && c < index) {
        i++;
        while (i < s->len && ((unsigned char)s->chars[i] & 0xc0) == 0x80)
            i++;
        c++;
    }
    return i;
}
uint32_t str_char_at(ObjString *s, int index) {
    int off = utf8_offset(s, index), adv;
    return utf8_decode(s->chars + off, s->len - off, &adv);
}

/* ---------------------------------------------------------------- equality and hashing */

bool values_same(Value a, Value b) {
    if (a.type != b.type)
        return false;
    switch (a.type) {
    case VAL_NULL:
    case VAL_UNDEF: return true;
    case VAL_BOOL: return a.as.b == b.as.b;
    case VAL_INT: return a.as.i == b.as.i;
    case VAL_DOUBLE: return a.as.d == b.as.d;
    case VAL_CHAR: return a.as.c == b.as.c;
    default: return a.as.o == b.as.o;
    }
}

static bool call_bool_method(VM *vm, Value self, Value method, Value arg, bool *out) {
    Value r;
    if (!vm_call(vm, val_obj(new_bound(vm, self, method)), 1, &arg, &r))
        return false;
    if (!IS_BOOL(r))
        return vm_throw(vm, "TypeError", "equality methods must return bool");
    *out = r.as.b;
    return true;
}

bool values_equal(VM *vm, Value a, Value b, bool *out) {
    *out = false;
    if (IS_NUM(a) && IS_NUM(b)) {
        if (IS_INT(a) && IS_INT(b))
            *out = a.as.i == b.as.i;
        else
            *out = AS_NUM(a) == AS_NUM(b);
        return true;
    }
    if (!IS_OBJ(a) || !IS_OBJ(b)) {
        if (IS_OBJT(a, OBJ_INSTANCE) && !IS_NULL(AS_INSTANCE(a)->klass->m_eq) && !IS_NULL(b))
            return call_bool_method(vm, a, AS_INSTANCE(a)->klass->m_eq, b, out);
        *out = values_same(a, b);
        return true;
    }
    if (AS_OBJ(a) == AS_OBJ(b)) {
        *out = true;
        return true;
    }
    ObjType ta = (ObjType)OBJ_TYPE(a), tb = (ObjType)OBJ_TYPE(b);
    if (ta == OBJ_INSTANCE) {
        ObjClass *k = AS_INSTANCE(a)->klass;
        if (!IS_NULL(k->m_eq))
            return call_bool_method(vm, a, k->m_eq, b, out);
        if (!IS_NULL(k->m_equals) && tb == OBJ_INSTANCE)
            return call_bool_method(vm, a, k->m_equals, b, out);
        if (k->is_variant && tb == OBJ_INSTANCE && AS_INSTANCE(b)->klass == k) {
            ObjInstance *x = AS_INSTANCE(a), *y = AS_INSTANCE(b);
            for (int i = 0; i < x->nfields; i++) {
                bool eq;
                if (!values_equal(vm, x->fields[i], y->fields[i], &eq))
                    return false;
                if (!eq)
                    return true;
            }
            *out = true;
        }
        return true;
    }
    if (ta == OBJ_STRING && tb == OBJ_STRING) {
        ObjString *x = AS_STRING(a), *y = AS_STRING(b);
        *out = x->len == y->len && x->hash == y->hash && memcmp(x->chars, y->chars, (size_t)x->len) == 0;
        return true;
    }
    if ((ta == OBJ_ARRAY || ta == OBJ_TUPLE) && (tb == OBJ_ARRAY || tb == OBJ_TUPLE)) {
        ObjList *x = AS_LIST(a), *y = AS_LIST(b);
        if (x->count != y->count)
            return true;
        for (int i = 0; i < x->count; i++) {
            bool eq;
            if (!values_equal(vm, x->items[i], y->items[i], &eq))
                return false;
            if (!eq)
                return true;
        }
        *out = true;
        return true;
    }
    if ((ta == OBJ_MAP && tb == OBJ_MAP) || (ta == OBJ_SET && tb == OBJ_SET)) {
        ObjMap *x = AS_MAP(a), *y = AS_MAP(b);
        if (x->live != y->live)
            return true;
        for (int i = 0; i < x->used; i++) {
            if (!x->entries[i].live)
                continue;
            Value v;
            bool found;
            if (!map_get(vm, y, x->entries[i].key, &v, &found))
                return false;
            if (!found)
                return true;
            if (ta == OBJ_MAP) {
                bool eq;
                if (!values_equal(vm, x->entries[i].value, v, &eq))
                    return false;
                if (!eq)
                    return true;
            }
        }
        *out = true;
        return true;
    }
    if (ta == OBJ_RESULT && tb == OBJ_RESULT) {
        ObjResult *x = (ObjResult *)AS_OBJ(a), *y = (ObjResult *)AS_OBJ(b);
        if (x->ok != y->ok)
            return true;
        return values_equal(vm, x->value, y->value, out);
    }
    if (ta == OBJ_JSON && tb == OBJ_JSON)
        return values_equal(vm, ((ObjJson *)AS_OBJ(a))->v, ((ObjJson *)AS_OBJ(b))->v, out);
    if (ta == OBJ_BOUND && tb == OBJ_BOUND) {
        ObjBound *x = (ObjBound *)AS_OBJ(a), *y = (ObjBound *)AS_OBJ(b);
        *out = values_same(x->receiver, y->receiver) && values_same(x->method, y->method);
        return true;
    }
    return true;
}

static uint32_t mix(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return (uint32_t)x;
}
bool value_hash(VM *vm, Value v, uint32_t *out) {
    switch (v.type) {
    case VAL_NULL:
    case VAL_UNDEF: *out = 0x9e3779b9u; return true;
    case VAL_BOOL: *out = v.as.b ? 1231u : 1237u; return true;
    case VAL_INT: *out = mix((uint64_t)v.as.i); return true;
    case VAL_DOUBLE: {
        double d = v.as.d;
        if (d == floor(d) && d >= -9.2e18 && d <= 9.2e18) {
            *out = mix((uint64_t)(int64_t)d);
            return true;
        }
        uint64_t bits;
        memcpy(&bits, &d, sizeof bits);
        *out = mix(bits);
        return true;
    }
    case VAL_CHAR: *out = mix((uint64_t)v.as.c * 31u + 7u); return true;
    default: break;
    }
    Obj *o = AS_OBJ(v);
    switch ((ObjType)o->type) {
    case OBJ_STRING: *out = ((ObjString *)o)->hash; return true;
    case OBJ_ARRAY:
    case OBJ_TUPLE: {
        ObjList *l = (ObjList *)o;
        uint32_t h = 17;
        for (int i = 0; i < l->count; i++) {
            uint32_t e;
            if (!value_hash(vm, l->items[i], &e))
                return false;
            h = h * 31u + e;
        }
        *out = h;
        return true;
    }
    case OBJ_INSTANCE: {
        ObjClass *k = ((ObjInstance *)o)->klass;
        if (!IS_NULL(k->m_hash)) {
            Value r;
            if (!vm_call(vm, val_obj(new_bound(vm, v, k->m_hash)), 0, NULL, &r))
                return false;
            if (!IS_INT(r))
                return vm_throw(vm, "TypeError", "hash() must return int");
            *out = mix((uint64_t)r.as.i);
            return true;
        }
        if (k->is_variant) {
            ObjInstance *in = (ObjInstance *)o;
            uint32_t h = mix((uint64_t)(uintptr_t)k);
            for (int i = 0; i < in->nfields; i++) {
                uint32_t e;
                if (!value_hash(vm, in->fields[i], &e))
                    return false;
                h = h * 31u + e;
            }
            *out = h;
            return true;
        }
        break;
    }
    case OBJ_JSON: return value_hash(vm, ((ObjJson *)o)->v, out);
    case OBJ_RESULT: {
        uint32_t h;
        if (!value_hash(vm, ((ObjResult *)o)->value, &h))
            return false;
        *out = h * 3u + (((ObjResult *)o)->ok ? 1u : 2u);
        return true;
    }
    default: break;
    }
    *out = mix((uint64_t)(uintptr_t)o);
    return true;
}

/* ---------------------------------------------------------------- maps */

static void map_reindex(VM *vm, ObjMap *m, int index_cap) {
    /* compact entries */
    int w = 0;
    for (int i = 0; i < m->used; i++)
        if (m->entries[i].live)
            m->entries[w++] = m->entries[i];
    m->used = w;
    TB_FREE(vm, int32_t, m->index, m->index_cap);
    m->index = TB_ALLOC(vm, int32_t, index_cap);
    m->index_cap = index_cap;
    for (int i = 0; i < index_cap; i++)
        m->index[i] = -1;
    for (int i = 0; i < m->used; i++) {
        uint32_t s = m->entries[i].hash & (uint32_t)(index_cap - 1);
        while (m->index[s] != -1)
            s = (s + 1) & (uint32_t)(index_cap - 1);
        m->index[s] = i;
    }
}
/* Returns the entry index or -1; *slot receives the index-table position for insertion. */
static bool map_find(VM *vm, ObjMap *m, Value key, uint32_t h, int *entry, int *slot) {
    *entry = -1;
    *slot = -1;
    if (!m->index_cap)
        return true;
    uint32_t s = h & (uint32_t)(m->index_cap - 1);
    int tomb = -1;
    for (;;) {
        int32_t e = m->index[s];
        if (e == -1) {
            *slot = tomb >= 0 ? tomb : (int)s;
            return true;
        }
        if (e == -2) {
            if (tomb < 0)
                tomb = (int)s;
        } else if (m->entries[e].hash == h) {
            Value k = m->entries[e].key;
            bool eq;
            if (k.type == key.type && (k.type == VAL_INT ? k.as.i == key.as.i : 0)) {
                *entry = e;
                *slot = (int)s;
                return true;
            }
            if (!values_equal(vm, k, key, &eq))
                return false;
            if (eq) {
                *entry = e;
                *slot = (int)s;
                return true;
            }
        }
        s = (s + 1) & (uint32_t)(m->index_cap - 1);
    }
}
bool map_get(VM *vm, ObjMap *m, Value key, Value *out, bool *found) {
    *found = false;
    if (!m->live)
        return true;
    uint32_t h;
    if (!value_hash(vm, key, &h))
        return false;
    int e, s;
    if (!map_find(vm, m, key, h, &e, &s))
        return false;
    if (e >= 0) {
        *found = true;
        *out = m->entries[e].value;
    }
    return true;
}
bool map_has(VM *vm, ObjMap *m, Value key, bool *found) {
    Value v;
    return map_get(vm, m, key, &v, found);
}
bool map_set(VM *vm, ObjMap *m, Value key, Value v) {
    uint32_t h;
    if (!value_hash(vm, key, &h))
        return false;
    if ((m->used + 1) * 2 > m->index_cap)
        map_reindex(vm, m, m->index_cap ? (m->live + 1) * 2 > m->index_cap / 2 ? m->index_cap * 2 : m->index_cap : 8);
    int e, s;
    if (!map_find(vm, m, key, h, &e, &s))
        return false;
    if (e >= 0) {
        m->entries[e].value = v;
        return true;
    }
    if (m->used == m->cap) {
        int nc = GROW_CAP(m->cap);
        m->entries = tb_realloc(vm, m->entries, sizeof(MapEntry) * (size_t)m->cap, sizeof(MapEntry) * (size_t)nc);
        m->cap = nc;
    }
    /* reindexing may have run inside map_find through user hash methods: recompute the slot */
    if (s < 0 || m->index[s] >= 0) {
        if (!map_find(vm, m, key, h, &e, &s))
            return false;
    }
    m->entries[m->used].key = key;
    m->entries[m->used].value = v;
    m->entries[m->used].hash = h;
    m->entries[m->used].live = true;
    m->index[s] = m->used;
    m->used++;
    m->live++;
    return true;
}
bool map_remove(VM *vm, ObjMap *m, Value key, bool *found) {
    *found = false;
    if (!m->live)
        return true;
    uint32_t h;
    if (!value_hash(vm, key, &h))
        return false;
    int e, s;
    if (!map_find(vm, m, key, h, &e, &s))
        return false;
    if (e < 0)
        return true;
    *found = true;
    m->entries[e].live = false;
    m->entries[e].key = val_null();
    m->entries[e].value = val_null();
    m->index[s] = -2;
    m->live--;
    return true;
}
void map_clear(VM *vm, ObjMap *m) {
    TB_FREE(vm, MapEntry, m->entries, m->cap);
    TB_FREE(vm, int32_t, m->index, m->index_cap);
    m->entries = NULL;
    m->index = NULL;
    m->cap = m->index_cap = m->used = m->live = 0;
}
ObjList *map_keys(VM *vm, ObjMap *m) {
    ObjList *l = new_list(vm, OBJ_ARRAY, m->live);
    for (int i = 0; i < m->used; i++)
        if (m->entries[i].live)
            l->items[l->count++] = m->entries[i].key;
    return l;
}
ObjList *map_values(VM *vm, ObjMap *m) {
    ObjList *l = new_list(vm, OBJ_ARRAY, m->live);
    for (int i = 0; i < m->used; i++)
        if (m->entries[i].live)
            l->items[l->count++] = m->entries[i].value;
    return l;
}

/* ---------------------------------------------------------------- comparison */

bool value_compare(VM *vm, Value a, Value b, int *out) {
    if (IS_NUM(a) && IS_NUM(b)) {
        if (IS_INT(a) && IS_INT(b))
            *out = a.as.i < b.as.i ? -1 : a.as.i > b.as.i;
        else {
            double x = AS_NUM(a), y = AS_NUM(b);
            *out = x < y ? -1 : x > y;
        }
        return true;
    }
    if (IS_STRING(a) && IS_STRING(b)) {
        ObjString *x = AS_STRING(a), *y = AS_STRING(b);
        int n = x->len < y->len ? x->len : y->len;
        int c = memcmp(x->chars, y->chars, (size_t)n);
        *out = c ? (c < 0 ? -1 : 1) : (x->len < y->len ? -1 : x->len > y->len);
        return true;
    }
    if (IS_CHAR(a) && IS_CHAR(b)) {
        *out = a.as.c < b.as.c ? -1 : a.as.c > b.as.c;
        return true;
    }
    if (IS_BOOL(a) && IS_BOOL(b)) {
        *out = (int)a.as.b - (int)b.as.b;
        return true;
    }
    if (IS_OBJT(a, OBJ_ENUMVAL) && IS_OBJT(b, OBJ_ENUMVAL)) {
        int x = ((ObjEnumVal *)AS_OBJ(a))->index, y = ((ObjEnumVal *)AS_OBJ(b))->index;
        *out = x < y ? -1 : x > y;
        return true;
    }
    if (IS_LIST(a) && IS_LIST(b)) {
        ObjList *x = AS_LIST(a), *y = AS_LIST(b);
        for (int i = 0; i < x->count && i < y->count; i++) {
            int c;
            if (!value_compare(vm, x->items[i], y->items[i], &c))
                return false;
            if (c) {
                *out = c;
                return true;
            }
        }
        *out = x->count < y->count ? -1 : x->count > y->count;
        return true;
    }
    if (IS_OBJT(a, OBJ_INSTANCE) && !IS_NULL(AS_INSTANCE(a)->klass->m_lt)) {
        Value r;
        Value lt = AS_INSTANCE(a)->klass->m_lt;
        if (!vm_call(vm, val_obj(new_bound(vm, a, lt)), 1, &b, &r))
            return false;
        if (IS_BOOL(r) && r.as.b) {
            *out = -1;
            return true;
        }
        if (!vm_call(vm, val_obj(new_bound(vm, b, lt)), 1, &a, &r))
            return false;
        *out = IS_BOOL(r) && r.as.b ? 1 : 0;
        return true;
    }
    Buf x, y;
    buf_init(&x);
    buf_init(&y);
    type_name_of(vm, a, &x);
    type_name_of(vm, b, &y);
    bool r = vm_throw(vm, "TypeError", "cannot compare %s with %s", x.data ? x.data : "?", y.data ? y.data : "?");
    buf_free(&x);
    buf_free(&y);
    return r;
}

/* ---------------------------------------------------------------- type names */

const char *obj_kind_name(Value v) {
    switch (v.type) {
    case VAL_NULL: return "null";
    case VAL_UNDEF: return "undefined";
    case VAL_BOOL: return "bool";
    case VAL_INT: return "int";
    case VAL_DOUBLE: return "double";
    case VAL_CHAR: return "char";
    default: break;
    }
    switch ((ObjType)OBJ_TYPE(v)) {
    case OBJ_STRING: return "string";
    case OBJ_ARRAY: return "array";
    case OBJ_TUPLE: return "tuple";
    case OBJ_MAP: return "map";
    case OBJ_SET: return "set";
    case OBJ_CLOSURE:
    case OBJ_NATIVE:
    case OBJ_BOUND:
    case OBJ_FUNCTION: return "function";
    case OBJ_CLASS: return "class";
    case OBJ_INSTANCE: return "instance";
    case OBJ_ENUM: return "enum";
    case OBJ_ENUMVAL: return "enum value";
    case OBJ_MODULE: return "module";
    case OBJ_ERROR: return "Error";
    case OBJ_RESULT: return "Result";
    case OBJ_ELEMENT: return "element";
    case OBJ_JSON: return "json.value";
    case OBJ_TYPE: return "type";
    case OBJ_FILE: return "io.file";
    case OBJ_WEAK: return "weak";
    default: return "value";
    }
}
const char *type_name_of(VM *vm, Value v, Buf *b) {
    (void)vm;
    if (IS_OBJT(v, OBJ_INSTANCE)) {
        buf_adds(b, AS_INSTANCE(v)->klass->name->chars);
    } else if (IS_OBJT(v, OBJ_ENUMVAL)) {
        buf_adds(b, ((ObjEnumVal *)AS_OBJ(v))->e->name->chars);
    } else if (IS_OBJT(v, OBJ_ERROR)) {
        buf_adds(b, ((ObjError *)AS_OBJ(v))->type->chars);
    } else if (IS_ARRAY(v)) {
        ObjList *l = AS_LIST(v);
        if (l->count) {
            type_name_of(vm, l->items[0], b);
            buf_adds(b, "[]");
        } else
            buf_adds(b, "array");
    } else if (IS_TUPLE(v)) {
        ObjList *l = AS_LIST(v);
        bool same = l->count > 0;
        for (int i = 1; i < l->count && same; i++)
            same = l->items[i].type == l->items[0].type &&
                   (!IS_OBJ(l->items[0]) || OBJ_TYPE(l->items[i]) == OBJ_TYPE(l->items[0]));
        if (same || l->count == 0) {
            buf_adds(b, "tuple<");
            if (l->count)
                type_name_of(vm, l->items[0], b);
            buf_addc(b, '>');
            return b->data;
        }
        buf_addc(b, '[');
        for (int i = 0; i < l->count && i < 8; i++) {
            if (i)
                buf_adds(b, ", ");
            type_name_of(vm, l->items[i], b);
        }
        if (l->count > 8)
            buf_adds(b, ", ...");
        buf_addc(b, ']');
    } else if (IS_MAP(v)) {
        ObjMap *m = AS_MAP(v);
        int first = -1;
        for (int i = 0; i < m->used; i++)
            if (m->entries[i].live) {
                first = i;
                break;
            }
        if (first >= 0) {
            buf_adds(b, "map<");
            type_name_of(vm, m->entries[first].key, b);
            buf_addc(b, ',');
            type_name_of(vm, m->entries[first].value, b);
            buf_addc(b, '>');
        } else
            buf_adds(b, "map");
    } else if (IS_SET(v)) {
        buf_adds(b, "set");
    } else if (IS_OBJT(v, OBJ_ELEMENT)) {
        buf_adds(b, AS_ELEMENT(v)->tag ? "element" : "fragment");
    } else
        buf_adds(b, obj_kind_name(v));
    return b->data;
}

/* ---------------------------------------------------------------- display */

void format_double(double d, Buf *b) {
    if (isnan(d)) {
        buf_adds(b, "nan");
        return;
    }
    if (isinf(d)) {
        buf_adds(b, d < 0 ? "-inf" : "inf");
        return;
    }
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%.15g", d);
    if (!strcmp(tmp, "-0"))
        strcpy(tmp, "-0");
    buf_adds(b, tmp);
}
static void quote_string(Buf *b, const char *s, size_t n, char q) {
    buf_addc(b, q);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '\n': buf_adds(b, "\\n"); break;
        case '\r': buf_adds(b, "\\r"); break;
        case '\t': buf_adds(b, "\\t"); break;
        case '\\': buf_adds(b, "\\\\"); break;
        default:
            if (c == (unsigned char)q) {
                buf_addc(b, '\\');
                buf_addc(b, (char)c);
            } else if (c < 0x20)
                buf_printf(b, "\\u{%x}", c);
            else
                buf_addc(b, (char)c);
        }
    }
    buf_addc(b, q);
}

static Obj *fmt_path[300];
static int fmt_depth;

static bool fmt_enter(Obj *o, Buf *b) {
    for (int i = 0; i < fmt_depth; i++)
        if (fmt_path[i] == o) {
            buf_adds(b, "<cycle>");
            return false;
        }
    if (fmt_depth >= 256) {
        buf_adds(b, "...");
        return false;
    }
    fmt_path[fmt_depth++] = o;
    return true;
}

bool value_to_buf(VM *vm, Value v, Buf *b, bool quoted, int depth) {
    switch (v.type) {
    case VAL_NULL: buf_adds(b, "null"); return true;
    case VAL_UNDEF: buf_adds(b, "<uninitialized>"); return true;
    case VAL_BOOL: buf_adds(b, v.as.b ? "true" : "false"); return true;
    case VAL_INT: buf_printf(b, "%lld", (long long)v.as.i); return true;
    case VAL_DOUBLE: format_double(v.as.d, b); return true;
    case VAL_CHAR:
        if (quoted) {
            Buf c;
            buf_init(&c);
            buf_utf8(&c, v.as.c);
            quote_string(b, c.data, c.len, '\'');
            buf_free(&c);
        } else
            buf_utf8(b, v.as.c);
        return true;
    default: break;
    }
    Obj *o = AS_OBJ(v);
    switch ((ObjType)o->type) {
    case OBJ_STRING:
        if (quoted)
            quote_string(b, ((ObjString *)o)->chars, (size_t)((ObjString *)o)->len, '"');
        else
            buf_add(b, ((ObjString *)o)->chars, (size_t)((ObjString *)o)->len);
        return true;
    case OBJ_ARRAY:
    case OBJ_TUPLE: {
        ObjList *l = (ObjList *)o;
        if (!fmt_enter(o, b))
            return true;
        bool tuple = o->type == OBJ_TUPLE;
        buf_addc(b, tuple ? '(' : '[');
        for (int i = 0; i < l->count; i++) {
            if (i)
                buf_adds(b, ", ");
            if (!value_to_buf(vm, l->items[i], b, true, depth + 1)) {
                fmt_depth--;
                return false;
            }
        }
        if (tuple && l->count == 1)
            buf_addc(b, ',');
        buf_addc(b, tuple ? ')' : ']');
        fmt_depth--;
        return true;
    }
    case OBJ_MAP:
    case OBJ_SET: {
        ObjMap *m = (ObjMap *)o;
        if (!fmt_enter(o, b))
            return true;
        buf_addc(b, '{');
        bool first = true;
        for (int i = 0; i < m->used; i++) {
            if (!m->entries[i].live)
                continue;
            if (!first)
                buf_adds(b, ", ");
            first = false;
            if (!value_to_buf(vm, m->entries[i].key, b, true, depth + 1)) {
                fmt_depth--;
                return false;
            }
            if (o->type == OBJ_MAP) {
                buf_adds(b, ": ");
                if (!value_to_buf(vm, m->entries[i].value, b, true, depth + 1)) {
                    fmt_depth--;
                    return false;
                }
            }
        }
        buf_addc(b, '}');
        fmt_depth--;
        return true;
    }
    case OBJ_INSTANCE: {
        ObjInstance *in = (ObjInstance *)o;
        ObjClass *k = in->klass;
        if (!IS_NULL(k->m_to_string)) {
            Value r;
            if (!vm_call(vm, val_obj(new_bound(vm, v, k->m_to_string)), 0, NULL, &r))
                return false;
            if (IS_STRING(r))
                buf_add(b, AS_CSTR(r), (size_t)AS_STRING(r)->len);
            else
                return value_to_buf(vm, r, b, false, depth + 1);
            return true;
        }
        if (!fmt_enter(o, b))
            return true;
        buf_adds(b, k->name->chars);
        if (!(k->is_variant && in->nfields == 0)) {
            buf_addc(b, '(');
            for (int i = 0; i < in->nfields; i++) {
                if (i)
                    buf_adds(b, ", ");
                buf_adds(b, k->field_names[i]->chars);
                buf_addc(b, '=');
                if (!value_to_buf(vm, in->fields[i], b, true, depth + 1)) {
                    fmt_depth--;
                    return false;
                }
            }
            buf_addc(b, ')');
        }
        fmt_depth--;
        return true;
    }
    case OBJ_ENUMVAL: buf_adds(b, ((ObjEnumVal *)o)->name->chars); return true;
    case OBJ_ENUM: buf_adds(b, ((ObjEnum *)o)->name->chars); return true;
    case OBJ_CLASS: buf_printf(b, "<class %s>", ((ObjClass *)o)->name->chars); return true;
    case OBJ_CLOSURE:
        if (!strcmp(((ObjClosure *)o)->fn->name->chars, "lambda"))
            buf_adds(b, "<function>");
        else
            buf_printf(b, "<function %s>", ((ObjClosure *)o)->fn->name->chars);
        return true;
    case OBJ_FUNCTION: buf_printf(b, "<function %s>", ((ObjFunction *)o)->name->chars); return true;
    case OBJ_NATIVE: buf_printf(b, "<function %s>", ((ObjNative *)o)->name); return true;
    case OBJ_BOUND: {
        Value m = ((ObjBound *)o)->method;
        const char *n = IS_OBJT(m, OBJ_CLOSURE) ? ((ObjClosure *)AS_OBJ(m))->fn->name->chars
                        : IS_OBJT(m, OBJ_NATIVE) ? ((ObjNative *)AS_OBJ(m))->name
                                                 : "?";
        buf_printf(b, "<method %s>", n);
        return true;
    }
    case OBJ_MODULE: buf_printf(b, "<module %s>", ((ObjModule *)o)->name->chars); return true;
    case OBJ_ERROR: {
        ObjError *e = (ObjError *)o;
        buf_add(b, e->message->chars, (size_t)e->message->len);
        return true;
    }
    case OBJ_RESULT: {
        ObjResult *r = (ObjResult *)o;
        buf_adds(b, r->ok ? "Result.ok(" : "Result.error(");
        if (!value_to_buf(vm, r->value, b, true, depth + 1))
            return false;
        buf_addc(b, ')');
        return true;
    }
    case OBJ_ELEMENT: return element_render(vm, v, b);
    case OBJ_JSON: return lib_json_encode(vm, ((ObjJson *)o)->v, b, 0, 0, true);
    case OBJ_TYPE: buf_adds(b, "<type>"); return true;
    case OBJ_FILE: buf_printf(b, "<file %s>", ((ObjFile *)o)->path ? ((ObjFile *)o)->path->chars : "?"); return true;
    case OBJ_WEAK: {
        ObjWeak *w = (ObjWeak *)o;
        buf_adds(b, "weak(");
        if (w->target) {
            if (!value_to_buf(vm, val_obj(w->target), b, true, depth + 1))
                return false;
        } else
            buf_adds(b, "null");
        buf_addc(b, ')');
        return true;
    }
    default: buf_adds(b, "<value>"); return true;
    }
}

ObjString *value_to_string(VM *vm, Value v) {
    if (IS_STRING(v))
        return AS_STRING(v);
    Buf b;
    buf_init(&b);
    if (!value_to_buf(vm, v, &b, false, 0)) {
        buf_free(&b);
        return NULL;
    }
    ObjString *s = str_copy(vm, b.data ? b.data : "", b.len);
    buf_free(&b);
    return s;
}

/* ---------------------------------------------------------------- format specs */

typedef struct {
    uint32_t fill;
    char align;
    char sign;
    bool alt, zero;
    int width;
    char group;
    int precision;
    char type;
    char bad;
} Spec;

static bool parse_spec(const char *s, Spec *sp) {
    memset(sp, 0, sizeof *sp);
    sp->fill = ' ';
    sp->precision = -1;
    int n = (int)strlen(s), p = 0;
    /* fill + align */
    int adv;
    if (n > 0) {
        uint32_t first = utf8_decode(s, n, &adv);
        if (adv < n && strchr("<>^=", s[adv]) && s[adv]) {
            sp->fill = first;
            sp->align = s[adv];
            p = adv + 1;
        } else if (strchr("<>^=", s[0]) && s[0]) {
            sp->align = s[0];
            p = 1;
        }
    }
    if (p < n && (s[p] == '+' || s[p] == '-' || s[p] == ' '))
        sp->sign = s[p++];
    if (p < n && s[p] == '#') {
        sp->alt = true;
        p++;
    }
    if (p < n && s[p] == '0') {
        sp->zero = true;
        p++;
    }
    while (p < n && isdigit((unsigned char)s[p])) {
        sp->width = sp->width * 10 + (s[p++] - '0');
        if (sp->width > 10000)
            return false;
    }
    if (p < n && (s[p] == ',' || s[p] == '_'))
        sp->group = s[p++];
    if (p < n && s[p] == '.') {
        p++;
        if (p >= n || !isdigit((unsigned char)s[p]))
            return false;
        sp->precision = 0;
        while (p < n && isdigit((unsigned char)s[p])) {
            sp->precision = sp->precision * 10 + (s[p++] - '0');
            if (sp->precision > 1000)
                return false;
        }
    }
    if (p < n && strchr("dfeEgGxXobcs%", s[p]))
        sp->type = s[p++];
    if (p + 1 == n && isalpha((unsigned char)s[p])) {
        sp->type = '?';
        sp->bad = s[p];
        return true;
    }
    return p == n;
}

static void group_digits(Buf *out, const char *digits, size_t n, char sep, int every) {
    size_t lead = n % (size_t)every;
    if (lead == 0)
        lead = (size_t)every;
    for (size_t i = 0; i < n; i++) {
        if (i && (i - lead) % (size_t)every == 0 && i >= lead)
            buf_addc(out, sep);
        buf_addc(out, digits[i]);
    }
}

static void pad_out(Buf *out, const char *sign, const char *body, size_t body_len, Spec *sp, char default_align) {
    char align = sp->align ? sp->align : default_align;
    uint32_t fill = sp->fill;
    if (sp->zero && !sp->align) {
        align = '=';
        fill = '0';
    }
    int len = utf8_count(sign, strlen(sign)) + utf8_count(body, body_len);
    int pad = sp->width > len ? sp->width - len : 0;
    int left = 0, right = 0;
    switch (align) {
    case '<': right = pad; break;
    case '^': left = pad / 2; right = pad - left; break;
    case '=':
        buf_adds(out, sign);
        for (int i = 0; i < pad; i++)
            buf_utf8(out, fill);
        buf_add(out, body, body_len);
        return;
    default: left = pad; break;
    }
    for (int i = 0; i < left; i++)
        buf_utf8(out, fill);
    buf_adds(out, sign);
    buf_add(out, body, body_len);
    for (int i = 0; i < right; i++)
        buf_utf8(out, fill);
}

static bool format_integer(VM *vm, int64_t v, Spec *sp, Buf *out) {
    char type = sp->type ? sp->type : 'd';
    if (type == 'c') {
        Buf c;
        buf_init(&c);
        if (v < 0 || v > 0x10ffff) {
            buf_free(&c);
            return vm_throw(vm, "ValueError", "%lld is not a character code", (long long)v);
        }
        buf_utf8(&c, (uint32_t)v);
        pad_out(out, "", c.data, c.len, sp, '<');
        buf_free(&c);
        return true;
    }
    bool neg = v < 0;
    uint64_t mag = neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    char digits[80];
    int nd = 0;
    int base = type == 'x' || type == 'X' ? 16 : type == 'o' ? 8 : type == 'b' ? 2 : 10;
    const char *alphabet = type == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        digits[nd++] = alphabet[mag % (uint64_t)base];
        mag /= (uint64_t)base;
    } while (mag);
    char rev[80];
    for (int i = 0; i < nd; i++)
        rev[i] = digits[nd - 1 - i];
    Buf body;
    buf_init(&body);
    if (sp->alt && base != 10)
        buf_adds(&body, base == 16 ? (type == 'X' ? "0X" : "0x") : base == 8 ? "0o" : "0b");
    if (sp->group)
        group_digits(&body, rev, (size_t)nd, sp->group, base == 10 ? 3 : 4);
    else
        buf_add(&body, rev, (size_t)nd);
    const char *sign = neg ? "-" : sp->sign == '+' ? "+" : sp->sign == ' ' ? " " : "";
    pad_out(out, sign, body.data ? body.data : "", body.len, sp, '>');
    buf_free(&body);
    return true;
}

static bool format_real(VM *vm, double d, Spec *sp, Buf *out) {
    (void)vm;
    char type = sp->type;
    bool neg = signbit(d) && !isnan(d);
    double mag = fabs(d);
    char tmp[1200];
    if (isnan(d) || isinf(d)) {
        snprintf(tmp, sizeof tmp, "%s", isnan(d) ? "nan" : "inf");
    } else if (type == 'f' || type == '%') {
        double x = type == '%' ? mag * 100.0 : mag;
        snprintf(tmp, sizeof tmp, "%.*f", sp->precision >= 0 ? sp->precision : 6, x);
    } else if (type == 'e' || type == 'E') {
        snprintf(tmp, sizeof tmp, type == 'e' ? "%.*e" : "%.*E", sp->precision >= 0 ? sp->precision : 6, mag);
    } else if (type == 'g' || type == 'G') {
        snprintf(tmp, sizeof tmp, type == 'g' ? "%.*g" : "%.*G", sp->precision >= 0 ? (sp->precision ? sp->precision : 1) : 6, mag);
    } else {
        if (sp->precision >= 0)
            snprintf(tmp, sizeof tmp, "%.*g", sp->precision ? sp->precision : 1, mag);
        else {
            Buf t;
            buf_init(&t);
            format_double(mag, &t);
            snprintf(tmp, sizeof tmp, "%s", t.data);
            buf_free(&t);
        }
    }
    Buf body;
    buf_init(&body);
    if (sp->group && !isnan(d) && !isinf(d)) {
        size_t intlen = strspn(tmp, "0123456789");
        group_digits(&body, tmp, intlen, sp->group, 3);
        buf_adds(&body, tmp + intlen);
    } else
        buf_adds(&body, tmp);
    if (type == '%')
        buf_addc(&body, '%');
    const char *sign = neg ? "-" : sp->sign == '+' ? "+" : sp->sign == ' ' ? " " : "";
    pad_out(out, sign, body.data, body.len, sp, '>');
    buf_free(&body);
    return true;
}

bool value_format_spec(VM *vm, Value v, const char *spec, Buf *out) {
    Spec sp;
    if (!parse_spec(spec, &sp))
        return vm_throw(vm, "ValueError", "invalid format spec '%s'", spec);
    char t = sp.type;
    if (t == '?')
        return vm_throw(vm, "ValueError", "unknown %s format type '%c'",
                        IS_INT(v) ? "integer" : IS_DOUBLE(v) ? "floating" : "string", sp.bad);
    if (IS_INT(v) && t != 's') {
        if (t == 'f' || t == 'e' || t == 'E' || t == 'g' || t == 'G' || t == '%')
            return format_real(vm, (double)v.as.i, &sp, out);
        if (!t && sp.precision >= 0)
            return format_integer(vm, v.as.i, &sp, out);
        return format_integer(vm, v.as.i, &sp, out);
    }
    if (IS_DOUBLE(v) && t != 's') {
        if (t == 'd' || t == 'x' || t == 'X' || t == 'o' || t == 'b' || t == 'c') {
            double d = v.as.d;
            if (d != floor(d) || !(d >= -9.2e18 && d <= 9.2e18))
                return vm_throw(vm, "ValueError", "format type '%c' needs a whole number", t);
            return format_integer(vm, (int64_t)d, &sp, out);
        }
        return format_real(vm, v.as.d, &sp, out);
    }
    if (IS_CHAR(v) && t == 'd')
        return format_integer(vm, v.as.c, &sp, out);
    if (t && t != 's' && t != 'c')
        return vm_throw(vm, "ValueError", "format type '%c' needs a number", t);
    Buf text;
    buf_init(&text);
    if (!value_to_buf(vm, v, &text, false, 0)) {
        buf_free(&text);
        return false;
    }
    size_t n = text.len;
    if (sp.precision >= 0 && text.data) {
        ObjString tmp;
        (void)tmp;
        int count = 0;
        size_t i = 0;
        while (i < text.len && count < sp.precision) {
            i++;
            while (i < text.len && ((unsigned char)text.data[i] & 0xc0) == 0x80)
                i++;
            count++;
        }
        n = i;
    }
    bool numeric = IS_NUM(v);
    if (sp.zero && !sp.align && !numeric) {
        sp.zero = false;
        sp.fill = '0';
        sp.align = '>';
    }
    pad_out(out, "", text.data ? text.data : "", n, &sp, numeric ? '>' : '<');
    buf_free(&text);
    return true;
}

/* ---------------------------------------------------------------- HTML */

void html_escape(Buf *b, const char *s, size_t n, bool attr) {
    (void)attr;
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
        case '&': buf_adds(b, "&amp;"); break;
        case '<': buf_adds(b, "&lt;"); break;
        case '>': buf_adds(b, "&gt;"); break;
        case '"': buf_adds(b, "&quot;"); break;
        case '\'': buf_adds(b, "&#39;"); break;
        default: buf_addc(b, s[i]);
        }
    }
}
static bool is_void_tag(const char *t) {
    static const char *tags[] = {"area", "base", "br", "col", "embed", "hr", "img", "input",
                                 "link", "meta", "param", "source", "track", "wbr", NULL};
    for (int i = 0; tags[i]; i++)
        if (!strcmp(tags[i], t))
            return true;
    return false;
}
static bool render_node(VM *vm, Value v, Buf *b, int depth) {
    if (depth > 512)
        return vm_throw(vm, "ValueError", "markup nested too deeply");
    if (IS_OBJT(v, OBJ_ELEMENT)) {
        ObjElement *e = AS_ELEMENT(v);
        if (e->tag) {
            buf_addc(b, '<');
            buf_add(b, e->tag->chars, (size_t)e->tag->len);
            for (int i = 0; i < e->attr_names.count; i++) {
                Value av = e->attr_values.items[i];
                if (IS_NULL(av) || (IS_BOOL(av) && !av.as.b))
                    continue;
                if (IS_OBJ(av) && (OBJ_TYPE(av) == OBJ_CLOSURE || OBJ_TYPE(av) == OBJ_BOUND || OBJ_TYPE(av) == OBJ_NATIVE))
                    continue;
                buf_addc(b, ' ');
                ObjString *n = AS_STRING(e->attr_names.items[i]);
                buf_add(b, n->chars, (size_t)n->len);
                buf_adds(b, "=\"");
                if (!IS_BOOL(av)) {
                    if (IS_STRING(av))
                        html_escape(b, AS_CSTR(av), (size_t)AS_STRING(av)->len, true);
                    else {
                        Buf t;
                        buf_init(&t);
                        if (!value_to_buf(vm, av, &t, false, 0)) {
                            buf_free(&t);
                            return false;
                        }
                        html_escape(b, t.data ? t.data : "", t.len, true);
                        buf_free(&t);
                    }
                }
                buf_addc(b, '"');
            }
            buf_addc(b, '>');
            if (is_void_tag(e->tag->chars) && e->children.count == 0)
                return true;
        }
        for (int i = 0; i < e->children.count; i++)
            if (!render_node(vm, e->children.items[i], b, depth + 1))
                return false;
        if (e->tag) {
            buf_adds(b, "</");
            buf_add(b, e->tag->chars, (size_t)e->tag->len);
            buf_addc(b, '>');
        }
        return true;
    }
    if (IS_STRING(v)) {
        html_escape(b, AS_CSTR(v), (size_t)AS_STRING(v)->len, false);
        return true;
    }
    Buf t;
    buf_init(&t);
    if (!value_to_buf(vm, v, &t, false, 0)) {
        buf_free(&t);
        return false;
    }
    html_escape(b, t.data ? t.data : "", t.len, false);
    buf_free(&t);
    return true;
}
bool element_render(VM *vm, Value v, Buf *b) { return render_node(vm, v, b, 0); }
