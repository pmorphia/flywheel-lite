/* Allocation, object construction, interning, tables, and the mark-sweep collector.
 * Collection only happens at VM safepoints (calls and loop back-edges), so native code may
 * hold unrooted objects except across calls back into the VM (use push_root). */
#include "lite.h"

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("flywheel-lite: out of memory\n", stderr);
        exit(70);
    }
    return p;
}
void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("flywheel-lite: out of memory\n", stderr);
        exit(70);
    }
    return q;
}
char *xstrndup(const char *s, size_t n) {
    char *r = xmalloc(n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}
char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

void *tb_realloc(VM *vm, void *ptr, size_t old, size_t size) {
    vm->bytes_allocated += size;
    vm->bytes_allocated -= old;
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    return xrealloc(ptr, size);
}

Obj *alloc_obj(VM *vm, size_t size, ObjType type) {
    Obj *o = tb_realloc(vm, NULL, 0, size);
    memset(o, 0, size);
    o->type = (uint8_t)type;
    o->next = vm->objects;
    vm->objects = o;
    return o;
}
#define ALLOC_OBJ(vm, T, kind) ((T *)alloc_obj(vm, sizeof(T), kind))

void push_root(VM *vm, Value v) {
    if (vm->root_count >= (int)(sizeof vm->roots / sizeof vm->roots[0])) {
        fputs("flywheel-lite: too many temporary roots\n", stderr);
        abort();
    }
    vm->roots[vm->root_count++] = v;
}
void pop_root(VM *vm, int n) { vm->root_count -= n; }

/* ---------------------------------------------------------------- arrays */

void va_init(ValueArray *a) {
    a->items = NULL;
    a->count = a->cap = 0;
}
void va_push(VM *vm, ValueArray *a, Value v) {
    if (a->count == a->cap) {
        int nc = GROW_CAP(a->cap);
        a->items = tb_realloc(vm, a->items, sizeof(Value) * (size_t)a->cap, sizeof(Value) * (size_t)nc);
        a->cap = nc;
    }
    a->items[a->count++] = v;
}
void va_free(VM *vm, ValueArray *a) {
    TB_FREE(vm, Value, a->items, a->cap);
    va_init(a);
}

/* ---------------------------------------------------------------- strings */

uint32_t hash_bytes(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}
int utf8_count(const char *s, size_t n) {
    int c = 0;
    for (size_t i = 0; i < n; i++)
        if (((uint8_t)s[i] & 0xc0) != 0x80)
            c++;
    return c;
}
static ObjString *str_alloc(VM *vm, const char *chars, size_t len, uint32_t hash) {
    if (len > 0x7fffffff) {
        fputs("flywheel-lite: string too large\n", stderr);
        exit(70);
    }
    ObjString *s = (ObjString *)alloc_obj(vm, sizeof(ObjString) + len + 1, OBJ_STRING);
    s->len = (int)len;
    memcpy(s->chars, chars, len);
    s->chars[len] = 0;
    s->hash = hash;
    s->clen = utf8_count(chars, len);
    return s;
}
ObjString *str_copy(VM *vm, const char *chars, size_t len) {
    return str_alloc(vm, chars, len, hash_bytes(chars, len));
}
ObjString *str_cstr(VM *vm, const char *chars) { return str_copy(vm, chars, strlen(chars)); }
ObjString *str_intern(VM *vm, const char *chars, size_t len) {
    uint32_t h = hash_bytes(chars, len);
    ObjString *s = table_find_string(&vm->strings, chars, len, h);
    if (s)
        return s;
    s = str_alloc(vm, chars, len, h);
    push_root(vm, val_obj(s));
    table_set(vm, &vm->strings, s, val_null());
    pop_root(vm, 1);
    return s;
}
ObjString *str_intern_c(VM *vm, const char *chars) { return str_intern(vm, chars, strlen(chars)); }
ObjString *str_take_buf(VM *vm, char *chars, size_t len) {
    ObjString *s = str_copy(vm, chars, len);
    free(chars);
    return s;
}
ObjString *str_fmt(VM *vm, const char *fmt, ...) {
    char small[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    if ((size_t)n < sizeof small)
        return str_copy(vm, small, (size_t)n);
    char *big = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return str_take_buf(vm, big, (size_t)n);
}

/* ---------------------------------------------------------------- objects */

ObjList *new_list(VM *vm, ObjType type, int cap) {
    ObjList *l = ALLOC_OBJ(vm, ObjList, type);
    if (cap > 0) {
        l->items = TB_ALLOC(vm, Value, cap);
        l->cap = cap;
    }
    return l;
}
void list_push(VM *vm, ObjList *l, Value v) {
    if (l->count == l->cap) {
        int nc = GROW_CAP(l->cap);
        l->items = tb_realloc(vm, l->items, sizeof(Value) * (size_t)l->cap, sizeof(Value) * (size_t)nc);
        l->cap = nc;
    }
    l->items[l->count++] = v;
}
ObjMap *new_map(VM *vm, ObjType type) { return ALLOC_OBJ(vm, ObjMap, type); }
ObjFunction *new_function(VM *vm) {
    ObjFunction *f = ALLOC_OBJ(vm, ObjFunction, OBJ_FUNCTION);
    chunk_init(&f->chunk);
    return f;
}
ObjClosure *new_closure(VM *vm, ObjFunction *fn) {
    ObjUpvalue **ups = NULL;
    if (fn->upvalue_count) {
        ups = TB_ALLOC(vm, ObjUpvalue *, fn->upvalue_count);
        for (int i = 0; i < fn->upvalue_count; i++)
            ups[i] = NULL;
    }
    ObjClosure *c = ALLOC_OBJ(vm, ObjClosure, OBJ_CLOSURE);
    c->fn = fn;
    c->upvalues = ups;
    c->upvalue_count = fn->upvalue_count;
    return c;
}
ObjUpvalue *new_upvalue(VM *vm, Value *slot) {
    ObjUpvalue *u = ALLOC_OBJ(vm, ObjUpvalue, OBJ_UPVALUE);
    u->location = slot;
    u->closed = val_null();
    return u;
}
ObjNative *new_native(VM *vm, const char *name, NativeFn fn, int min, int max) {
    ObjNative *n = ALLOC_OBJ(vm, ObjNative, OBJ_NATIVE);
    n->fn = fn;
    n->name = name;
    n->min_args = min;
    n->max_args = max;
    n->data = val_null();
    return n;
}
ObjBound *new_bound(VM *vm, Value receiver, Value method) {
    ObjBound *b = ALLOC_OBJ(vm, ObjBound, OBJ_BOUND);
    b->receiver = receiver;
    b->method = method;
    return b;
}
ObjClass *new_class(VM *vm, ObjString *name) {
    ObjClass *k = ALLOC_OBJ(vm, ObjClass, OBJ_CLASS);
    k->name = name;
    table_init(&k->field_index);
    table_init(&k->methods);
    table_init(&k->statics);
    k->defaults = val_null();
    k->singleton = val_null();
    Value n = val_null();
    k->m_init = k->m_to_string = k->m_eq = k->m_lt = k->m_le = k->m_gt = k->m_ge = n;
    k->m_hash = k->m_equals = k->m_next = k->m_iterator = n;
    return k;
}
ObjInstance *new_instance(VM *vm, ObjClass *klass) {
    ObjInstance *i = (ObjInstance *)alloc_obj(
        vm, sizeof(ObjInstance) + sizeof(Value) * (size_t)klass->nfields, OBJ_INSTANCE);
    i->klass = klass;
    i->nfields = klass->nfields;
    for (int f = 0; f < klass->nfields; f++)
        i->fields[f] = val_undef();
    return i;
}
ObjEnum *new_enum(VM *vm, ObjString *name, int count) {
    ObjEnum *e = ALLOC_OBJ(vm, ObjEnum, OBJ_ENUM);
    e->name = name;
    e->members = TB_ALLOC(vm, Value, count);
    for (int i = 0; i < count; i++)
        e->members[i] = val_null();
    e->count = count;
    return e;
}
ObjModule *new_module(VM *vm, ObjString *name) {
    ObjModule *m = ALLOC_OBJ(vm, ObjModule, OBJ_MODULE);
    m->name = name;
    table_init(&m->names);
    va_init(&m->values);
    va_init(&m->names_by_slot);
    m->init = val_null();
    m->result = val_null();
    return m;
}
ObjError *new_error(VM *vm, ObjString *type, ObjString *message) {
    ObjError *e = ALLOC_OBJ(vm, ObjError, OBJ_ERROR);
    e->type = type;
    e->message = message;
    e->payload = val_null();
    return e;
}
ObjResult *new_result(VM *vm, bool ok, Value v) {
    ObjResult *r = ALLOC_OBJ(vm, ObjResult, OBJ_RESULT);
    r->ok = ok;
    r->value = v;
    return r;
}
ObjElement *new_element(VM *vm, ObjString *tag) {
    ObjElement *e = ALLOC_OBJ(vm, ObjElement, OBJ_ELEMENT);
    e->tag = tag;
    va_init(&e->attr_names);
    va_init(&e->attr_values);
    va_init(&e->children);
    return e;
}
ObjJson *new_json(VM *vm, Value v) {
    ObjJson *j = ALLOC_OBJ(vm, ObjJson, OBJ_JSON);
    j->v = v;
    return j;
}
ObjTypeDesc *new_type(VM *vm, TypeKind kind) {
    ObjTypeDesc *t = ALLOC_OBJ(vm, ObjTypeDesc, OBJ_TYPE);
    t->kind = (uint8_t)kind;
    return t;
}
ObjFile *new_file(VM *vm, FILE *fp, ObjString *path) {
    ObjFile *f = ALLOC_OBJ(vm, ObjFile, OBJ_FILE);
    f->fp = fp;
    f->path = path;
    return f;
}
ObjWeak *new_weak(VM *vm, Obj *target) {
    ObjWeak *w = ALLOC_OBJ(vm, ObjWeak, OBJ_WEAK);
    w->target = target;
    return w;
}

/* ---------------------------------------------------------------- tables */

void table_init(Table *t) {
    t->count = t->cap = 0;
    t->entries = NULL;
}
void table_free(VM *vm, Table *t) {
    TB_FREE(vm, Entry, t->entries, t->cap);
    table_init(t);
}
static Entry *find_entry(Entry *entries, int cap, ObjString *key) {
    uint32_t i = key->hash & (uint32_t)(cap - 1);
    Entry *tomb = NULL;
    for (;;) {
        Entry *e = &entries[i];
        if (e->key == NULL) {
            if (IS_NULL(e->value))
                return tomb ? tomb : e;
            if (!tomb)
                tomb = e;
        } else if (e->key == key) {
            return e;
        }
        i = (i + 1) & (uint32_t)(cap - 1);
    }
}
bool table_get(Table *t, ObjString *key, Value *out) {
    if (t->count == 0)
        return false;
    Entry *e = find_entry(t->entries, t->cap, key);
    if (!e->key)
        return false;
    *out = e->value;
    return true;
}
static void table_grow(VM *vm, Table *t, int cap) {
    Entry *entries = TB_ALLOC(vm, Entry, cap);
    for (int i = 0; i < cap; i++) {
        entries[i].key = NULL;
        entries[i].value = val_null();
    }
    t->count = 0;
    for (int i = 0; i < t->cap; i++) {
        Entry *e = &t->entries[i];
        if (!e->key)
            continue;
        Entry *d = find_entry(entries, cap, e->key);
        d->key = e->key;
        d->value = e->value;
        t->count++;
    }
    TB_FREE(vm, Entry, t->entries, t->cap);
    t->entries = entries;
    t->cap = cap;
}
bool table_set(VM *vm, Table *t, ObjString *key, Value v) {
    if ((t->count + 1) * 4 > t->cap * 3)
        table_grow(vm, t, GROW_CAP(t->cap));
    Entry *e = find_entry(t->entries, t->cap, key);
    bool fresh = e->key == NULL;
    if (fresh && IS_NULL(e->value))
        t->count++;
    e->key = key;
    e->value = v;
    return fresh;
}
bool table_delete(Table *t, ObjString *key) {
    if (t->count == 0)
        return false;
    Entry *e = find_entry(t->entries, t->cap, key);
    if (!e->key)
        return false;
    e->key = NULL;
    e->value = val_bool(true);
    return true;
}
ObjString *table_find_string(Table *t, const char *chars, size_t len, uint32_t hash) {
    if (t->count == 0)
        return NULL;
    uint32_t i = hash & (uint32_t)(t->cap - 1);
    for (;;) {
        Entry *e = &t->entries[i];
        if (e->key == NULL) {
            if (IS_NULL(e->value))
                return NULL;
        } else if (e->key->hash == hash && (size_t)e->key->len == len &&
                   memcmp(e->key->chars, chars, len) == 0) {
            return e->key;
        }
        i = (i + 1) & (uint32_t)(t->cap - 1);
    }
}
void table_copy(VM *vm, Table *from, Table *to) {
    for (int i = 0; i < from->cap; i++)
        if (from->entries[i].key)
            table_set(vm, to, from->entries[i].key, from->entries[i].value);
}

/* ---------------------------------------------------------------- modules */

int module_find(ObjModule *m, ObjString *name) {
    Value v;
    if (table_get(&m->names, name, &v))
        return (int)v.as.i;
    return -1;
}
int module_slot(VM *vm, ObjModule *m, ObjString *name) {
    int s = module_find(m, name);
    if (s >= 0)
        return s;
    s = m->values.count;
    va_push(vm, &m->values, val_undef());
    va_push(vm, &m->names_by_slot, val_obj(name));
    table_set(vm, &m->names, name, val_int(s));
    if (s >= m->private_cap) {
        int nc = GROW_CAP(m->private_cap);
        while (nc <= s)
            nc *= 2;
        m->is_private = xrealloc(m->is_private, (size_t)nc);
        memset(m->is_private + m->private_cap, 0, (size_t)(nc - m->private_cap));
        m->is_component_slot = xrealloc(m->is_component_slot, (size_t)nc);
        memset(m->is_component_slot + m->private_cap, 0, (size_t)(nc - m->private_cap));
        m->private_cap = nc;
    }
    return s;
}
void module_set_native(VM *vm, ObjModule *m, const char *name, NativeFn fn, int min, int max) {
    ObjNative *n = new_native(vm, name, fn, min, max);
    push_root(vm, val_obj(n));
    int s = module_slot(vm, m, str_intern_c(vm, name));
    m->values.items[s] = val_obj(n);
    pop_root(vm, 1);
}
void module_set_value(VM *vm, ObjModule *m, const char *name, Value v) {
    push_root(vm, v);
    int s = module_slot(vm, m, str_intern_c(vm, name));
    m->values.items[s] = v;
    pop_root(vm, 1);
}

/* ---------------------------------------------------------------- chunks */

void chunk_init(Chunk *c) {
    c->code = NULL;
    c->lines = NULL;
    c->count = c->cap = 0;
    va_init(&c->constants);
}
void chunk_write(VM *vm, Chunk *c, uint8_t byte, int line) {
    if (c->count == c->cap) {
        int nc = GROW_CAP(c->cap);
        c->code = tb_realloc(vm, c->code, (size_t)c->cap, (size_t)nc);
        c->lines = tb_realloc(vm, c->lines, sizeof(int) * (size_t)c->cap, sizeof(int) * (size_t)nc);
        c->cap = nc;
    }
    c->code[c->count] = byte;
    c->lines[c->count] = line;
    c->count++;
}
int chunk_add_const(VM *vm, Chunk *c, Value v) {
    /* Reuse identical constants (interned strings, small scalars). */
    for (int i = 0; i < c->constants.count; i++) {
        Value k = c->constants.items[i];
        if (k.type != v.type)
            continue;
        if ((IS_OBJ(v) && AS_OBJ(k) == AS_OBJ(v) && OBJ_TYPE(v) == OBJ_STRING) ||
            (IS_INT(v) && k.as.i == v.as.i) || (IS_CHAR(v) && k.as.c == v.as.c))
            return i;
    }
    va_push(vm, &c->constants, v);
    return c->constants.count - 1;
}

/* ---------------------------------------------------------------- arena */

typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t used, cap;
    char data[];
} ArenaBlock;
struct Arena {
    ArenaBlock *head;
};
Arena *arena_new(void) {
    Arena *a = xmalloc(sizeof(Arena));
    a->head = NULL;
    return a;
}
void *arena_alloc(Arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > 65536 ? n : 65536;
        ArenaBlock *b = xmalloc(sizeof(ArenaBlock) + cap);
        b->next = a->head;
        b->used = 0;
        b->cap = cap;
        a->head = b;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += n;
    memset(p, 0, n);
    return p;
}
char *arena_strndup(Arena *a, const char *s, size_t n) {
    char *r = arena_alloc(a, n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}
void arena_free(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *n = b->next;
        free(b);
        b = n;
    }
    free(a);
}

/* ---------------------------------------------------------------- collector */

static void mark_obj(VM *vm, Obj *o) {
    if (!o || o->marked)
        return;
    o->marked = true;
    if (vm->gray_count == vm->gray_cap) {
        vm->gray_cap = GROW_CAP(vm->gray_cap);
        vm->gray = xrealloc(vm->gray, sizeof(Obj *) * (size_t)vm->gray_cap);
    }
    vm->gray[vm->gray_count++] = o;
}
static inline void mark_val(VM *vm, Value v) {
    if (IS_OBJ(v))
        mark_obj(vm, AS_OBJ(v));
}
static void mark_array(VM *vm, ValueArray *a) {
    for (int i = 0; i < a->count; i++)
        mark_val(vm, a->items[i]);
}
static void mark_table(VM *vm, Table *t) {
    for (int i = 0; i < t->cap; i++) {
        Entry *e = &t->entries[i];
        if (e->key) {
            mark_obj(vm, (Obj *)e->key);
            mark_val(vm, e->value);
        }
    }
}
static void blacken(VM *vm, Obj *o) {
    switch ((ObjType)o->type) {
    case OBJ_STRING:
    case OBJ_NATIVE:
        if (o->type == OBJ_NATIVE)
            mark_val(vm, ((ObjNative *)o)->data);
        break;
    case OBJ_ARRAY:
    case OBJ_TUPLE: {
        ObjList *l = (ObjList *)o;
        for (int i = 0; i < l->count; i++)
            mark_val(vm, l->items[i]);
        break;
    }
    case OBJ_MAP:
    case OBJ_SET: {
        ObjMap *m = (ObjMap *)o;
        for (int i = 0; i < m->used; i++)
            if (m->entries[i].live) {
                mark_val(vm, m->entries[i].key);
                mark_val(vm, m->entries[i].value);
            }
        break;
    }
    case OBJ_FUNCTION: {
        ObjFunction *f = (ObjFunction *)o;
        mark_obj(vm, (Obj *)f->name);
        mark_obj(vm, (Obj *)f->module);
        mark_obj(vm, (Obj *)f->owner);
        mark_array(vm, &f->chunk.constants);
        break;
    }
    case OBJ_CLOSURE: {
        ObjClosure *c = (ObjClosure *)o;
        mark_obj(vm, (Obj *)c->fn);
        for (int i = 0; i < c->upvalue_count; i++)
            mark_obj(vm, (Obj *)c->upvalues[i]);
        break;
    }
    case OBJ_UPVALUE:
        mark_val(vm, ((ObjUpvalue *)o)->closed);
        break;
    case OBJ_BOUND:
        mark_val(vm, ((ObjBound *)o)->receiver);
        mark_val(vm, ((ObjBound *)o)->method);
        break;
    case OBJ_CLASS: {
        ObjClass *k = (ObjClass *)o;
        mark_obj(vm, (Obj *)k->name);
        mark_obj(vm, (Obj *)k->super);
        mark_obj(vm, (Obj *)k->module);
        for (int i = 0; i < k->nfields; i++) {
            mark_obj(vm, (Obj *)k->field_names[i]);
            mark_val(vm, k->field_types[i]);
            mark_val(vm, k->field_consts[i]);
        }
        mark_table(vm, &k->field_index);
        mark_table(vm, &k->methods);
        mark_table(vm, &k->statics);
        for (int i = 0; i < k->ninterfaces; i++)
            mark_obj(vm, (Obj *)k->interfaces[i]);
        mark_val(vm, k->defaults);
        mark_val(vm, k->singleton);
        Value *ms[] = {&k->m_init, &k->m_to_string, &k->m_eq,   &k->m_lt,     &k->m_le,   &k->m_gt,
                       &k->m_ge,   &k->m_hash,      &k->m_equals, &k->m_next, &k->m_iterator};
        for (size_t i = 0; i < sizeof ms / sizeof ms[0]; i++)
            mark_val(vm, *ms[i]);
        break;
    }
    case OBJ_INSTANCE: {
        ObjInstance *i = (ObjInstance *)o;
        mark_obj(vm, (Obj *)i->klass);
        for (int f = 0; f < i->nfields; f++)
            mark_val(vm, i->fields[f]);
        break;
    }
    case OBJ_ENUM: {
        ObjEnum *e = (ObjEnum *)o;
        mark_obj(vm, (Obj *)e->name);
        for (int i = 0; i < e->count; i++)
            mark_val(vm, e->members[i]);
        break;
    }
    case OBJ_ENUMVAL:
        mark_obj(vm, (Obj *)((ObjEnumVal *)o)->e);
        mark_obj(vm, (Obj *)((ObjEnumVal *)o)->name);
        break;
    case OBJ_MODULE: {
        ObjModule *m = (ObjModule *)o;
        mark_obj(vm, (Obj *)m->name);
        mark_obj(vm, (Obj *)m->path);
        mark_table(vm, &m->names);
        mark_array(vm, &m->values);
        mark_array(vm, &m->names_by_slot);
        mark_val(vm, m->init);
        mark_val(vm, m->result);
        break;
    }
    case OBJ_ERROR:
        mark_obj(vm, (Obj *)((ObjError *)o)->type);
        mark_obj(vm, (Obj *)((ObjError *)o)->message);
        mark_val(vm, ((ObjError *)o)->payload);
        break;
    case OBJ_RESULT:
        mark_val(vm, ((ObjResult *)o)->value);
        break;
    case OBJ_ELEMENT: {
        ObjElement *e = (ObjElement *)o;
        mark_obj(vm, (Obj *)e->tag);
        mark_array(vm, &e->attr_names);
        mark_array(vm, &e->attr_values);
        mark_array(vm, &e->children);
        mark_obj(vm, (Obj *)e->parent);
        break;
    }
    case OBJ_JSON:
        mark_val(vm, ((ObjJson *)o)->v);
        break;
    case OBJ_TYPE: {
        ObjTypeDesc *t = (ObjTypeDesc *)o;
        mark_obj(vm, (Obj *)t->name);
        mark_obj(vm, (Obj *)t->module);
        mark_obj(vm, (Obj *)t->member);
        for (int i = 0; i < t->nargs; i++)
            mark_obj(vm, (Obj *)t->args[i]);
        break;
    }
    case OBJ_FILE:
        mark_obj(vm, (Obj *)((ObjFile *)o)->path);
        break;
    case OBJ_WEAK:
        break; /* weak: not traced */
    default:
        break;
    }
}

static void free_obj(VM *vm, Obj *o) {
    switch ((ObjType)o->type) {
    case OBJ_STRING:
        tb_realloc(vm, o, sizeof(ObjString) + (size_t)((ObjString *)o)->len + 1, 0);
        return;
    case OBJ_ARRAY:
    case OBJ_TUPLE: {
        ObjList *l = (ObjList *)o;
        TB_FREE(vm, Value, l->items, l->cap);
        tb_realloc(vm, o, sizeof(ObjList), 0);
        return;
    }
    case OBJ_MAP:
    case OBJ_SET: {
        ObjMap *m = (ObjMap *)o;
        TB_FREE(vm, MapEntry, m->entries, m->cap);
        TB_FREE(vm, int32_t, m->index, m->index_cap);
        tb_realloc(vm, o, sizeof(ObjMap), 0);
        return;
    }
    case OBJ_FUNCTION: {
        ObjFunction *f = (ObjFunction *)o;
        tb_realloc(vm, f->chunk.code, (size_t)f->chunk.cap, 0);
        TB_FREE(vm, int, f->chunk.lines, f->chunk.cap);
        va_free(vm, &f->chunk.constants);
        TB_FREE(vm, InlineCache, f->caches, f->ncaches);
        tb_realloc(vm, o, sizeof(ObjFunction), 0);
        return;
    }
    case OBJ_CLOSURE: {
        ObjClosure *c = (ObjClosure *)o;
        TB_FREE(vm, ObjUpvalue *, c->upvalues, c->upvalue_count);
        tb_realloc(vm, o, sizeof(ObjClosure), 0);
        return;
    }
    case OBJ_UPVALUE:
        tb_realloc(vm, o, sizeof(ObjUpvalue), 0);
        return;
    case OBJ_NATIVE:
        tb_realloc(vm, o, sizeof(ObjNative), 0);
        return;
    case OBJ_BOUND:
        tb_realloc(vm, o, sizeof(ObjBound), 0);
        return;
    case OBJ_CLASS: {
        ObjClass *k = (ObjClass *)o;
        TB_FREE(vm, ObjString *, k->field_names, k->nfields);
        TB_FREE(vm, Value, k->field_types, k->nfields);
        TB_FREE(vm, bool, k->field_has_default, k->nfields);
        TB_FREE(vm, Value, k->field_consts, k->nfields);
        table_free(vm, &k->field_index);
        table_free(vm, &k->methods);
        table_free(vm, &k->statics);
        TB_FREE(vm, ObjClass *, k->interfaces, k->ninterfaces);
        tb_realloc(vm, o, sizeof(ObjClass), 0);
        return;
    }
    case OBJ_INSTANCE:
        tb_realloc(vm, o, sizeof(ObjInstance) + sizeof(Value) * (size_t)((ObjInstance *)o)->nfields, 0);
        return;
    case OBJ_ENUM:
        TB_FREE(vm, Value, ((ObjEnum *)o)->members, ((ObjEnum *)o)->count);
        tb_realloc(vm, o, sizeof(ObjEnum), 0);
        return;
    case OBJ_ENUMVAL:
        tb_realloc(vm, o, sizeof(ObjEnumVal), 0);
        return;
    case OBJ_MODULE: {
        ObjModule *m = (ObjModule *)o;
        table_free(vm, &m->names);
        va_free(vm, &m->values);
        va_free(vm, &m->names_by_slot);
        free(m->is_private);
        free(m->is_component_slot);
        tb_realloc(vm, o, sizeof(ObjModule), 0);
        return;
    }
    case OBJ_ERROR:
        tb_realloc(vm, o, sizeof(ObjError), 0);
        return;
    case OBJ_RESULT:
        tb_realloc(vm, o, sizeof(ObjResult), 0);
        return;
    case OBJ_ELEMENT: {
        ObjElement *e = (ObjElement *)o;
        va_free(vm, &e->attr_names);
        va_free(vm, &e->attr_values);
        va_free(vm, &e->children);
        tb_realloc(vm, o, sizeof(ObjElement), 0);
        return;
    }
    case OBJ_JSON:
        tb_realloc(vm, o, sizeof(ObjJson), 0);
        return;
    case OBJ_TYPE: {
        ObjTypeDesc *t = (ObjTypeDesc *)o;
        TB_FREE(vm, ObjTypeDesc *, t->args, t->nargs);
        tb_realloc(vm, o, sizeof(ObjTypeDesc), 0);
        return;
    }
    case OBJ_FILE: {
        ObjFile *f = (ObjFile *)o;
        if (f->fp && !f->closed && !f->is_std)
            fclose(f->fp);
        tb_realloc(vm, o, sizeof(ObjFile), 0);
        return;
    }
    case OBJ_WEAK:
        tb_realloc(vm, o, sizeof(ObjWeak), 0);
        return;
    default:
        free(o);
    }
}

void gc_collect(VM *vm) {
    for (Value *s = vm->stack; s < vm->sp; s++)
        mark_val(vm, *s);
    for (int i = 0; i < vm->frame_count; i++)
        mark_obj(vm, (Obj *)vm->frames[i].closure);
    for (ObjUpvalue *u = vm->open_upvalues; u; u = u->next)
        mark_obj(vm, (Obj *)u);
    for (int i = 0; i < vm->root_count; i++)
        mark_val(vm, vm->roots[i]);
    mark_table(vm, &vm->builtins);
    mark_table(vm, &vm->modules);
    mark_array(vm, &vm->module_list);
    mark_val(vm, vm->error);
    for (size_t i = 0; i < sizeof vm->desc_args / sizeof vm->desc_args[0]; i++)
        mark_val(vm, vm->desc_args[i]);
    for (int i = 0; i < TM_COUNT; i++)
        mark_table(vm, &vm->type_methods[i]);
    mark_val(vm, vm->empty_fragment);
    Obj *named[] = {(Obj *)vm->s_init, (Obj *)vm->s_to_string, (Obj *)vm->s_length,
                    (Obj *)vm->s_message, (Obj *)vm->s_name};
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++)
        mark_obj(vm, named[i]);
    while (vm->gray_count > 0)
        blacken(vm, vm->gray[--vm->gray_count]);
    /* Interned strings are weak. */
    for (int i = 0; i < vm->strings.cap; i++) {
        Entry *e = &vm->strings.entries[i];
        if (e->key && !e->key->obj.marked)
            table_delete(&vm->strings, e->key);
    }
    /* Weak references clear when their target dies. */
    for (Obj *o = vm->objects; o; o = o->next)
        if (o->type == OBJ_WEAK && o->marked) {
            ObjWeak *w = (ObjWeak *)o;
            if (w->target && !w->target->marked)
                w->target = NULL;
        }
    Obj **link = &vm->objects;
    while (*link) {
        Obj *o = *link;
        if (o->marked) {
            o->marked = false;
            link = &o->next;
        } else {
            *link = o->next;
            free_obj(vm, o);
        }
    }
    vm->next_gc = vm->bytes_allocated * 2;
    if (vm->next_gc < 8u * 1024 * 1024)
        vm->next_gc = 8u * 1024 * 1024;
    if (vm->gc_stress) /* every safepoint; on deep stacks, every few hundred kilobytes */
        vm->next_gc = vm->frame_count > 500 ? vm->bytes_allocated + 256 * 1024 : 0;
}

void free_all_objects(VM *vm) {
    Obj *o = vm->objects;
    while (o) {
        Obj *n = o->next;
        free_obj(vm, o);
        o = n;
    }
    vm->objects = NULL;
    free(vm->gray);
    vm->gray = NULL;
}
