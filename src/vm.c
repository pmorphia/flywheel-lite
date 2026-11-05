/* The bytecode interpreter: dispatch loop, calls, errors, members, classes, and modules. */
#include "opcodes.h"
#include "turbine.h"
#include <math.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define PATH_SEP '\\'
#else
#include <unistd.h>
#define PATH_SEP '/'
#endif

static RunResult run(VM *vm, int base);

/* ---------------------------------------------------------------- errors */

static void record_trace(VM *vm) {
    Buf b;
    buf_init(&b);
    int shown = 0;
    for (int i = vm->frame_count - 1; i >= 0 && shown < 12; i--, shown++) {
        CallFrame *f = &vm->frames[i];
        ObjFunction *fn = f->closure->fn;
        size_t off = f->ip > fn->chunk.code ? (size_t)(f->ip - fn->chunk.code - 1) : 0;
        int line = fn->chunk.count ? fn->chunk.lines[off < (size_t)fn->chunk.count ? off : (size_t)fn->chunk.count - 1] : 0;
        const char *file = fn->module && fn->module->path ? fn->module->path->chars : "<input>";
        buf_printf(&b, "  at %s:%d in %s\n", file, line, fn->name ? fn->name->chars : "?");
    }
    if (vm->frame_count > 12)
        buf_printf(&b, "  ... %d more\n", vm->frame_count - 12);
    size_t n = b.len < sizeof vm->trace - 1 ? b.len : sizeof vm->trace - 1;
    if (b.data)
        memcpy(vm->trace, b.data, n);
    vm->trace[n] = 0;
    buf_free(&b);
}

bool vm_throw_value(VM *vm, Value v) {
    vm->error = v;
    vm->has_error = true;
    if (!IS_OBJ(v) || AS_OBJ(v) != vm->traced) {
        record_trace(vm);
        vm->traced = IS_OBJ(v) ? AS_OBJ(v) : NULL;
    }
    return false;
}
bool vm_throw(VM *vm, const char *type, const char *fmt, ...) {
    char small[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    ObjString *t = str_intern_c(vm, type);
    push_root(vm, val_obj(t));
    ObjString *m = str_cstr(vm, small);
    push_root(vm, val_obj(m));
    ObjError *e = new_error(vm, t, m);
    pop_root(vm, 2);
    return vm_throw_value(vm, val_obj(e));
}

ObjString *error_type_name(VM *vm, Value err) {
    if (IS_OBJT(err, OBJ_ERROR))
        return ((ObjError *)AS_OBJ(err))->type;
    if (IS_OBJT(err, OBJ_INSTANCE))
        return AS_INSTANCE(err)->klass->name;
    Buf b;
    buf_init(&b);
    type_name_of(vm, err, &b);
    ObjString *s = str_copy(vm, b.data ? b.data : "", b.len);
    buf_free(&b);
    return s;
}
ObjString *error_message(VM *vm, Value err) {
    if (IS_OBJT(err, OBJ_ERROR))
        return ((ObjError *)AS_OBJ(err))->message;
    if (IS_OBJT(err, OBJ_INSTANCE)) {
        ObjInstance *in = AS_INSTANCE(err);
        Value idx;
        if (table_get(&in->klass->field_index, vm->s_message, &idx) && IS_STRING(in->fields[idx.as.i]))
            return AS_STRING(in->fields[idx.as.i]);
    }
    if (IS_STRING(err))
        return AS_STRING(err);
    ObjString *s = value_to_string(vm, err);
    if (!s) {
        vm->has_error = false;
        s = str_cstr(vm, "exception");
    }
    return s;
}
void vm_report_error(VM *vm, Value err) {
    fflush(stdout);
    ObjString *t = error_type_name(vm, err);
    push_root(vm, val_obj(t));
    ObjString *m = error_message(vm, err);
    fprintf(stderr, "Flywheel runtime error: %s: %s\n", t->chars, m->len ? m->chars : "exception");
    pop_root(vm, 1);
    if (vm->trace[0] && getenv("TURBINE_TRACE") && strcmp(getenv("TURBINE_TRACE"), "0") != 0)
        fputs(vm->trace, stderr);
    else if (vm->trace[0]) {
        /* the innermost location only */
        const char *nl = strchr(vm->trace, '\n');
        fprintf(stderr, "%.*s\n", nl ? (int)(nl - vm->trace) : (int)strlen(vm->trace), vm->trace);
    }
}

static bool error_matches(VM *vm, Value err, ObjString *name) {
    if (!strcmp(name->chars, "Error"))
        return true;
    if (IS_OBJT(err, OBJ_ERROR)) {
        ObjError *e = (ObjError *)AS_OBJ(err);
        return e->type == name || !strcmp(e->type->chars, name->chars);
    }
    if (IS_OBJT(err, OBJ_INSTANCE)) {
        for (ObjClass *k = AS_INSTANCE(err)->klass; k; k = k->super) {
            if (!strcmp(k->name->chars, name->chars))
                return true;
            for (int i = 0; i < k->ninterfaces; i++)
                if (!strcmp(k->interfaces[i]->name->chars, name->chars))
                    return true;
        }
        return false;
    }
    ObjString *t = error_type_name(vm, err);
    return !strcmp(t->chars, name->chars);
}

/* ---------------------------------------------------------------- classes */

bool vm_is_instance(ObjClass *klass, ObjClass *of) {
    for (ObjClass *k = klass; k; k = k->super) {
        if (k == of)
            return true;
        for (int i = 0; i < k->ninterfaces; i++)
            if (vm_is_instance(k->interfaces[i], of))
                return true;
    }
    return false;
}
Value vm_find_method(ObjClass *klass, ObjString *name) {
    Value v;
    if (table_get(&klass->methods, name, &v))
        return v;
    return val_null();
}

static void finalize_class(VM *vm, ObjClass *k) {
    if (k->finalized)
        return;
    k->finalized = true;
    ObjClass *s = k->super;
    if (s) {
        int own = k->nfields, total = s->nfields + own;
        ObjString **names = TB_ALLOC(vm, ObjString *, total ? total : 1);
        Value *types = TB_ALLOC(vm, Value, total ? total : 1);
        bool *has = TB_ALLOC(vm, bool, total ? total : 1);
        Value *consts = TB_ALLOC(vm, Value, total ? total : 1);
        for (int i = 0; i < s->nfields; i++) {
            names[i] = s->field_names[i];
            types[i] = s->field_types[i];
            has[i] = s->field_has_default[i];
            consts[i] = s->field_consts[i];
        }
        for (int i = 0; i < own; i++) {
            names[s->nfields + i] = k->field_names[i];
            types[s->nfields + i] = k->field_types[i];
            has[s->nfields + i] = k->field_has_default[i];
            consts[s->nfields + i] = k->field_consts[i];
        }
        TB_FREE(vm, ObjString *, k->field_names, own ? own : 1);
        TB_FREE(vm, Value, k->field_types, own ? own : 1);
        TB_FREE(vm, bool, k->field_has_default, own ? own : 1);
        TB_FREE(vm, Value, k->field_consts, own ? own : 1);
        k->field_names = names;
        k->field_types = types;
        k->field_has_default = has;
        k->field_consts = consts;
        k->nfields = total;
        k->own_start = s->nfields;
        for (int i = 0; i < s->methods.cap; i++) {
            Entry *e = &s->methods.entries[i];
            if (!e->key)
                continue;
            Value existing;
            if (!table_get(&k->methods, e->key, &existing))
                table_set(vm, &k->methods, e->key, e->value);
        }
    }
    for (int i = 0; i < k->nfields; i++)
        table_set(vm, &k->field_index, k->field_names[i], val_int(i));
#define M(field, nm) k->field = vm_find_method(k, str_intern_c(vm, nm))
    M(m_init, "init");
    M(m_to_string, "to_string");
    M(m_eq, "operator==");
    M(m_lt, "operator<");
    M(m_le, "operator<=");
    M(m_gt, "operator>");
    M(m_ge, "operator>=");
    M(m_hash, "hash");
    M(m_equals, "equals");
    M(m_next, "next");
    M(m_iterator, "iterator");
#undef M
    if (k->is_variant && s) {
        const char *dot = strrchr(k->name->chars, '.');
        ObjString *short_name = str_intern_c(vm, dot ? dot + 1 : k->name->chars);
        k->variant_index = s->statics.count;
        if (k->nfields == 0) {
            ObjInstance *one = new_instance(vm, k);
            k->singleton = val_obj(one);
            table_set(vm, &s->statics, short_name, k->singleton);
        } else
            table_set(vm, &s->statics, short_name, val_obj(k));
    }
}

static bool run_defaults(VM *vm, ObjClass *k, ObjInstance *inst) {
    if (k->super && !run_defaults(vm, k->super, inst))
        return false;
    if (IS_NULL(k->defaults))
        return true;
    Value out;
    Value recv = val_obj(inst);
    ObjClosure *cl = (ObjClosure *)AS_OBJ(k->defaults);
    (void)cl;
    return vm_call(vm, val_obj(new_bound(vm, recv, k->defaults)), 0, NULL, &out);
}
bool vm_run_defaults(VM *vm, ObjClass *k, ObjInstance *inst) { return run_defaults(vm, k, inst); }
static bool chain_has_defaults(ObjClass *k) {
    for (; k; k = k->super)
        if (!IS_NULL(k->defaults))
            return true;
    return false;
}
static bool check_required(VM *vm, ObjInstance *inst, bool from_init) {
    for (int i = 0; i < inst->nfields; i++)
        if (IS_UNDEF(inst->fields[i])) {
            if (from_init)
                return vm_throw(vm, "RuntimeError", "init must assign required field '%s' of %s",
                                inst->klass->field_names[i]->chars, inst->klass->name->chars);
            return vm_throw(vm, "TypeError", "%s requires field '%s'", inst->klass->name->chars,
                            inst->klass->field_names[i]->chars);
        }
    return true;
}
static bool set_field(VM *vm, ObjInstance *inst, int i, Value v) {
    Value t = inst->klass->field_types[i];
    if (IS_OBJ(t) && (AS_TYPE(t)->convert || IS_OBJT(v, OBJ_JSON))) {
        if (!vm_coerce(vm, v, AS_TYPE(t), &v, inst->klass->field_names[i]->chars))
            return false;
    }
    inst->fields[i] = v;
    return true;
}
bool vm_init_fields(VM *vm, ObjClass *k, ObjInstance *inst, int argc, Value *args) {
    if (argc > k->nfields)
        return vm_throw(vm, "TypeError", "%s takes at most %d argument%s, got %d", k->name->chars,
                        k->nfields, k->nfields == 1 ? "" : "s", argc);
    for (int i = 0; i < argc; i++)
        if (!set_field(vm, inst, i, args[i]))
            return false;
    return true;
}
/* Whether construction uses `init` (an inherited init applies only when no fields are added
 * that lack defaults). */
static Value applicable_init(ObjClass *k) {
    if (IS_NULL(k->m_init))
        return val_null();
    ObjClosure *cl = (ObjClosure *)AS_OBJ(k->m_init);
    if (cl->fn->owner != k) {
        for (int i = k->own_start; i < k->nfields; i++)
            if (!k->field_has_default[i])
                return val_null();
    }
    return k->m_init;
}
static ObjInstance *start_instance(VM *vm, ObjClass *k) {
    ObjInstance *inst = new_instance(vm, k);
    for (int i = 0; i < k->nfields; i++)
        if (!IS_UNDEF(k->field_consts[i])) {
            Value v = k->field_consts[i], t = k->field_types[i];
            if (IS_OBJ(t) && AS_TYPE(t)->kind == TY_WEAK && IS_NULL(v))
                v = val_obj(new_weak(vm, NULL));
            inst->fields[i] = v;
        }
    return inst;
}
static bool constructible(VM *vm, ObjClass *k) {
    if (k->is_interface)
        return vm_throw(vm, "TypeError", "interface %s cannot be constructed", k->name->chars);
    if (k->is_enum)
        return vm_throw(vm, "TypeError", "enum %s cannot be constructed; use one of its members", k->name->chars);
    if (!IS_NULL(k->singleton))
        return vm_throw(vm, "TypeError", "%s has no fields; use it as a value", k->name->chars);
    if (!k->finalized)
        return vm_throw(vm, "RuntimeError", "class %s used before its definition", k->name->chars);
    return true;
}
bool vm_construct(VM *vm, ObjClass *k, int argc, Value *args, Value *out) {
    if (!constructible(vm, k))
        return false;
    ObjInstance *inst = start_instance(vm, k);
    push_root(vm, val_obj(inst));
    Value init = applicable_init(k);
    bool ok = true;
    if (!IS_NULL(init)) {
        if (chain_has_defaults(k))
            ok = run_defaults(vm, k, inst);
        Value r;
        if (ok)
            ok = vm_call(vm, val_obj(new_bound(vm, val_obj(inst), init)), argc, args, &r);
        if (ok)
            ok = check_required(vm, inst, true);
    } else {
        ok = vm_init_fields(vm, k, inst, argc, args);
        if (ok && chain_has_defaults(k))
            ok = run_defaults(vm, k, inst);
        if (ok)
            ok = check_required(vm, inst, false);
    }
    pop_root(vm, 1);
    *out = val_obj(inst);
    return ok;
}

/* ---------------------------------------------------------------- types */

Value vm_resolve_named(VM *vm, ObjTypeDesc *t) {
    (void)vm;
    if (!t->module || t->slot < 0 || t->slot >= t->module->values.count)
        return val_null();
    Value v = t->module->values.items[t->slot];
    if (t->member && IS_OBJT(v, OBJ_CLASS)) {
        Value s;
        if (!table_get(&AS_CLASS(v)->statics, t->member, &s))
            return val_null();
        return s;
    }
    if (t->member)
        return val_null();
    return v;
}

static bool int_in_range(int64_t v, ObjTypeDesc *t) {
    if (t->is_unsigned) {
        if (v < 0)
            return false;
        if (t->bits >= 64)
            return true;
        return (uint64_t)v <= ((1ull << t->bits) - 1);
    }
    if (t->bits >= 64)
        return true;
    int64_t lim = (int64_t)1 << (t->bits - 1);
    return v >= -lim && v < lim;
}
static const char *tname(ObjTypeDesc *t) {
    switch (t->kind) {
    case TY_INT: return "int";
    case TY_DOUBLE: return "double";
    case TY_FLOAT: return "float";
    case TY_BOOL: return "bool";
    case TY_CHAR: return "char";
    case TY_STRING: return "string";
    case TY_ARRAY: return "array";
    case TY_TUPLE:
    case TY_HTUPLE: return "tuple";
    case TY_MAP: return "map";
    case TY_SET: return "set";
    case TY_RESULT: return "Result";
    case TY_ELEMENT: return "element";
    case TY_FRAGMENT: return "fragment";
    case TY_JSON: return "json.value";
    case TY_NAMED:
    case TY_ERROR: return t->name ? t->name->chars : "value";
    default: return "value";
    }
}
static bool mismatch(VM *vm, Value v, ObjTypeDesc *t, const char *what) {
    Buf b;
    buf_init(&b);
    type_name_of(vm, v, &b);
    bool r = vm_throw(vm, "TypeError", "cannot use %s as %s%s%s", b.data ? b.data : "value", tname(t),
                      what && what[0] ? " for " : "", what ? what : "");
    buf_free(&b);
    return r;
}

bool vm_type_test(VM *vm, Value v, ObjTypeDesc *t) {
    switch (t->kind) {
    case TY_ANY:
    case TY_FUNCTION:
        return !IS_NULL(v) || t->kind == TY_ANY;
    case TY_VOID:
        return IS_NULL(v);
    case TY_NULLABLE:
        return IS_NULL(v) || vm_type_test(vm, v, t->args[0]);
    case TY_BOOL: return IS_BOOL(v);
    case TY_INT: return IS_INT(v) && int_in_range(v.as.i, t);
    case TY_DOUBLE:
    case TY_FLOAT: return IS_DOUBLE(v);
    case TY_CHAR: return IS_CHAR(v);
    case TY_STRING: return IS_STRING(v);
    case TY_ARRAY: return IS_ARRAY(v);
    case TY_TUPLE:
    case TY_HTUPLE: return IS_TUPLE(v);
    case TY_MAP: return IS_MAP(v);
    case TY_SET: return IS_SET(v);
    case TY_RESULT: return IS_OBJT(v, OBJ_RESULT);
    case TY_ELEMENT:
    case TY_FRAGMENT: return IS_OBJT(v, OBJ_ELEMENT);
    case TY_JSON: return IS_OBJT(v, OBJ_JSON);
    case TY_WEAK: return IS_OBJT(v, OBJ_WEAK);
    case TY_ERROR:
        if (IS_NULL(v))
            return false;
        return error_matches(vm, v, t->name);
    case TY_NAMED: {
        Value k = vm_resolve_named(vm, t);
        if (IS_OBJT(k, OBJ_CLASS))
            return IS_OBJT(v, OBJ_INSTANCE) && vm_is_instance(AS_INSTANCE(v)->klass, AS_CLASS(k));
        if (IS_OBJT(k, OBJ_ENUM))
            return IS_OBJT(v, OBJ_ENUMVAL) && ((ObjEnumVal *)AS_OBJ(v))->e == (ObjEnum *)AS_OBJ(k);
        if (IS_OBJT(k, OBJ_INSTANCE)) /* field-less enum member */
            return IS_OBJ(v) && AS_OBJ(v) == AS_OBJ(k);
        if (IS_OBJT(v, OBJ_INSTANCE) && t->name)
            return error_matches(vm, v, t->name);
        return !IS_NULL(v);
    }
    }
    return false;
}

static bool coerce_list_items(VM *vm, ObjList *l, ObjTypeDesc *elem, const char *what) {
    for (int i = 0; i < l->count; i++)
        if (!vm_coerce(vm, l->items[i], elem, &l->items[i], what))
            return false;
    return true;
}
bool json_decode_into(VM *vm, Value v, ObjTypeDesc *t, Value *out, const char *path);
bool vm_coerce(VM *vm, Value v, ObjTypeDesc *t, Value *out, const char *what) {
    *out = v;
    /* an untyped json.decode result converts to the declared type */
    if (IS_OBJT(v, OBJ_JSON) && t->kind != TY_JSON && t->kind != TY_ANY && t->kind != TY_VOID &&
        t->kind != TY_FUNCTION && !(t->kind == TY_NULLABLE && t->args[0]->kind == TY_JSON)) {
        push_root(vm, v);
        bool ok = json_decode_into(vm, v, t, out, "");
        pop_root(vm, 1);
        return ok;
    }
    switch (t->kind) {
    case TY_ANY:
    case TY_VOID:
    case TY_FUNCTION:
        return true;
    case TY_NULLABLE:
        if (IS_NULL(v))
            return true;
        return vm_coerce(vm, v, t->args[0], out, what);
    default:
        break;
    }
    if (IS_NULL(v)) {
        if (t->kind == TY_NAMED && IS_NULL(vm_resolve_named(vm, t)))
            return true;
        if (t->kind == TY_WEAK) {
            *out = val_obj(new_weak(vm, NULL));
            return true;
        }
        return vm_throw(vm, "TypeError", "cannot assign null to non-nullable %s%s%s", tname(t),
                        what && what[0] ? " " : "", what ? what : "");
    }
    switch (t->kind) {
    case TY_INT:
        if (!IS_INT(v))
            return mismatch(vm, v, t, what);
        if (!int_in_range(v.as.i, t))
            return vm_throw(vm, "NumericError", "integer cast out of range: %lld does not fit %s%d", (long long)v.as.i,
                            t->is_unsigned ? "u" : "i", t->bits);
        return true;
    case TY_DOUBLE:
        if (IS_INT(v)) {
            *out = val_double((double)v.as.i);
            return true;
        }
        return IS_DOUBLE(v) ? true : mismatch(vm, v, t, what);
    case TY_FLOAT:
        if (IS_INT(v) || IS_DOUBLE(v)) {
            *out = val_double((double)(float)AS_NUM(v));
            return true;
        }
        return mismatch(vm, v, t, what);
    case TY_BOOL:
        return IS_BOOL(v) ? true : mismatch(vm, v, t, what);
    case TY_CHAR:
        return IS_CHAR(v) ? true : mismatch(vm, v, t, what);
    case TY_STRING:
        return IS_STRING(v) ? true : mismatch(vm, v, t, what);
    case TY_ARRAY: {
        if (IS_TUPLE(v)) {
            ObjList *src = AS_LIST(v);
            ObjList *l = new_list(vm, OBJ_ARRAY, src->count);
            for (int i = 0; i < src->count; i++)
                l->items[i] = src->items[i];
            l->count = src->count;
            *out = v = val_obj(l);
        }
        if (!IS_ARRAY(v))
            return mismatch(vm, v, t, what);
        if (t->nargs && t->args[0]->convert)
            return coerce_list_items(vm, AS_LIST(v), t->args[0], what);
        return true;
    }
    case TY_HTUPLE:
    case TY_TUPLE: {
        if (!IS_LIST(v))
            return mismatch(vm, v, t, what);
        ObjList *src = AS_LIST(v);
        if (t->kind == TY_TUPLE && src->count != t->nargs)
            return vm_throw(vm, "ValueError", "expected a tuple of %d items, got %d", t->nargs, src->count);
        if (IS_ARRAY(v)) {
            ObjList *l = new_list(vm, OBJ_TUPLE, src->count);
            for (int i = 0; i < src->count; i++)
                l->items[i] = src->items[i];
            l->count = src->count;
            *out = val_obj(l);
            src = l;
        }
        if (t->kind == TY_TUPLE) {
            for (int i = 0; i < src->count; i++)
                if (t->args[i]->convert && !vm_coerce(vm, src->items[i], t->args[i], &src->items[i], what))
                    return false;
        } else if (t->nargs && t->args[0]->convert)
            return coerce_list_items(vm, src, t->args[0], what);
        return true;
    }
    case TY_SET:
        if (IS_MAP(v) && AS_MAP(v)->live == 0) {
            *out = val_obj(new_map(vm, OBJ_SET));
            return true;
        }
        if (IS_ARRAY(v) && AS_LIST(v)->count == 0) {
            *out = val_obj(new_map(vm, OBJ_SET));
            return true;
        }
        return IS_SET(v) ? true : mismatch(vm, v, t, what);
    case TY_MAP: {
        if (!IS_MAP(v))
            return mismatch(vm, v, t, what);
        if (t->nargs == 2 && t->args[1]->convert) {
            ObjMap *m = AS_MAP(v);
            for (int i = 0; i < m->used; i++)
                if (m->entries[i].live &&
                    !vm_coerce(vm, m->entries[i].value, t->args[1], &m->entries[i].value, what))
                    return false;
        }
        return true;
    }
    case TY_RESULT:
        return IS_OBJT(v, OBJ_RESULT) ? true : mismatch(vm, v, t, what);
    case TY_ELEMENT:
    case TY_FRAGMENT:
        return IS_OBJT(v, OBJ_ELEMENT) ? true : mismatch(vm, v, t, what);
    case TY_JSON:
        if (IS_OBJT(v, OBJ_JSON))
            return true;
        *out = val_obj(new_json(vm, v));
        return true;
    case TY_WEAK:
        if (IS_OBJT(v, OBJ_WEAK))
            return true;
        if (IS_OBJ(v)) {
            *out = val_obj(new_weak(vm, AS_OBJ(v)));
            return true;
        }
        return mismatch(vm, v, t, what);
    case TY_ERROR:
        return true;
    case TY_NAMED: {
        Value k = vm_resolve_named(vm, t);
        if (IS_OBJT(k, OBJ_CLASS) || IS_OBJT(k, OBJ_ENUM)) {
            if (!vm_type_test(vm, v, t))
                return mismatch(vm, v, t, what);
        }
        return true;
    }
    default:
        return true;
    }
}

/* ---------------------------------------------------------------- calls */

static bool push_frame(VM *vm, ObjClosure *cl, int argc) {
    ObjFunction *fn = cl->fn;
    if (argc > fn->arity || argc < fn->required) {
        if (fn->required == fn->arity)
            return vm_throw(vm, "TypeError", "%s expects %d argument%s, got %d", fn->name->chars, fn->arity,
                            fn->arity == 1 ? "" : "s", argc);
        return vm_throw(vm, "TypeError", "%s expects %d to %d arguments, got %d", fn->name->chars,
                        fn->required, fn->arity, argc);
    }
    if (vm->frame_count == FRAMES_MAX || vm->sp + 256 + fn->arity >= vm->stack + STACK_MAX)
        return vm_throw(vm, "RuntimeError", "stack overflow (recursion too deep or unbounded)");
    for (int i = argc; i < fn->arity; i++)
        *vm->sp++ = val_null();
    CallFrame *f = &vm->frames[vm->frame_count++];
    f->closure = cl;
    f->ip = fn->chunk.code;
    f->slots = vm->sp - fn->arity - 1;
    f->argc = argc;
    f->ctor = false;
    return true;
}

static bool call_native(VM *vm, ObjNative *n, Value *args, int argc, Value *result_slot) {
    if (argc < n->min_args || (n->max_args >= 0 && argc > n->max_args)) {
        int shown = n->is_method ? 1 : 0;
        if (n->min_args == n->max_args)
            return vm_throw(vm, "TypeError", "%s expects %d argument%s, got %d", n->name, n->min_args - shown,
                            n->min_args - shown == 1 ? "" : "s", argc - shown);
        return vm_throw(vm, "TypeError", "%s got %d argument%s", n->name, argc - shown, argc - shown == 1 ? "" : "s");
    }
    Value out = val_null();
    vm->desc_args[0] = n->data;
    if (!n->fn(vm, argc, args, &out))
        return false;
    *result_slot = out;
    return true;
}

/* Calls the value at sp[-argc-1]. On success either a frame was pushed (*pushed) or the
 * result replaced the callee and arguments. */
static bool call_value(VM *vm, int argc, bool *pushed) {
    Value *slot = vm->sp - argc - 1;
    Value callee = *slot;
    *pushed = false;
    if (!IS_OBJ(callee))
        goto not_callable;
    switch (OBJ_TYPE(callee)) {
    case OBJ_CLOSURE:
        if (!push_frame(vm, (ObjClosure *)AS_OBJ(callee), argc))
            return false;
        *pushed = true;
        return true;
    case OBJ_NATIVE: {
        ObjNative *n = (ObjNative *)AS_OBJ(callee);
        if (!call_native(vm, n, slot + 1, argc, slot))
            return false;
        vm->sp = slot + 1;
        return true;
    }
    case OBJ_BOUND: {
        ObjBound *b = (ObjBound *)AS_OBJ(callee);
        Value m = b->method;
        *slot = b->receiver;
        if (IS_OBJT(m, OBJ_CLOSURE)) {
            if (!push_frame(vm, (ObjClosure *)AS_OBJ(m), argc))
                return false;
            *pushed = true;
            return true;
        }
        if (IS_OBJT(m, OBJ_NATIVE)) {
            ObjNative *n = (ObjNative *)AS_OBJ(m);
            if (n->is_method) {
                if (!call_native(vm, n, slot, argc + 1, slot))
                    return false;
            } else if (!call_native(vm, n, slot + 1, argc, slot))
                return false;
            vm->sp = slot + 1;
            return true;
        }
        *slot = m;
        return call_value(vm, argc, pushed);
    }
    case OBJ_CLASS: {
        ObjClass *k = AS_CLASS(callee);
        if (!constructible(vm, k))
            return false;
        Value init = applicable_init(k);
        if (!IS_NULL(init) && !chain_has_defaults(k)) {
            ObjInstance *inst = start_instance(vm, k);
            *slot = val_obj(inst);
            if (!push_frame(vm, (ObjClosure *)AS_OBJ(init), argc))
                return false;
            vm->frames[vm->frame_count - 1].ctor = true;
            *pushed = true;
            return true;
        }
        if (IS_NULL(init) && !chain_has_defaults(k)) {
            ObjInstance *inst = start_instance(vm, k);
            *slot = val_obj(inst);
            if (!vm_init_fields(vm, k, inst, argc, slot + 1) || !check_required(vm, inst, false))
                return false;
            vm->sp = slot + 1;
            return true;
        }
        Value out;
        if (!vm_construct(vm, k, argc, slot + 1, &out))
            return false;
        *slot = out;
        vm->sp = slot + 1;
        return true;
    }
    default:
        break;
    }
not_callable: {
    Buf b;
    buf_init(&b);
    type_name_of(vm, callee, &b);
    bool r = vm_throw(vm, "TypeError", "%s is not callable", b.data ? b.data : "value");
    buf_free(&b);
    return r;
}
}

bool vm_call(VM *vm, Value callee, int argc, Value *args, Value *out) {
    if (vm->native_depth > 3000)
        return vm_throw(vm, "RuntimeError", "stack overflow (recursion too deep or unbounded)");
    if (vm->sp + argc + 300 >= vm->stack + STACK_MAX)
        return vm_throw(vm, "RuntimeError", "stack overflow (recursion too deep or unbounded)");
    Value *base = vm->sp;
    *vm->sp++ = callee;
    for (int i = 0; i < argc; i++)
        *vm->sp++ = args[i];
    bool pushed;
    if (!call_value(vm, argc, &pushed)) {
        vm->sp = base;
        return false;
    }
    if (pushed) {
        vm->native_depth++;
        RunResult r = run(vm, vm->frame_count - 1);
        vm->native_depth--;
        if (r != RUN_OK) {
            vm->sp = base;
            return false;
        }
    }
    *out = vm->sp[-1];
    vm->sp = base;
    return true;
}

bool vm_invoke_method(VM *vm, Value receiver, ObjString *name, int argc, Value *args, Value *out,
                      bool *found) {
    *found = false;
    if (IS_OBJT(receiver, OBJ_INSTANCE)) {
        Value m = vm_find_method(AS_INSTANCE(receiver)->klass, name);
        if (IS_NULL(m))
            return true;
        *found = true;
        ObjBound *b = new_bound(vm, receiver, m);
        return vm_call(vm, val_obj(b), argc, args, out);
    }
    return true;
}

/* ---------------------------------------------------------------- members */

static bool enum_values(VM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    ObjEnum *e = (ObjEnum *)AS_OBJ(args[0]);
    ObjList *l = new_list(vm, OBJ_ARRAY, e->count);
    for (int i = 0; i < e->count; i++)
        l->items[i] = e->members[i];
    l->count = e->count;
    *out = val_obj(l);
    return true;
}
static bool enum_from_name(VM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    ObjEnum *e = (ObjEnum *)AS_OBJ(args[0]);
    if (!IS_STRING(args[1]))
        return vm_throw(vm, "TypeError", "from_name expects a string");
    *out = val_null();
    for (int i = 0; i < e->count; i++)
        if (!strcmp(((ObjEnumVal *)AS_OBJ(e->members[i]))->name->chars, AS_CSTR(args[1])))
            *out = e->members[i];
    return true;
}
static bool get_class_static(VM *vm, ObjClass *k, ObjString *name, Value *out) {
    for (ObjClass *c = k; c; c = c->super) {
        if (table_get(&c->statics, name, out))
            return true;
    }
    if (!strcmp(name->chars, "from_json")) {
        ObjNative *n = new_native(vm, "from_json", lib_class_from_json, 2, 2);
        n->is_method = true;
        *out = val_obj(new_bound(vm, val_obj(k), val_obj(n)));
        return true;
    }
    if (!strcmp(name->chars, "name")) {
        *out = val_obj(k->name);
        return true;
    }
    return false;
}

bool vm_get_member(VM *vm, Value obj, ObjString *name, Value *out) {
    if (IS_OBJ(obj)) {
        switch (OBJ_TYPE(obj)) {
        case OBJ_INSTANCE: {
            ObjInstance *in = AS_INSTANCE(obj);
            Value idx;
            if (table_get(&in->klass->field_index, name, &idx)) {
                Value v = in->fields[idx.as.i];
                if (IS_UNDEF(v))
                    return vm_throw(vm, "RuntimeError", "field '%s' of %s is read before it is assigned",
                                    name->chars, in->klass->name->chars);
                *out = v;
                return true;
            }
            Value m = vm_find_method(in->klass, name);
            if (!IS_NULL(m)) {
                *out = val_obj(new_bound(vm, obj, m));
                return true;
            }
            return vm_throw(vm, "TypeError", "%s has no member '%s'", in->klass->name->chars, name->chars);
        }
        case OBJ_CLASS:
            if (get_class_static(vm, AS_CLASS(obj), name, out))
                return true;
            return vm_throw(vm, "TypeError", "%s has no member '%s'", AS_CLASS(obj)->name->chars, name->chars);
        case OBJ_ENUM: {
            ObjEnum *e = (ObjEnum *)AS_OBJ(obj);
            for (int i = 0; i < e->count; i++)
                if (((ObjEnumVal *)AS_OBJ(e->members[i]))->name == name) {
                    *out = e->members[i];
                    return true;
                }
            if (!strcmp(name->chars, "values") || !strcmp(name->chars, "from_name")) {
                bool values = name->chars[0] == 'v';
                ObjNative *n = new_native(vm, name->chars, values ? enum_values : enum_from_name,
                                          values ? 1 : 2, values ? 1 : 2);
                n->is_method = true;
                *out = val_obj(new_bound(vm, obj, val_obj(n)));
                return true;
            }
            return vm_throw(vm, "TypeError", "enum %s has no member '%s'", e->name->chars, name->chars);
        }
        case OBJ_MODULE: {
            ObjModule *m = AS_MODULE(obj);
            int s = module_find(m, name);
            if (s < 0)
                return vm_throw(vm, "TypeError", "module '%s' has no member '%s'", m->name->chars, name->chars);
            Value v = m->values.items[s];
            if (IS_UNDEF(v))
                return vm_throw(vm, "RuntimeError", "'%s' is used before it is initialized", name->chars);
            *out = v;
            return true;
        }
        default:
            break;
        }
    }
    if (IS_NULL(obj))
        return vm_throw(vm, "CastError", "cannot cast null to a non-null type (reading '%s')", name->chars);
    bool found = false;
    if (!lib_builtin_property(vm, obj, name, out, &found))
        return false;
    if (found)
        return true;
    if (lib_has_builtin_method(vm, obj, name))
        return lib_bound_builtin(vm, obj, name, out);
    Buf b;
    buf_init(&b);
    type_name_of(vm, obj, &b);
    bool r = vm_throw(vm, "TypeError", "%s has no member '%s'", b.data ? b.data : "value", name->chars);
    buf_free(&b);
    return r;
}

/* ---------------------------------------------------------------- arithmetic */

#if defined(__GNUC__) || defined(__clang__)
#define ADD_OVF(a, b, r) __builtin_add_overflow(a, b, r)
#define SUB_OVF(a, b, r) __builtin_sub_overflow(a, b, r)
#define MUL_OVF(a, b, r) __builtin_mul_overflow(a, b, r)
#else
static bool ADD_OVF(int64_t a, int64_t b, int64_t *r) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
        return true;
    *r = a + b;
    return false;
}
static bool SUB_OVF(int64_t a, int64_t b, int64_t *r) {
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))
        return true;
    *r = a - b;
    return false;
}
static bool MUL_OVF(int64_t a, int64_t b, int64_t *r) {
    if (a == 0 || b == 0) {
        *r = 0;
        return false;
    }
    if ((a == -1 && b == INT64_MIN) || (b == -1 && a == INT64_MIN))
        return true;
    if (a > 0 ? (b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a) : (b > 0 ? a < INT64_MIN / b : a < INT64_MAX / b))
        return true;
    *r = a * b;
    return false;
}
#endif

static bool overflow(VM *vm, const char *op) {
    return vm_throw(vm, "NumericError", "integer overflow (%s)", op);
}
static bool type_error2(VM *vm, const char *op, Value a, Value b) {
    Buf x, y;
    buf_init(&x);
    buf_init(&y);
    type_name_of(vm, a, &x);
    type_name_of(vm, b, &y);
    bool r = vm_throw(vm, "TypeError", "unsupported operand types for %s: %s and %s", op, x.data ? x.data : "?",
                      y.data ? y.data : "?");
    buf_free(&x);
    buf_free(&y);
    return r;
}

static ObjString *concat(VM *vm, const char *a, size_t an, const char *b, size_t bn) {
    char *buf = xmalloc(an + bn + 1);
    memcpy(buf, a, an);
    memcpy(buf + an, b, bn);
    return str_take_buf(vm, buf, an + bn);
}
static ObjList *repeat_list(VM *vm, ObjList *l, int64_t n, ObjType type) {
    if (n <= 0 || l->count == 0)
        return new_list(vm, type, 0);
    ObjList *r = new_list(vm, type, (int)(n * l->count));
    for (int64_t k = 0; k < n; k++)
        for (int i = 0; i < l->count; i++)
            r->items[r->count++] = l->items[i];
    return r;
}

/* Operator methods on instances: calls `operator<op>` and stores the result. */
static bool instance_op(VM *vm, const char *op, Value a, Value b, int argc, Value *out, bool *found) {
    *found = false;
    if (!IS_OBJT(a, OBJ_INSTANCE))
        return true;
    char name[32];
    snprintf(name, sizeof name, "operator%s", op);
    Value m = vm_find_method(AS_INSTANCE(a)->klass, str_intern_c(vm, name));
    if (IS_NULL(m))
        return true;
    *found = true;
    Value args[1] = {b};
    return vm_call(vm, val_obj(new_bound(vm, a, m)), argc, args, out);
}

static bool arith(VM *vm, uint8_t op, Value a, Value b, Value *out) {
    if (IS_INT(a) && IS_INT(b)) {
        int64_t x = a.as.i, y = b.as.i, r;
        switch (op) {
        case OP_ADD:
            if (ADD_OVF(x, y, &r))
                return overflow(vm, "+");
            *out = val_int(r);
            return true;
        case OP_SUB:
            if (SUB_OVF(x, y, &r))
                return overflow(vm, "-");
            *out = val_int(r);
            return true;
        case OP_MUL:
            if (MUL_OVF(x, y, &r))
                return overflow(vm, "*");
            *out = val_int(r);
            return true;
        case OP_DIV:
            if (y == 0)
                return vm_throw(vm, "NumericError", "division by zero");
            *out = val_double((double)x / (double)y);
            return true;
        case OP_IDIV:
            if (y == 0)
                return vm_throw(vm, "NumericError", "integer division by zero");
            if (x == INT64_MIN && y == -1)
                return vm_throw(vm, "NumericError", "integer division overflow");
            *out = val_int(x / y);
            return true;
        case OP_MOD:
            if (y == 0)
                return vm_throw(vm, "NumericError", "integer division by zero");
            if (x == INT64_MIN && y == -1)
                return vm_throw(vm, "NumericError", "integer division overflow");
            *out = val_int(x % y);
            return true;
        }
    }
    if (IS_NUM(a) && IS_NUM(b)) {
        double x = AS_NUM(a), y = AS_NUM(b);
        switch (op) {
        case OP_ADD: *out = val_double(x + y); return true;
        case OP_SUB: *out = val_double(x - y); return true;
        case OP_MUL: *out = val_double(x * y); return true;
        case OP_DIV: *out = val_double(x / y); return true;
        case OP_IDIV: {
            if (y == 0)
                return vm_throw(vm, "NumericError", "division by zero");
            double q = trunc(x / y);
            if (!(q >= -9223372036854775808.0 && q < 9223372036854775808.0))
                return vm_throw(vm, "NumericError", "integer division result out of range");
            *out = val_int((int64_t)q);
            return true;
        }
        case OP_MOD: *out = val_double(fmod(x, y)); return true;
        }
    }
    static const char *names[] = {"+", "-", "*", "/", "//", "%"};
    const char *opname = names[op - OP_ADD];
    if (op == OP_ADD) {
        if (IS_STRING(a) && IS_STRING(b)) {
            *out = val_obj(concat(vm, AS_CSTR(a), (size_t)AS_STRING(a)->len, AS_CSTR(b), (size_t)AS_STRING(b)->len));
            return true;
        }
        if ((IS_STRING(a) && IS_CHAR(b)) || (IS_CHAR(a) && IS_STRING(b))) {
            Buf c;
            buf_init(&c);
            if (IS_CHAR(a)) {
                buf_utf8(&c, a.as.c);
                buf_add(&c, AS_CSTR(b), (size_t)AS_STRING(b)->len);
            } else {
                buf_add(&c, AS_CSTR(a), (size_t)AS_STRING(a)->len);
                buf_utf8(&c, b.as.c);
            }
            *out = val_obj(str_copy(vm, c.data, c.len));
            buf_free(&c);
            return true;
        }
        if (IS_CHAR(a) && IS_CHAR(b)) {
            Buf c;
            buf_init(&c);
            buf_utf8(&c, a.as.c);
            buf_utf8(&c, b.as.c);
            *out = val_obj(str_copy(vm, c.data, c.len));
            buf_free(&c);
            return true;
        }
        if (IS_LIST(a) && IS_LIST(b)) {
            ObjList *x = AS_LIST(a), *y = AS_LIST(b);
            ObjList *r = new_list(vm, (ObjType)OBJ_TYPE(a), x->count + y->count);
            for (int i = 0; i < x->count; i++)
                r->items[r->count++] = x->items[i];
            for (int i = 0; i < y->count; i++)
                r->items[r->count++] = y->items[i];
            *out = val_obj(r);
            return true;
        }
    }
    if (op == OP_MUL) {
        Value s = a, n = b;
        if (IS_INT(a) && !IS_INT(b)) {
            s = b;
            n = a;
        }
        if (IS_INT(n) && IS_STRING(s)) {
            if (n.as.i < 0)
                return vm_throw(vm, "NumericError", "negative repetition count");
            ObjString *str = AS_STRING(s);
            if (str->len == 0 || n.as.i == 0) {
                *out = val_obj(str_copy(vm, "", 0));
                return true;
            }
            if ((uint64_t)n.as.i > (uint64_t)0x7fffffff / (uint64_t)str->len)
                return vm_throw(vm, "NumericError", "string repetition size overflow");
            size_t total = (size_t)str->len * (size_t)n.as.i;
            char *buf = xmalloc(total + 1);
            for (int64_t k = 0; k < n.as.i; k++)
                memcpy(buf + (size_t)k * (size_t)str->len, str->chars, (size_t)str->len);
            *out = val_obj(str_take_buf(vm, buf, total));
            return true;
        }
        if (IS_INT(n) && IS_LIST(s)) {
            if (n.as.i < 0)
                return vm_throw(vm, "NumericError", "negative repetition count");
            if (AS_LIST(s)->count && (uint64_t)n.as.i > (uint64_t)0x7fffffff / (uint64_t)AS_LIST(s)->count)
                return vm_throw(vm, "NumericError", "collection repetition size overflow");
            *out = val_obj(repeat_list(vm, AS_LIST(s), n.as.i, (ObjType)OBJ_TYPE(s)));
            return true;
        }
        if (IS_INT(n) && IS_MAP(s))
            return vm_throw(vm, "TypeError", "maps cannot be repeated");
    }
    bool found;
    if (!instance_op(vm, opname, a, b, 1, out, &found))
        return false;
    if (found)
        return true;
    return type_error2(vm, opname, a, b);
}

static bool compare_op(VM *vm, uint8_t op, Value a, Value b, bool *out) {
    if (IS_INT(a) && IS_INT(b)) {
        int64_t x = a.as.i, y = b.as.i;
        switch (op) {
        case OP_LT: *out = x < y; return true;
        case OP_LE: *out = x <= y; return true;
        case OP_GT: *out = x > y; return true;
        case OP_GE: *out = x >= y; return true;
        }
    }
    if (op == OP_EQ || op == OP_NE) {
        bool eq;
        if (!values_equal(vm, a, b, &eq))
            return false;
        *out = op == OP_EQ ? eq : !eq;
        return true;
    }
    if (IS_OBJT(a, OBJ_INSTANCE)) {
        ObjClass *k = AS_INSTANCE(a)->klass;
        Value direct = op == OP_LT ? k->m_lt : op == OP_LE ? k->m_le : op == OP_GT ? k->m_gt : k->m_ge;
        Value r;
        if (!IS_NULL(direct)) {
            if (!vm_call(vm, val_obj(new_bound(vm, a, direct)), 1, &b, &r))
                return false;
            if (!IS_BOOL(r))
                return vm_throw(vm, "TypeError", "comparison operators must return bool");
            *out = r.as.b;
            return true;
        }
        if (!IS_NULL(k->m_lt)) {
            /* derive from operator <: a > b is b < a, a <= b is not (b < a), a >= b is not (a < b) */
            if (op == OP_GT || op == OP_LE) {
                if (!IS_OBJT(b, OBJ_INSTANCE) || IS_NULL(AS_INSTANCE(b)->klass->m_lt))
                    return type_error2(vm, ">", a, b);
                if (!vm_call(vm, val_obj(new_bound(vm, b, AS_INSTANCE(b)->klass->m_lt)), 1, &a, &r))
                    return false;
            } else if (!vm_call(vm, val_obj(new_bound(vm, a, k->m_lt)), 1, &b, &r))
                return false;
            if (!IS_BOOL(r))
                return vm_throw(vm, "TypeError", "comparison operators must return bool");
            *out = (op == OP_GT || op == OP_LT) ? r.as.b : !r.as.b;
            return true;
        }
    }
    int c;
    if (!value_compare(vm, a, b, &c))
        return false;
    switch (op) {
    case OP_LT: *out = c < 0; break;
    case OP_LE: *out = c <= 0; break;
    case OP_GT: *out = c > 0; break;
    default: *out = c >= 0; break;
    }
    return true;
}

static bool contains(VM *vm, Value container, Value item, bool *out) {
    *out = false;
    if (IS_LIST(container)) {
        ObjList *l = AS_LIST(container);
        for (int i = 0; i < l->count; i++) {
            bool eq;
            if (!values_equal(vm, l->items[i], item, &eq))
                return false;
            if (eq) {
                *out = true;
                return true;
            }
        }
        return true;
    }
    if (IS_MAP(container) || IS_SET(container))
        return map_has(vm, AS_MAP(container), item, out);
    if (IS_STRING(container)) {
        ObjString *s = AS_STRING(container);
        if (IS_STRING(item)) {
            *out = AS_STRING(item)->len == 0 || strstr(s->chars, AS_CSTR(item)) != NULL;
            if (memchr(s->chars, 0, (size_t)s->len)) {
                /* strings with NUL bytes: byte search */
                ObjString *t = AS_STRING(item);
                *out = false;
                for (int i = 0; i + t->len <= s->len; i++)
                    if (!memcmp(s->chars + i, t->chars, (size_t)t->len)) {
                        *out = true;
                        break;
                    }
            }
            return true;
        }
        if (IS_CHAR(item)) {
            Buf b;
            buf_init(&b);
            buf_utf8(&b, item.as.c);
            *out = strstr(s->chars, b.data) != NULL;
            buf_free(&b);
            return true;
        }
    }
    if (IS_OBJT(container, OBJ_JSON)) {
        Value inner = ((ObjJson *)AS_OBJ(container))->v;
        if (IS_STRING(item) && IS_MAP(inner))
            return map_has(vm, AS_MAP(inner), item, out);
        return contains(vm, inner, item, out);
    }
    if (IS_OBJT(container, OBJ_INSTANCE)) {
        bool found;
        Value r;
        if (!vm_invoke_method(vm, container, str_intern_c(vm, "contains"), 1, &item, &r, &found))
            return false;
        if (found && IS_BOOL(r)) {
            *out = r.as.b;
            return true;
        }
    }
    return type_error2(vm, "in", item, container);
}

/* ---------------------------------------------------------------- indexing */

static bool index_value(VM *vm, Value obj, Value key, Value *out) {
    if (IS_LIST(obj)) {
        ObjList *l = AS_LIST(obj);
        if (!IS_INT(key))
            return vm_throw(vm, "TypeError", "sequence indices must be integers");
        int64_t i = key.as.i;
        if (i < 0 || i >= l->count)
            return vm_throw(vm, "IndexError", "index out of bounds");
        *out = l->items[i];
        return true;
    }
    if (IS_MAP(obj)) {
        bool found;
        if (!map_get(vm, AS_MAP(obj), key, out, &found))
            return false;
        if (!found) {
            ObjString *k = value_to_string(vm, key);
            return vm_throw(vm, "KeyError", "key %s not found", k ? k->chars : "?");
        }
        return true;
    }
    if (IS_STRING(obj)) {
        if (!IS_INT(key))
            return vm_throw(vm, "TypeError", "string indices must be integers");
        ObjString *s = AS_STRING(obj);
        if (key.as.i < 0 || key.as.i >= s->clen)
            return vm_throw(vm, "IndexError", "string index out of bounds");
        *out = val_char(str_char_at(s, (int)key.as.i));
        return true;
    }
    if (IS_OBJT(obj, OBJ_JSON)) {
        Value inner = ((ObjJson *)AS_OBJ(obj))->v, r;
        if (IS_MAP(inner) && IS_STRING(key)) {
            bool found;
            if (!map_get(vm, AS_MAP(inner), key, &r, &found))
                return false;
            if (!found)
                return vm_throw(vm, "JsonError", "missing JSON field '%s'", AS_CSTR(key));
        } else if (IS_ARRAY(inner) && IS_INT(key)) {
            ObjList *l = AS_LIST(inner);
            if (key.as.i < 0 || key.as.i >= l->count)
                return vm_throw(vm, "JsonError", "JSON index %lld out of range", (long long)key.as.i);
            r = l->items[key.as.i];
        } else
            return vm_throw(vm, "JsonError", "cannot index this JSON value with that key");
        *out = val_obj(new_json(vm, r));
        return true;
    }
    if (IS_OBJT(obj, OBJ_ELEMENT) && IS_INT(key)) {
        ObjElement *e = AS_ELEMENT(obj);
        if (key.as.i < 0 || key.as.i >= e->children.count)
            return vm_throw(vm, "IndexError", "child index out of range");
        *out = e->children.items[key.as.i];
        return true;
    }
    bool found;
    if (!instance_op(vm, "[]", obj, key, 1, out, &found))
        return false;
    if (found)
        return true;
    return type_error2(vm, "[]", obj, key);
}
static bool set_index(VM *vm, Value obj, Value key, Value v) {
    if (IS_LIST(obj)) {
        ObjList *l = AS_LIST(obj);
        if (l->frozen)
            return vm_throw(vm, "ValueError", "cannot modify a frozen array");
        if (!IS_INT(key))
            return vm_throw(vm, "TypeError", "sequence indices must be integers");
        if (key.as.i < 0 || key.as.i >= l->count)
            return vm_throw(vm, "IndexError", "index out of bounds");
        l->items[key.as.i] = v;
        return true;
    }
    if (IS_MAP(obj)) {
        if (AS_MAP(obj)->frozen)
            return vm_throw(vm, "ValueError", "cannot modify a frozen map");
        return map_set(vm, AS_MAP(obj), key, v);
    }
    if (IS_OBJT(obj, OBJ_INSTANCE)) {
        Value m = vm_find_method(AS_INSTANCE(obj)->klass, str_intern_c(vm, "operator[]="));
        if (!IS_NULL(m)) {
            Value args[2] = {key, v}, r;
            return vm_call(vm, val_obj(new_bound(vm, obj, m)), 2, args, &r);
        }
    }
    if (IS_STRING(obj))
        return vm_throw(vm, "TypeError", "strings are immutable");
    return type_error2(vm, "[]=", obj, key);
}
static bool slice_value(VM *vm, Value obj, Value lo, Value hi, bool inclusive, Value *out) {
    if (!IS_INT(lo) || !IS_INT(hi))
        return vm_throw(vm, "TypeError", "slice bounds must be integers");
    int64_t n;
    if (IS_STRING(obj))
        n = AS_STRING(obj)->clen;
    else if (IS_LIST(obj))
        n = AS_LIST(obj)->count;
    else
        return type_error2(vm, "slice", obj, lo);
    int64_t a = lo.as.i, b = hi.as.i;
    if (a < 0)
        a += n;
    if (b < 0)
        b += n;
    if (inclusive)
        b += 1;
    if (a < 0)
        a = 0;
    if (b > n)
        b = n;
    if (b < a)
        b = a;
    if (a > n)
        a = b = n;
    if (IS_STRING(obj)) {
        ObjString *s = AS_STRING(obj);
        int ba = utf8_offset(s, (int)a), bb = utf8_offset(s, (int)b);
        *out = val_obj(str_copy(vm, s->chars + ba, (size_t)(bb - ba)));
        return true;
    }
    ObjList *l = AS_LIST(obj);
    ObjList *r = new_list(vm, (ObjType)OBJ_TYPE(obj), (int)(b - a));
    for (int64_t i = a; i < b; i++)
        r->items[r->count++] = l->items[i];
    *out = val_obj(r);
    return true;
}

/* ---------------------------------------------------------------- iteration helpers */

static bool make_range(VM *vm, Value lo, Value hi, bool inclusive, Value *out) {
    if (!IS_INT(lo) || !IS_INT(hi))
        return vm_throw(vm, "TypeError", "range endpoints must be integers");
    int64_t a = lo.as.i, b = hi.as.i;
    uint64_t count = 0;
    if (inclusive ? a <= b : a < b) {
        count = (uint64_t)b - (uint64_t)a;
        if (inclusive) {
            if (count == UINT64_MAX)
                return vm_throw(vm, "NumericError", "range size overflow");
            count++;
        }
    }
    if (count > 0x7fffffffu)
        return vm_throw(vm, "NumericError", "range size overflow");
    ObjList *l = new_list(vm, OBJ_TUPLE, (int)count);
    for (uint64_t i = 0; i < count; i++)
        l->items[i] = val_int(a + (int64_t)i);
    l->count = (int)count;
    *out = val_obj(l);
    return true;
}

/* Literal arrays are tuples when their items have different kinds. */
static int value_kind(Value v) {
    if (IS_OBJ(v)) {
        ObjType t = (ObjType)OBJ_TYPE(v);
        if (t == OBJ_CLOSURE || t == OBJ_NATIVE || t == OBJ_BOUND)
            return 100 + OBJ_CLOSURE;
        if (t == OBJ_ARRAY || t == OBJ_TUPLE)
            return 100 + OBJ_ARRAY;
        return 100 + t;
    }
    return v.type;
}

/* ---------------------------------------------------------------- the dispatch loop */

#define READ_BYTE() (*ip++)
#define READ_U16() (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))
#define CONST(i) (frame->closure->fn->chunk.constants.items[i])
#define PUSH(v) (*vm->sp++ = (v))
#define POP() (*--vm->sp)
#define PEEK(n) (vm->sp[-1 - (n)])
#define SYNC() (frame->ip = ip)
#define RELOAD()                                                                                  \
    do {                                                                                          \
        frame = &vm->frames[vm->frame_count - 1];                                                 \
        ip = frame->ip;                                                                           \
    } while (0)
#define THROW() goto on_error
#define CHECK(x)                                                                                  \
    do {                                                                                          \
        if (TB_UNLIKELY(!(x)))                                                                    \
            THROW();                                                                              \
    } while (0)

static ObjUpvalue *capture(VM *vm, Value *slot) {
    ObjUpvalue *prev = NULL, *u = vm->open_upvalues;
    while (u && u->location > slot) {
        prev = u;
        u = u->next;
    }
    if (u && u->location == slot)
        return u;
    ObjUpvalue *n = new_upvalue(vm, slot);
    n->next = u;
    if (prev)
        prev->next = n;
    else
        vm->open_upvalues = n;
    return n;
}
static void close_upvalues(VM *vm, Value *last) {
    while (vm->open_upvalues && vm->open_upvalues->location >= last) {
        ObjUpvalue *u = vm->open_upvalues;
        u->closed = *u->location;
        u->location = &u->closed;
        vm->open_upvalues = u->next;
    }
}

static bool for_prep(VM *vm, Value *slots) {
    Value seq = slots[0];
    slots[1] = val_int(0);
    if (IS_LIST(seq)) {
        slots[2] = val_int(AS_LIST(seq)->count);
        return true;
    }
    if (IS_STRING(seq)) {
        slots[2] = val_int(0);
        return true;
    }
    if (IS_MAP(seq)) {
        slots[2] = seq;
        slots[0] = val_obj(map_keys(vm, AS_MAP(seq)));
        return true;
    }
    if (IS_SET(seq)) {
        slots[0] = val_obj(map_keys(vm, AS_MAP(seq)));
        slots[2] = val_int(AS_LIST(slots[0])->count);
        return true;
    }
    if (IS_OBJT(seq, OBJ_JSON)) {
        Value inner = ((ObjJson *)AS_OBJ(seq))->v;
        if (IS_ARRAY(inner)) {
            ObjList *src = AS_LIST(inner);
            ObjList *l = new_list(vm, OBJ_ARRAY, src->count);
            slots[0] = val_obj(l);
            for (int i = 0; i < src->count; i++)
                l->items[l->count++] = val_obj(new_json(vm, src->items[i]));
            slots[2] = val_int(l->count);
            return true;
        }
        if (IS_MAP(inner)) {
            slots[0] = val_obj(map_keys(vm, AS_MAP(inner)));
            slots[2] = val_int(AS_LIST(slots[0])->count);
            return true;
        }
        return vm_throw(vm, "JsonError", "only JSON arrays and objects can be iterated");
    }
    if (IS_OBJT(seq, OBJ_ELEMENT)) {
        ObjElement *e = AS_ELEMENT(seq);
        ObjList *l = new_list(vm, OBJ_ARRAY, e->children.count);
        for (int i = 0; i < e->children.count; i++)
            l->items[l->count++] = e->children.items[i];
        slots[0] = val_obj(l);
        slots[2] = val_int(l->count);
        return true;
    }
    if (IS_OBJT(seq, OBJ_INSTANCE)) {
        ObjClass *k = AS_INSTANCE(seq)->klass;
        if (IS_NULL(k->m_next) && !IS_NULL(k->m_iterator)) {
            Value it;
            if (!vm_call(vm, val_obj(new_bound(vm, seq, k->m_iterator)), 0, NULL, &it))
                return false;
            slots[0] = it;
            if (!IS_OBJT(it, OBJ_INSTANCE) || IS_NULL(AS_INSTANCE(it)->klass->m_next))
                return vm_throw(vm, "TypeError", "iterator() must return an object with next()");
        } else if (IS_NULL(k->m_next))
            return vm_throw(vm, "TypeError", "%s is not iterable (define next() or iterator())", k->name->chars);
        slots[2] = val_bool(true);
        return true;
    }
    Buf b;
    buf_init(&b);
    type_name_of(vm, seq, &b);
    bool r = vm_throw(vm, "TypeError", "%s is not iterable", b.data ? b.data : "value");
    buf_free(&b);
    return r;
}
/* Pushes the next one or two loop values; *done at the end. */
static bool for_next(VM *vm, Value *slots, int names, bool *done) {
    Value seq = slots[0];
    *done = false;
    if (IS_BOOL(slots[2])) {
        ObjInstance *it = AS_INSTANCE(seq);
        Value v;
        if (!vm_call(vm, val_obj(new_bound(vm, seq, it->klass->m_next)), 0, NULL, &v))
            return false;
        if (IS_NULL(v)) {
            *done = true;
            return true;
        }
        if (names == 2)
            PUSH(slots[1]);
        slots[1].as.i++;
        PUSH(v);
        return true;
    }
    if (IS_STRING(seq)) {
        ObjString *s = AS_STRING(seq);
        int64_t off = slots[1].as.i;
        if (off >= s->len) {
            *done = true;
            return true;
        }
        int adv;
        uint32_t c = utf8_decode(s->chars + off, (int)(s->len - off), &adv);
        if (names == 2)
            PUSH(slots[2]);
        slots[2].as.i++;
        slots[1].as.i += adv;
        PUSH(val_char(c));
        return true;
    }
    ObjList *l = AS_LIST(seq);
    int64_t i = slots[1].as.i;
    bool map_pairs = IS_MAP(slots[2]);
    int64_t limit = map_pairs ? l->count : slots[2].as.i;
    if (i >= limit) {
        *done = true;
        return true;
    }
    if (i >= l->count)
        return vm_throw(vm, "IndexError", "array changed size during iteration");
    slots[1].as.i++;
    if (map_pairs) {
        Value key = l->items[i];
        PUSH(key);
        if (names == 2) {
            Value v;
            bool found;
            if (!map_get(vm, AS_MAP(slots[2]), key, &v, &found))
                return false;
            if (!found)
                v = val_null();
            PUSH(v);
        }
        return true;
    }
    if (names == 2)
        PUSH(val_int(i));
    PUSH(l->items[i]);
    return true;
}

static bool match_value(VM *vm, Value subject, Value pattern, bool *out) {
    if (IS_OBJT(pattern, OBJ_CLASS) && !IS_OBJT(subject, OBJ_CLASS)) {
        *out = IS_OBJT(subject, OBJ_INSTANCE) && vm_is_instance(AS_INSTANCE(subject)->klass, AS_CLASS(pattern));
        return true;
    }
    return values_equal(vm, subject, pattern, out);
}

static bool get_attr(VM *vm, Value attrs, ObjString *name, Value *out) {
    bool found = false;
    if (IS_MAP(attrs) && !map_get(vm, AS_MAP(attrs), val_obj(name), out, &found))
        return false;
    if (found)
        return true;
    if (!strcmp(name->chars, "style")) {
        *out = val_obj(str_intern_c(vm, ""));
        return true;
    }
    if (!strcmp(name->chars, "inner")) {
        *out = val_obj(new_element(vm, NULL));
        return true;
    }
    return vm_throw(vm, "ValueError", "missing component attribute '%s'", name->chars);
}

static RunResult run(VM *vm, int base) {
    CallFrame *frame = &vm->frames[vm->frame_count - 1];
    uint8_t *ip = frame->ip;

#if (defined(__GNUC__) || defined(__clang__)) && !defined(TURBINE_SWITCH_DISPATCH)
    static void *labels[] = {
#define X(name) &&L_##name,
        OPCODES(X)
#undef X
    };
#define DISPATCH() goto *labels[*ip++]
#define CASE(name) L_##name:
#define SWITCH_BEGIN DISPATCH();
#define SWITCH_END
#else
#define DISPATCH() goto dispatch
#define CASE(name) case name:
#define SWITCH_BEGIN                                                                              \
    dispatch:                                                                                     \
    switch (*ip++) {
#define SWITCH_END                                                                                \
    default:                                                                                      \
        vm_throw(vm, "RuntimeError", "invalid bytecode");                                         \
        goto on_error;                                                                            \
        }
#endif

    SWITCH_BEGIN
    CASE(OP_CONST) {
        uint16_t k = READ_U16();
        PUSH(CONST(k));
        DISPATCH();
    }
    CASE(OP_NULL) {
        PUSH(val_null());
        DISPATCH();
    }
    CASE(OP_TRUE) {
        PUSH(val_bool(true));
        DISPATCH();
    }
    CASE(OP_FALSE) {
        PUSH(val_bool(false));
        DISPATCH();
    }
    CASE(OP_INT) {
        int16_t v = (int16_t)READ_U16();
        PUSH(val_int(v));
        DISPATCH();
    }
    CASE(OP_POP) {
        vm->sp--;
        DISPATCH();
    }
    CASE(OP_DUP) {
        Value v = PEEK(0);
        PUSH(v);
        DISPATCH();
    }
    CASE(OP_DUP2) {
        Value a = PEEK(1), b = PEEK(0);
        PUSH(a);
        PUSH(b);
        DISPATCH();
    }
    CASE(OP_SWAP) {
        Value a = PEEK(0);
        vm->sp[-1] = vm->sp[-2];
        vm->sp[-2] = a;
        DISPATCH();
    }
    CASE(OP_GET_LOCAL) {
        uint8_t s = READ_BYTE();
        PUSH(frame->slots[s]);
        DISPATCH();
    }
    CASE(OP_SET_LOCAL) {
        uint8_t s = READ_BYTE();
        frame->slots[s] = POP();
        DISPATCH();
    }
    CASE(OP_GET_UPVAL) {
        uint8_t s = READ_BYTE();
        PUSH(*frame->closure->upvalues[s]->location);
        DISPATCH();
    }
    CASE(OP_SET_UPVAL) {
        uint8_t s = READ_BYTE();
        *frame->closure->upvalues[s]->location = POP();
        DISPATCH();
    }
    CASE(OP_GET_GLOBAL) {
        uint16_t s = READ_U16();
        Value v = frame->closure->fn->module->values.items[s];
        if (TB_UNLIKELY(IS_UNDEF(v))) {
            SYNC();
            ObjModule *m = frame->closure->fn->module;
            vm_throw(vm, "RuntimeError", "'%s' is used before it is initialized",
                     AS_CSTR(m->names_by_slot.items[s]));
            THROW();
        }
        PUSH(v);
        DISPATCH();
    }
    CASE(OP_SET_GLOBAL) {
        uint16_t s = READ_U16();
        frame->closure->fn->module->values.items[s] = POP();
        DISPATCH();
    }
    CASE(OP_GET_MGLOBAL) {
        uint16_t mk = READ_U16(), s = READ_U16();
        ObjModule *m = AS_MODULE(CONST(mk));
        Value v = m->values.items[s];
        if (TB_UNLIKELY(IS_UNDEF(v))) {
            SYNC();
            vm_throw(vm, "RuntimeError", "'%s' is used before it is initialized",
                     AS_CSTR(m->names_by_slot.items[s]));
            THROW();
        }
        PUSH(v);
        DISPATCH();
    }
    CASE(OP_SET_MGLOBAL) {
        uint16_t mk = READ_U16(), s = READ_U16();
        AS_MODULE(CONST(mk))->values.items[s] = POP();
        DISPATCH();
    }
    CASE(OP_GET_MEMBER) {
        uint16_t nk = READ_U16(), ck = READ_U16();
        Value obj = PEEK(0);
        if (IS_OBJT(obj, OBJ_INSTANCE)) {
            ObjInstance *in = AS_INSTANCE(obj);
            InlineCache *c = &frame->closure->fn->caches[ck];
            if (c->klass == in->klass && c->index >= 0) {
                Value v = in->fields[c->index];
                if (!IS_UNDEF(v)) {
                    vm->sp[-1] = v;
                    DISPATCH();
                }
            }
            Value idx;
            if (table_get(&in->klass->field_index, AS_STRING(CONST(nk)), &idx)) {
                c->klass = in->klass;
                c->index = (int)idx.as.i;
            }
        }
        SYNC();
        Value out;
        CHECK(vm_get_member(vm, obj, AS_STRING(CONST(nk)), &out));
        vm->sp[-1] = out;
        DISPATCH();
    }
    CASE(OP_SET_MEMBER) {
        uint16_t nk = READ_U16(), ck = READ_U16();
        Value obj = PEEK(1), v = PEEK(0);
        SYNC();
        if (IS_OBJT(obj, OBJ_INSTANCE)) {
            ObjInstance *in = AS_INSTANCE(obj);
            InlineCache *c = &frame->closure->fn->caches[ck];
            int index = -1;
            if (c->klass == in->klass)
                index = c->index;
            else {
                Value idx;
                if (table_get(&in->klass->field_index, AS_STRING(CONST(nk)), &idx)) {
                    index = (int)idx.as.i;
                    c->klass = in->klass;
                    c->index = index;
                }
            }
            if (index < 0) {
                vm_throw(vm, "TypeError", "%s has no field '%s'", in->klass->name->chars, AS_CSTR(CONST(nk)));
                THROW();
            }
            CHECK(set_field(vm, in, index, v));
            vm->sp -= 2;
            DISPATCH();
        }
        if (IS_OBJT(obj, OBJ_MODULE)) {
            ObjModule *m = AS_MODULE(obj);
            int s = module_find(m, AS_STRING(CONST(nk)));
            if (s < 0 || m->native) {
                vm_throw(vm, "TypeError", "cannot assign to %s.%s", m->name->chars, AS_CSTR(CONST(nk)));
                THROW();
            }
            m->values.items[s] = v;
            vm->sp -= 2;
            DISPATCH();
        }
        {
            bool found = false;
            CHECK(lib_set_property(vm, obj, AS_STRING(CONST(nk)), v, &found));
            if (!found) {
                Buf b;
                buf_init(&b);
                type_name_of(vm, obj, &b);
                vm_throw(vm, "TypeError", "cannot assign to member '%s' of %s", AS_CSTR(CONST(nk)), b.data ? b.data : "value");
                buf_free(&b);
                THROW();
            }
        }
        vm->sp -= 2;
        DISPATCH();
    }
    CASE(OP_GET_INDEX) {
        Value key = PEEK(0), obj = PEEK(1);
        if (IS_ARRAY(obj) && IS_INT(key)) {
            ObjList *l = AS_LIST(obj);
            if (key.as.i >= 0 && key.as.i < l->count) {
                vm->sp--;
                vm->sp[-1] = l->items[key.as.i];
                DISPATCH();
            }
        }
        SYNC();
        Value out;
        CHECK(index_value(vm, obj, key, &out));
        vm->sp--;
        vm->sp[-1] = out;
        DISPATCH();
    }
    CASE(OP_SLICE) {
        uint8_t incl = READ_BYTE();
        SYNC();
        Value out;
        CHECK(slice_value(vm, PEEK(2), PEEK(1), PEEK(0), incl, &out));
        vm->sp -= 2;
        vm->sp[-1] = out;
        DISPATCH();
    }
    CASE(OP_FIELD_UNSET) {
        uint16_t nk = READ_U16();
        Value obj = PEEK(0);
        bool unset = false;
        if (IS_OBJT(obj, OBJ_INSTANCE)) {
            Value idx;
            if (table_get(&AS_INSTANCE(obj)->klass->field_index, AS_STRING(CONST(nk)), &idx))
                unset = IS_UNDEF(AS_INSTANCE(obj)->fields[idx.as.i]);
        }
        vm->sp[-1] = val_bool(unset);
        DISPATCH();
    }
    CASE(OP_SET_INDEX) {
        Value v = PEEK(0), key = PEEK(1), obj = PEEK(2);
        SYNC();
        if (IS_ARRAY(obj) && IS_INT(key) && !AS_LIST(obj)->frozen && key.as.i >= 0 &&
            key.as.i < AS_LIST(obj)->count) {
            AS_LIST(obj)->items[key.as.i] = v;
        } else
            CHECK(set_index(vm, obj, key, v));
        vm->sp -= 3;
        DISPATCH();
    }
    CASE(OP_ADD) {
        Value b = PEEK(0), a = PEEK(1);
        if (IS_INT(a) && IS_INT(b)) {
            int64_t r;
            if (!ADD_OVF(a.as.i, b.as.i, &r)) {
                vm->sp--;
                vm->sp[-1] = val_int(r);
                DISPATCH();
            }
        } else if (IS_DOUBLE(a) && IS_DOUBLE(b)) {
            vm->sp--;
            vm->sp[-1] = val_double(a.as.d + b.as.d);
            DISPATCH();
        }
        goto arith_slow;
    }
    CASE(OP_SUB) {
        Value b = PEEK(0), a = PEEK(1);
        if (IS_INT(a) && IS_INT(b)) {
            int64_t r;
            if (!SUB_OVF(a.as.i, b.as.i, &r)) {
                vm->sp--;
                vm->sp[-1] = val_int(r);
                DISPATCH();
            }
        } else if (IS_DOUBLE(a) && IS_DOUBLE(b)) {
            vm->sp--;
            vm->sp[-1] = val_double(a.as.d - b.as.d);
            DISPATCH();
        }
        goto arith_slow;
    }
    CASE(OP_MUL) {
        Value b = PEEK(0), a = PEEK(1);
        if (IS_INT(a) && IS_INT(b)) {
            int64_t r;
            if (!MUL_OVF(a.as.i, b.as.i, &r)) {
                vm->sp--;
                vm->sp[-1] = val_int(r);
                DISPATCH();
            }
        } else if (IS_DOUBLE(a) && IS_DOUBLE(b)) {
            vm->sp--;
            vm->sp[-1] = val_double(a.as.d * b.as.d);
            DISPATCH();
        }
        goto arith_slow;
    }
    CASE(OP_DIV)
    CASE(OP_IDIV)
    CASE(OP_MOD) {
    arith_slow : {
        uint8_t op = ip[-1];
        SYNC();
        Value out;
        CHECK(arith(vm, op, PEEK(1), PEEK(0), &out));
        vm->sp--;
        vm->sp[-1] = out;
        DISPATCH();
    }
    }
    CASE(OP_NEG) {
        Value a = PEEK(0);
        if (IS_INT(a)) {
            if (a.as.i == INT64_MIN) {
                SYNC();
                overflow(vm, "-");
                THROW();
            }
            vm->sp[-1] = val_int(-a.as.i);
        } else if (IS_DOUBLE(a))
            vm->sp[-1] = val_double(-a.as.d);
        else {
            SYNC();
            bool found;
            Value out;
            CHECK(instance_op(vm, "-()", a, val_null(), 0, &out, &found));
            if (!found) {
                type_error2(vm, "unary -", a, a);
                THROW();
            }
            vm->sp[-1] = out;
        }
        DISPATCH();
    }
    CASE(OP_POS) {
        if (!IS_NUM(PEEK(0))) {
            SYNC();
            type_error2(vm, "unary +", PEEK(0), PEEK(0));
            THROW();
        }
        DISPATCH();
    }
    CASE(OP_NOT) {
        Value a = PEEK(0);
        if (!IS_BOOL(a)) {
            SYNC();
            Buf b;
            buf_init(&b);
            type_name_of(vm, a, &b);
            vm_throw(vm, "TypeError", "'not' needs a bool, got %s", b.data ? b.data : "?");
            buf_free(&b);
            THROW();
        }
        vm->sp[-1] = val_bool(!a.as.b);
        DISPATCH();
    }
    CASE(OP_EQ)
    CASE(OP_NE)
    CASE(OP_LT)
    CASE(OP_LE)
    CASE(OP_GT)
    CASE(OP_GE) {
        uint8_t op = ip[-1];
        Value b = PEEK(0), a = PEEK(1);
        bool r;
        if (IS_INT(a) && IS_INT(b)) {
            int64_t x = a.as.i, y = b.as.i;
            switch (op) {
            case OP_EQ: r = x == y; break;
            case OP_NE: r = x != y; break;
            case OP_LT: r = x < y; break;
            case OP_LE: r = x <= y; break;
            case OP_GT: r = x > y; break;
            default: r = x >= y; break;
            }
        } else {
            SYNC();
            CHECK(compare_op(vm, op, a, b, &r));
        }
        vm->sp--;
        vm->sp[-1] = val_bool(r);
        DISPATCH();
    }
    CASE(OP_IN) {
        SYNC();
        bool r;
        CHECK(contains(vm, PEEK(0), PEEK(1), &r));
        vm->sp--;
        vm->sp[-1] = val_bool(r);
        DISPATCH();
    }
    CASE(OP_CHAIN) {
        uint8_t op = READ_BYTE();
        SYNC();
        bool r;
        CHECK(compare_op(vm, op, PEEK(1), PEEK(0), &r));
        vm->sp[-2] = vm->sp[-1];
        vm->sp[-1] = val_bool(r);
        DISPATCH();
    }
    CASE(OP_RANGE) {
        uint8_t incl = READ_BYTE();
        SYNC();
        Value out;
        CHECK(make_range(vm, PEEK(1), PEEK(0), incl, &out));
        vm->sp--;
        vm->sp[-1] = out;
        DISPATCH();
    }
    CASE(OP_IS) {
        uint16_t tk = READ_U16();
        SYNC();
        vm->sp[-1] = val_bool(vm_type_test(vm, PEEK(0), AS_TYPE(CONST(tk))));
        DISPATCH();
    }
    CASE(OP_AS) {
        uint16_t tk = READ_U16();
        SYNC();
        Value out;
        CHECK(vm_cast(vm, AS_TYPE(CONST(tk)), PEEK(0), &out));
        vm->sp[-1] = out;
        DISPATCH();
    }
    CASE(OP_COERCE) {
        uint16_t tk = READ_U16(), wk = READ_U16();
        SYNC();
        Value out;
        CHECK(vm_coerce(vm, PEEK(0), AS_TYPE(CONST(tk)), &out, AS_CSTR(CONST(wk))));
        vm->sp[-1] = out;
        DISPATCH();
    }
    CASE(OP_COERCE_LOCAL) {
        uint8_t s = READ_BYTE();
        uint16_t tk = READ_U16(), wk = READ_U16();
        SYNC();
        Value out;
        CHECK(vm_coerce(vm, frame->slots[s], AS_TYPE(CONST(tk)), &out, AS_CSTR(CONST(wk))));
        frame->slots[s] = out;
        DISPATCH();
    }
    CASE(OP_JUMP) {
        uint16_t off = READ_U16();
        ip += off;
        DISPATCH();
    }
    CASE(OP_JUMP_IF_FALSE) {
        uint16_t off = READ_U16();
        Value c = POP();
        if (TB_UNLIKELY(!IS_BOOL(c))) {
            SYNC();
            Buf b;
            buf_init(&b);
            type_name_of(vm, c, &b);
            vm_throw(vm, "TypeError", "conditions must be bool, got %s", b.data ? b.data : "?");
            buf_free(&b);
            THROW();
        }
        if (!c.as.b)
            ip += off;
        DISPATCH();
    }
    CASE(OP_AND) {
        uint16_t off = READ_U16();
        Value c = PEEK(0);
        if (TB_UNLIKELY(!IS_BOOL(c))) {
            SYNC();
            vm_throw(vm, "TypeError", "'and' needs bool operands");
            THROW();
        }
        if (!c.as.b)
            ip += off;
        else
            vm->sp--;
        DISPATCH();
    }
    CASE(OP_OR) {
        uint16_t off = READ_U16();
        Value c = PEEK(0);
        if (TB_UNLIKELY(!IS_BOOL(c))) {
            SYNC();
            vm_throw(vm, "TypeError", "'or' needs bool operands");
            THROW();
        }
        if (c.as.b)
            ip += off;
        else
            vm->sp--;
        DISPATCH();
    }
    CASE(OP_JUMP_NOT_NULL) {
        uint16_t off = READ_U16();
        if (!IS_NULL(PEEK(0)))
            ip += off;
        else
            vm->sp--;
        DISPATCH();
    }
    CASE(OP_LOOP) {
        uint16_t off = READ_U16();
        ip -= off;
        if (TB_UNLIKELY(vm->bytes_allocated > vm->next_gc) && !vm->gc_off) {
            SYNC();
            gc_collect(vm);
        }
        DISPATCH();
    }
    CASE(OP_CALL) {
        uint8_t argc = READ_BYTE();
        SYNC();
        vm_maybe_gc(vm);
        Value callee = PEEK(argc);
        if (IS_OBJT(callee, OBJ_CLOSURE)) {
            ObjClosure *cl = (ObjClosure *)AS_OBJ(callee);
            if (argc == cl->fn->arity && vm->frame_count < FRAMES_MAX &&
                vm->sp + 256 < vm->stack + STACK_MAX) {
                CallFrame *f = &vm->frames[vm->frame_count++];
                f->closure = cl;
                f->ip = cl->fn->chunk.code;
                f->slots = vm->sp - argc - 1;
                f->argc = argc;
                f->ctor = false;
                frame = f;
                ip = f->ip;
                DISPATCH();
            }
        }
        bool pushed;
        CHECK(call_value(vm, argc, &pushed));
        if (pushed)
            RELOAD();
        DISPATCH();
    }
    CASE(OP_INVOKE) {
        uint16_t nk = READ_U16();
        uint8_t argc = READ_BYTE();
        uint16_t ck = READ_U16();
        SYNC();
        vm_maybe_gc(vm);
        ObjString *name = AS_STRING(CONST(nk));
        Value recv = PEEK(argc);
        bool pushed = false;
        if (IS_OBJT(recv, OBJ_INSTANCE)) {
            ObjInstance *in = AS_INSTANCE(recv);
            InlineCache *c = &frame->closure->fn->caches[ck];
            Value m;
            if (c->klass == in->klass && c->index == -2)
                m = c->value;
            else {
                m = vm_find_method(in->klass, name);
                if (!IS_NULL(m)) {
                    c->klass = in->klass;
                    c->index = -2;
                    c->value = m;
                }
            }
            if (!IS_NULL(m)) {
                if (IS_OBJT(m, OBJ_NATIVE)) {
                    Value *args = vm->sp - argc - 1;
                    CHECK(call_native(vm, (ObjNative *)AS_OBJ(m), args, argc + 1, args));
                    vm->sp = args + 1;
                    DISPATCH();
                }
                CHECK(push_frame(vm, (ObjClosure *)AS_OBJ(m), argc));
                RELOAD();
                DISPATCH();
            }
            Value idx;
            if (table_get(&in->klass->field_index, name, &idx)) {
                vm->sp[-1 - argc] = in->fields[idx.as.i];
                CHECK(call_value(vm, argc, &pushed));
                if (pushed)
                    RELOAD();
                DISPATCH();
            }
            vm_throw(vm, "TypeError", "%s has no method '%s'", in->klass->name->chars, name->chars);
            THROW();
        }
        if (IS_OBJ(recv) && (OBJ_TYPE(recv) == OBJ_CLASS || OBJ_TYPE(recv) == OBJ_MODULE ||
                             OBJ_TYPE(recv) == OBJ_ENUM)) {
            Value callee;
            CHECK(vm_get_member(vm, recv, name, &callee));
            vm->sp[-1 - argc] = callee;
            CHECK(call_value(vm, argc, &pushed));
            if (pushed)
                RELOAD();
            DISPATCH();
        }
        if (IS_OBJ(recv) && (OBJ_TYPE(recv) == OBJ_CLOSURE || OBJ_TYPE(recv) == OBJ_NATIVE || OBJ_TYPE(recv) == OBJ_BOUND) &&
            !strcmp(name->chars, "call")) {
            CHECK(call_value(vm, argc, &pushed));
            if (pushed)
                RELOAD();
            DISPATCH();
        }
        if (IS_NULL(recv)) {
            vm_throw(vm, "CastError", "cannot cast null to a non-null type (calling '%s')", name->chars);
            THROW();
        }
        {
            bool found = false;
            Value out;
            Value *args = vm->sp - argc - 1;
            CHECK(lib_builtin_method(vm, recv, name, argc + 1, args, &out, &found));
            if (found) {
                vm->sp = args + 1;
                vm->sp[-1] = out;
                DISPATCH();
            }
            Value callee;
            CHECK(vm_get_member(vm, recv, name, &callee));
            if (argc == 0 && !IS_OBJ(callee)) {
                vm->sp[-1] = callee;
                DISPATCH();
            }
            vm->sp[-1 - argc] = callee;
            CHECK(call_value(vm, argc, &pushed));
            if (pushed)
                RELOAD();
        }
        DISPATCH();
    }
    CASE(OP_SUPER_INVOKE) {
        uint16_t nk = READ_U16();
        uint8_t argc = READ_BYTE();
        SYNC();
        ObjClass *owner = frame->closure->fn->owner;
        ObjString *name = AS_STRING(CONST(nk));
        if (!owner || !owner->super) {
            vm_throw(vm, "TypeError", "super used outside a subclass method");
            THROW();
        }
        ObjClass *sup = owner->super;
        Value m = vm_find_method(sup, name);
        if (IS_NULL(m)) {
            if (name == vm->s_init) {
                Value recv = PEEK(argc);
                CHECK(vm_init_fields(vm, sup, AS_INSTANCE(recv), argc, vm->sp - argc));
                vm->sp -= argc;
                vm->sp[-1] = val_null();
                DISPATCH();
            }
            vm_throw(vm, "TypeError", "%s has no method '%s'", sup->name->chars, name->chars);
            THROW();
        }
        CHECK(push_frame(vm, (ObjClosure *)AS_OBJ(m), argc));
        RELOAD();
        DISPATCH();
    }
    CASE(OP_SUPER_GET) {
        uint16_t nk = READ_U16();
        SYNC();
        ObjClass *owner = frame->closure->fn->owner;
        if (!owner || !owner->super) {
            vm_throw(vm, "TypeError", "super used outside a subclass method");
            THROW();
        }
        Value m = vm_find_method(owner->super, AS_STRING(CONST(nk)));
        if (IS_NULL(m)) {
            vm_throw(vm, "TypeError", "%s has no method '%s'", owner->super->name->chars, AS_CSTR(CONST(nk)));
            THROW();
        }
        vm->sp[-1] = val_obj(new_bound(vm, PEEK(0), m));
        DISPATCH();
    }
    CASE(OP_CALL_STMT) {
        Value v = PEEK(0);
        if (IS_OBJ(v)) {
            ObjType t = (ObjType)OBJ_TYPE(v);
            if (t == OBJ_CLOSURE || t == OBJ_NATIVE || t == OBJ_BOUND) {
                SYNC();
                bool pushed;
                CHECK(call_value(vm, 0, &pushed));
                if (pushed)
                    RELOAD();
            } else if (t == OBJ_CLASS) {
                SYNC();
                vm_throw(vm, "TypeError", "a class reference on its own does nothing; call %s() to construct",
                         AS_CLASS(v)->name->chars);
                THROW();
            }
        }
        DISPATCH();
    }
    CASE(OP_CLOSURE) {
        uint16_t fk = READ_U16();
        ObjFunction *fn = (ObjFunction *)AS_OBJ(CONST(fk));
        ObjClosure *cl = new_closure(vm, fn);
        PUSH(val_obj(cl));
        for (int i = 0; i < cl->upvalue_count; i++) {
            uint8_t is_local = READ_BYTE(), index = READ_BYTE();
            cl->upvalues[i] = is_local ? capture(vm, frame->slots + index) : frame->closure->upvalues[index];
        }
        DISPATCH();
    }
    CASE(OP_CLOSE_UPVAL) {
        close_upvalues(vm, vm->sp - 1);
        vm->sp--;
        DISPATCH();
    }
    CASE(OP_RETURN) {
        Value result = POP();
        if (frame->ctor) {
            SYNC();
            ObjInstance *inst = AS_INSTANCE(frame->slots[0]);
            CHECK(check_required(vm, inst, true));
            result = frame->slots[0];
        }
        close_upvalues(vm, frame->slots);
        int index = vm->frame_count - 1;
        while (vm->handler_count > 0 && vm->handlers[vm->handler_count - 1].frame >= index)
            vm->handler_count--;
        vm->frame_count--;
        vm->sp = frame->slots;
        PUSH(result);
        if (index == base)
            return RUN_OK;
        RELOAD();
        DISPATCH();
    }
    CASE(OP_ARRAY) {
        uint16_t n = READ_U16();
        Value *items = vm->sp - n;
        int kind = -1;
        bool tuple = false;
        for (int i = 0; i < n; i++) {
            if (IS_NULL(items[i]))
                continue;
            int k = value_kind(items[i]);
            if (kind == -1)
                kind = k;
            else if (k != kind)
                tuple = true;
        }
        ObjList *l = new_list(vm, tuple ? OBJ_TUPLE : OBJ_ARRAY, n);
        for (int i = 0; i < n; i++)
            l->items[i] = items[i];
        l->count = n;
        vm->sp -= n;
        PUSH(val_obj(l));
        DISPATCH();
    }
    CASE(OP_MAP)
    CASE(OP_SET) {
        uint8_t op = ip[-1];
        uint16_t n = READ_U16();
        SYNC();
        ObjMap *m = new_map(vm, op == OP_MAP ? OBJ_MAP : OBJ_SET);
        push_root(vm, val_obj(m));
        Value *items = vm->sp - (op == OP_MAP ? 2 * n : n);
        bool ok = true;
        for (int i = 0; i < n && ok; i++)
            ok = op == OP_MAP ? map_set(vm, m, items[2 * i], items[2 * i + 1]) : map_set(vm, m, items[i], val_null());
        pop_root(vm, 1);
        CHECK(ok);
        vm->sp = items;
        PUSH(val_obj(m));
        DISPATCH();
    }
    CASE(OP_STRING) {
        uint16_t n = READ_U16();
        SYNC();
        Buf b;
        buf_init(&b);
        Value *items = vm->sp - n;
        for (int i = 0; i < n; i++) {
            if (IS_STRING(items[i]))
                buf_add(&b, AS_CSTR(items[i]), (size_t)AS_STRING(items[i])->len);
            else if (!value_to_buf(vm, items[i], &b, false, 0)) {
                buf_free(&b);
                THROW();
            }
        }
        ObjString *s = str_copy(vm, b.data ? b.data : "", b.len);
        buf_free(&b);
        vm->sp = items;
        PUSH(val_obj(s));
        DISPATCH();
    }
    CASE(OP_FORMAT) {
        uint16_t sk = READ_U16();
        SYNC();
        Buf b;
        buf_init(&b);
        if (!value_format_spec(vm, PEEK(0), AS_CSTR(CONST(sk)), &b)) {
            buf_free(&b);
            THROW();
        }
        vm->sp[-1] = val_obj(str_copy(vm, b.data ? b.data : "", b.len));
        buf_free(&b);
        DISPATCH();
    }
    CASE(OP_ELEMENT) {
        uint16_t tk = READ_U16(), nk = READ_U16(), nchildren = READ_U16();
        SYNC();
        Value tag = CONST(tk);
        ObjList *names = AS_LIST(CONST(nk));
        Value *attrs = vm->sp - nchildren - names->count;
        Value out;
        CHECK(lib_element_new(vm, IS_NULL(tag) ? NULL : AS_STRING(tag), names, attrs, nchildren,
                              attrs + names->count, &out));
        vm->sp = attrs;
        PUSH(out);
        DISPATCH();
    }
    CASE(OP_JSON) {
        uint16_t k = READ_U16();
        SYNC();
        Value v;
        ObjString *text = AS_STRING(CONST(k));
        CHECK(lib_json_parse(vm, text->chars, (size_t)text->len, &v));
        PUSH(val_obj(new_json(vm, v)));
        DISPATCH();
    }
    CASE(OP_TRY) {
        uint16_t off = READ_U16();
        if (vm->handler_count == HANDLERS_MAX) {
            SYNC();
            vm_throw(vm, "RuntimeError", "too many nested try blocks");
            THROW();
        }
        Handler *h = &vm->handlers[vm->handler_count++];
        h->frame = vm->frame_count - 1;
        h->sp = vm->sp;
        h->ip = ip + off;
        DISPATCH();
    }
    CASE(OP_POP_HANDLER) {
        vm->handler_count--;
        DISPATCH();
    }
    CASE(OP_THROW) {
        Value v = POP();
        SYNC();
        if (IS_NULL(v) || IS_UNDEF(v) || IS_OBJT(v, OBJ_CLOSURE) || IS_OBJT(v, OBJ_NATIVE) || IS_OBJT(v, OBJ_BOUND)) {
            vm_throw(vm, "TypeError", "null and functions cannot be thrown");
            THROW();
        }
        vm_throw_value(vm, v);
        THROW();
    }
    CASE(OP_THROW_NAMED) {
        Value msg = PEEK(0), name = PEEK(1);
        SYNC();
        if (!IS_STRING(name)) {
            vm_throw(vm, "TypeError", "error names must be strings");
            THROW();
        }
        ObjString *m = IS_STRING(msg) ? AS_STRING(msg) : value_to_string(vm, msg);
        if (!m)
            THROW();
        ObjError *e = new_error(vm, str_intern(vm, AS_CSTR(name), (size_t)AS_STRING(name)->len), m);
        vm->sp -= 2;
        vm_throw_value(vm, val_obj(e));
        THROW();
    }
    CASE(OP_CATCH_MATCH) {
        uint16_t k = READ_U16();
        Value c = CONST(k), err = PEEK(0);
        bool m;
        if (IS_STRING(c))
            m = error_matches(vm, err, AS_STRING(c));
        else {
            ObjTypeDesc *t = AS_TYPE(c);
            if (t->kind == TY_ERROR || t->kind == TY_NAMED)
                m = (t->name && error_matches(vm, err, t->name)) || (t->kind == TY_NAMED && vm_type_test(vm, err, t) &&
                                                                    IS_OBJT(vm_resolve_named(vm, t), OBJ_CLASS));
            else
                m = !IS_OBJT(err, OBJ_ERROR) && vm_type_test(vm, err, t);
        }
        vm->sp[-1] = val_bool(m);
        DISPATCH();
    }
    CASE(OP_FOR_PREP) {
        uint8_t s = READ_BYTE();
        ip++; /* names */
        SYNC();
        CHECK(for_prep(vm, frame->slots + s));
        DISPATCH();
    }
    CASE(OP_FOR_NEXT) {
        uint8_t s = READ_BYTE(), names = READ_BYTE();
        uint16_t exit = READ_U16();
        Value *slots = frame->slots + s;
        if (IS_ARRAY(slots[0]) && names == 1 && IS_INT(slots[2])) {
            ObjList *l = AS_LIST(slots[0]);
            int64_t i = slots[1].as.i;
            if (i >= slots[2].as.i) {
                ip += exit;
                DISPATCH();
            }
            if (i < l->count) {
                slots[1].as.i++;
                PUSH(l->items[i]);
                DISPATCH();
            }
        }
        SYNC();
        bool done;
        CHECK(for_next(vm, slots, names, &done));
        if (done)
            ip += exit;
        DISPATCH();
    }
    CASE(OP_RANGE_PREP) {
        uint8_t s = READ_BYTE();
        if (!IS_INT(frame->slots[s]) || !IS_INT(frame->slots[s + 1])) {
            SYNC();
            vm_throw(vm, "TypeError", "range endpoints must be integers");
            THROW();
        }
        DISPATCH();
    }
    CASE(OP_RANGE_NEXT) {
        uint8_t s = READ_BYTE(), incl = READ_BYTE();
        uint16_t exit = READ_U16();
        Value *slots = frame->slots + s;
        int64_t cur = slots[0].as.i, end = slots[1].as.i;
        if (incl ? cur > end : cur >= end) {
            ip += exit;
            DISPATCH();
        }
        PUSH(val_int(cur));
        if (cur == INT64_MAX)
            slots[1].as.i = cur - 1;
        else
            slots[0].as.i = cur + 1;
        DISPATCH();
    }
    CASE(OP_ARG_PRESENT) {
        uint8_t idx = READ_BYTE();
        uint16_t off = READ_U16();
        if (frame->argc > idx)
            ip += off;
        DISPATCH();
    }
    CASE(OP_INHERIT) {
        uint16_t k = READ_U16();
        Value base = POP();
        ObjClass *klass = AS_CLASS(CONST(k));
        if (!IS_OBJT(base, OBJ_CLASS) || AS_CLASS(base)->is_interface) {
            SYNC();
            vm_throw(vm, "TypeError", "%s can only extend a class", klass->name->chars);
            THROW();
        }
        if (!AS_CLASS(base)->finalized)
            finalize_class(vm, AS_CLASS(base));
        klass->super = AS_CLASS(base);
        DISPATCH();
    }
    CASE(OP_IMPLEMENT) {
        uint16_t k = READ_U16();
        Value iface = POP();
        ObjClass *klass = AS_CLASS(CONST(k));
        if (!IS_OBJT(iface, OBJ_CLASS)) {
            SYNC();
            vm_throw(vm, "TypeError", "%s can only implement interfaces", klass->name->chars);
            THROW();
        }
        klass->interfaces = tb_realloc(vm, klass->interfaces, sizeof(ObjClass *) * (size_t)klass->ninterfaces,
                                       sizeof(ObjClass *) * (size_t)(klass->ninterfaces + 1));
        klass->interfaces[klass->ninterfaces++] = AS_CLASS(iface);
        DISPATCH();
    }
    CASE(OP_FINALIZE) {
        uint16_t k = READ_U16();
        ObjClass *klass = AS_CLASS(CONST(k));
        finalize_class(vm, klass);
        PUSH(val_obj(klass));
        DISPATCH();
    }
    CASE(OP_MATCH_VALUE) {
        SYNC();
        bool r;
        CHECK(match_value(vm, PEEK(1), PEEK(0), &r));
        vm->sp--;
        vm->sp[-1] = val_bool(r);
        DISPATCH();
    }
    CASE(OP_MATCH_ENUM) {
        uint16_t nk = READ_U16();
        Value fallback = PEEK(0), subj = PEEK(1);
        ObjString *name = AS_STRING(CONST(nk));
        bool r = false;
        SYNC();
        if (IS_OBJT(subj, OBJ_ENUMVAL)) {
            ObjEnumVal *ev = (ObjEnumVal *)AS_OBJ(subj);
            if (IS_OBJT(fallback, OBJ_ENUMVAL) && ((ObjEnumVal *)AS_OBJ(fallback))->e == ev->e)
                r = AS_OBJ(fallback) == AS_OBJ(subj);
            else
                r = ev->name == name;
        } else if (IS_OBJT(subj, OBJ_INSTANCE) && AS_INSTANCE(subj)->klass->is_variant &&
                   !IS_OBJT(fallback, OBJ_CLASS) && !IS_OBJT(fallback, OBJ_INSTANCE)) {
            const char *full = AS_INSTANCE(subj)->klass->name->chars;
            const char *dot = strrchr(full, '.');
            r = !strcmp(dot ? dot + 1 : full, name->chars);
        } else if (!IS_NULL(fallback) || IS_NULL(subj)) {
            CHECK(match_value(vm, subj, fallback, &r));
        }
        vm->sp--;
        vm->sp[-1] = val_bool(r);
        DISPATCH();
    }
    CASE(OP_IN_RANGE) {
        uint8_t incl = READ_BYTE();
        Value hi = PEEK(0), lo = PEEK(1), v = PEEK(2);
        bool r = false;
        if (IS_NUM(v) && IS_NUM(lo) && IS_NUM(hi)) {
            double x = AS_NUM(v);
            if (IS_INT(v) && IS_INT(lo) && IS_INT(hi))
                r = v.as.i >= lo.as.i && (incl ? v.as.i <= hi.as.i : v.as.i < hi.as.i);
            else
                r = x >= AS_NUM(lo) && (incl ? x <= AS_NUM(hi) : x < AS_NUM(hi));
        } else if (IS_CHAR(v) && IS_CHAR(lo) && IS_CHAR(hi))
            r = v.as.c >= lo.as.c && (incl ? v.as.c <= hi.as.c : v.as.c < hi.as.c);
        vm->sp -= 2;
        vm->sp[-1] = val_bool(r);
        DISPATCH();
    }
    CASE(OP_LEN_IS) {
        uint16_t n = READ_U16();
        ip++;
        Value v = PEEK(0);
        vm->sp[-1] = val_bool(IS_LIST(v) && AS_LIST(v)->count == n);
        DISPATCH();
    }
    CASE(OP_FIELD_AT) {
        uint8_t i = READ_BYTE();
        Value v = PEEK(0);
        if (!IS_OBJT(v, OBJ_INSTANCE) || i >= AS_INSTANCE(v)->nfields) {
            SYNC();
            vm_throw(vm, "ValueError", "pattern has more fields than the value");
            THROW();
        }
        vm->sp[-1] = AS_INSTANCE(v)->fields[i];
        DISPATCH();
    }
    CASE(OP_RESULT_IS) {
        uint8_t ok = READ_BYTE();
        Value v = PEEK(0);
        vm->sp[-1] = val_bool(IS_OBJT(v, OBJ_RESULT) && ((ObjResult *)AS_OBJ(v))->ok == (ok != 0));
        DISPATCH();
    }
    CASE(OP_RESULT_VALUE) {
        vm->sp[-1] = ((ObjResult *)AS_OBJ(PEEK(0)))->value;
        DISPATCH();
    }
    CASE(OP_DESTRUCTURE) {
        uint16_t n = READ_U16();
        Value v = PEEK(0);
        if (!IS_LIST(v) || AS_LIST(v)->count != n) {
            SYNC();
            vm_throw(vm, "ValueError", "cannot destructure %d element(s) into %d name(s)",
                     IS_LIST(v) ? AS_LIST(v)->count : 0, n);
            THROW();
        }
        DISPATCH();
    }
    CASE(OP_ATTR) {
        uint16_t nk = READ_U16();
        SYNC();
        Value out;
        CHECK(get_attr(vm, PEEK(0), AS_STRING(CONST(nk)), &out));
        vm->sp[-1] = out;
        DISPATCH();
    }
    CASE(OP_NO_MATCH) {
        SYNC();
        ObjString *s = value_to_string(vm, PEEK(0));
        vm_throw(vm, "MatchError", "no case matches %s", s ? s->chars : "the value");
        THROW();
    }
    CASE(OP_IMPORT) {
        uint16_t mk = READ_U16();
        ObjModule *m = AS_MODULE(CONST(mk));
        if (m->initialized || m->initializing || IS_NULL(m->init)) {
            PUSH(val_null());
            DISPATCH();
        }
        m->initializing = true;
        m->initialized = true;
        SYNC();
        PUSH(m->init);
        bool pushed;
        CHECK(call_value(vm, 0, &pushed));
        if (pushed)
            RELOAD();
        DISPATCH();
    }
    CASE(OP_TYPE_ERROR) {
        uint16_t k = READ_U16();
        SYNC();
        vm_throw(vm, "TypeError", "%s", AS_CSTR(CONST(k)));
        THROW();
    }
    SWITCH_END

on_error : {
    /* Unwind to the nearest handler that belongs to this run. */
    if (vm->exiting)
        return RUN_EXIT;
    if (vm->handler_count > 0 && vm->handlers[vm->handler_count - 1].frame >= base) {
        Handler h = vm->handlers[--vm->handler_count];
        vm->frame_count = h.frame + 1;
        close_upvalues(vm, h.sp);
        vm->sp = h.sp;
        PUSH(vm->error);
        vm->has_error = false;
        frame = &vm->frames[vm->frame_count - 1];
        ip = h.ip;
        DISPATCH();
    }
    close_upvalues(vm, vm->frames[base].slots);
    vm->sp = vm->frames[base].slots;
    vm->frame_count = base;
    return RUN_ERROR;
}
}

/* ---------------------------------------------------------------- modules */

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t cap = 65536, n = 0;
    char *buf = xmalloc(cap);
    for (;;) {
        if (n + 4096 > cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
        size_t r = fread(buf + n, 1, cap - n - 1, f);
        n += r;
        if (r == 0)
            break;
    }
    fclose(f);
    buf[n] = 0;
    *len = n;
    return buf;
}
static bool file_exists(const char *p) { return tb_path_kind(p) == 1; }
static bool dir_exists(const char *p) { return tb_path_kind(p) == 2; }
static char *dir_of(const char *path) {
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bs = strrchr(path, '\\');
    if (bs && (!slash || bs > slash))
        slash = bs;
#endif
    if (!slash)
        return xstrdup(".");
    return xstrndup(path, (size_t)(slash - path));
}
static char *join_path(const char *a, const char *b) {
    size_t n = strlen(a) + strlen(b) + 2;
    char *r = xmalloc(n);
    snprintf(r, n, "%s/%s", a, b);
    return r;
}
static char *canonical(const char *p) {
#ifdef _WIN32
    char buf[4096];
    if (_fullpath(buf, p, sizeof buf))
        return xstrdup(buf);
#else
    char *r = realpath(p, NULL);
    if (r) {
        char *c = xstrdup(r);
        free(r);
        return c;
    }
#endif
    return xstrdup(p);
}
/* Tries dir/rel(.fw|.fly); returns an allocated path or NULL. */
static char *try_module_file(const char *dir, const char *rel) {
    static const char *exts[] = {"", ".fw", ".fly"};
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(dir) + strlen(rel) + 8;
        char *p = xmalloc(n);
        snprintf(p, n, "%s/%s%s", dir, rel, exts[i]);
        if (i == 0) {
            size_t rl = strlen(rel);
            bool has_ext = (rl > 3 && !strcmp(rel + rl - 3, ".fw")) || (rl > 4 && !strcmp(rel + rl - 4, ".fly"));
            if (!has_ext) {
                free(p);
                continue;
            }
        }
        if (file_exists(p))
            return p;
        free(p);
    }
    return NULL;
}
static char *package_main(const char *pkgdir, const char *name) {
    char rel[1024];
    /* pulley.toml `main = "file"` */
    char *toml = join_path(pkgdir, "pulley.toml");
    size_t len;
    char *text = read_file(toml, &len);
    free(toml);
    if (text) {
        char *m = strstr(text, "\nmain");
        if (!m && !strncmp(text, "main", 4))
            m = text - 1;
        if (m) {
            char *q = strchr(m + 1, '"');
            char *e = q ? strchr(q + 1, '"') : NULL;
            if (q && e && e - q - 1 < (long)sizeof rel - 1) {
                snprintf(rel, sizeof rel, "%.*s", (int)(e - q - 1), q + 1);
                char *p = try_module_file(pkgdir, rel);
                if (p) {
                    free(text);
                    return p;
                }
            }
        }
        free(text);
    }
    const char *cands[] = {name, "main", "lib"};
    for (int i = 0; i < 3; i++) {
        char *p = try_module_file(pkgdir, cands[i]);
        if (p)
            return p;
    }
    return NULL;
}
static char *find_in_modules_dir(const char *mods, const char *name) {
    /* name may be pkg or pkg.part */
    char pkg[512];
    const char *dot = strchr(name, '.');
    snprintf(pkg, sizeof pkg, "%.*s", dot ? (int)(dot - name) : (int)strlen(name), name);
    char *pkgdir = join_path(mods, pkg);
    if (!dir_exists(pkgdir)) {
        char link[600];
        snprintf(link, sizeof link, "%s.link", pkgdir);
        size_t len;
        char *text = read_file(link, &len);
        free(pkgdir);
        pkgdir = NULL;
        if (!text)
            return NULL;
        while (len && (text[len - 1] == '\n' || text[len - 1] == '\r' || text[len - 1] == ' '))
            text[--len] = 0;
        pkgdir = text[0] == '/' ? xstrdup(text) : join_path(mods, text);
        free(text);
        if (!dir_exists(pkgdir)) {
            free(pkgdir);
            return NULL;
        }
    }
    char *r = NULL;
    if (dot) {
        char rel[512];
        snprintf(rel, sizeof rel, "%s", dot + 1);
        for (char *c = rel; *c; c++)
            if (*c == '.')
                *c = '/';
        r = try_module_file(pkgdir, rel);
    } else
        r = package_main(pkgdir, pkg);
    free(pkgdir);
    return r;
}
static bool is_path_import(const char *name) {
    size_t n = strlen(name);
    if (strchr(name, '/') || strchr(name, '\\'))
        return true;
    return (n > 3 && !strcmp(name + n - 3, ".fw")) || (n > 4 && !strcmp(name + n - 4, ".fly"));
}
static char *resolve_module_path(VM *vm, const char *name, const char *from) {
    char *from_dir = dir_of(from ? from : ".");
    char *main_dir = dir_of(vm->main_path ? vm->main_path : ".");
    char *result = NULL;
    if (is_path_import(name)) {
        const char *dirs[] = {from_dir, main_dir};
        for (int i = 0; i < 2 && !result; i++) {
            if (name[0] == '/'
#ifdef _WIN32
                || (name[0] && name[1] == ':')
#endif
            ) {
                result = try_module_file("", name);
                if (!result && file_exists(name))
                    result = xstrdup(name);
                break;
            }
            result = try_module_file(dirs[i], name);
        }
        free(from_dir);
        free(main_dir);
        return result;
    }
    char rel[1024];
    snprintf(rel, sizeof rel, "%s", name);
    for (char *c = rel; *c; c++)
        if (*c == '.')
            *c = '/';
    result = try_module_file(from_dir, rel);
    if (!result && strcmp(from_dir, main_dir) != 0)
        result = try_module_file(main_dir, rel);
    /* nearest .flywheel-modules at or above the importing file */
    if (!result) {
        char *d = canonical(from_dir);
        for (;;) {
            char *mods = join_path(d, ".flywheel-modules");
            if (dir_exists(mods))
                result = find_in_modules_dir(mods, name);
            free(mods);
            if (result)
                break;
            char *up = dir_of(d);
            if (!strcmp(up, d) || !strcmp(up, ".") || !up[0]) {
                free(up);
                break;
            }
            free(d);
            d = up;
        }
        free(d);
    }
    if (!result) {
        const char *home = getenv("FLYWHEEL_HOME");
        if (home) {
            char *mods = join_path(home, "modules");
            result = find_in_modules_dir(mods, name);
            free(mods);
        }
    }
    if (!result && vm->stdlib_dir) {
        result = try_module_file(vm->stdlib_dir, rel);
    }
    free(from_dir);
    free(main_dir);
    return result;
}

static ObjModule *compile_file_module(VM *vm, ObjString *name, const char *path, const char *source,
                                      size_t len, CompileError *err) {
    ObjModule *m = new_module(vm, name);
    m->path = str_cstr(vm, path);
    push_root(vm, val_obj(m));
    table_set(vm, &vm->modules, str_intern_c(vm, path), val_obj(m));
    va_push(vm, &vm->module_list, val_obj(m));
    m->initializing = true; /* compiling: imports of this module are cycles */
    ObjFunction *fn = compile_module(vm, m, source, len, m->path->chars, err);
    m->initializing = false;
    pop_root(vm, 1);
    if (!fn)
        return NULL;
    vm->gc_off++;
    m->init = val_obj(new_closure(vm, fn));
    vm->gc_off--;
    return m;
}

ObjModule *vm_load_module(VM *vm, const char *name, const char *from_path, bool *native,
                          CompileError *err) {
    *native = false;
    char *path = resolve_module_path(vm, name, from_path);
    if (path) {
        char *canon = canonical(path);
        free(path);
        Value existing;
        if (table_get(&vm->modules, str_intern_c(vm, canon), &existing)) {
            ObjModule *m = AS_MODULE(existing);
            if (m->initializing && IS_NULL(m->init)) {
                snprintf(err->message, sizeof err->message, "import cycle: %s imports itself through %s", canon,
                         from_path ? from_path : "?");
                free(canon);
                return NULL;
            }
            free(canon);
            return m;
        }
        size_t len;
        char *src = read_file(canon, &len);
        if (!src) {
            snprintf(err->message, sizeof err->message, "cannot read module file %s", canon);
            free(canon);
            return NULL;
        }
        const char *base = strrchr(name, '.');
        ObjString *mname = str_intern_c(vm, is_path_import(name) ? name : (base ? base + 1 : name));
        CompileError sub;
        memset(&sub, 0, sizeof sub);
        ObjModule *m = compile_file_module(vm, mname, canon, src, len, &sub);
        if (!m) {
            /* keep the source alive for the diagnostic: copy message with location */
            snprintf(err->message, sizeof err->message, "%.300s:%d: %.600s", canon, sub.line, sub.message);
            err->file = NULL;
        }
        free(src);
        free(canon);
        return m;
    }
    const char *embedded = lib_embedded_module(name);
    if (embedded) {
        char key[256];
        snprintf(key, sizeof key, "<library %s>", name);
        Value existing;
        if (table_get(&vm->modules, str_intern_c(vm, key), &existing))
            return AS_MODULE(existing);
        CompileError sub;
        memset(&sub, 0, sizeof sub);
        ObjModule *m = compile_file_module(vm, str_intern_c(vm, name), key, embedded, strlen(embedded), &sub);
        if (!m)
            snprintf(err->message, sizeof err->message, "library module %.100s:%d: %.800s", name, sub.line, sub.message);
        return m;
    }
    ObjModule *m = lib_native_module(vm, name);
    if (m) {
        *native = true;
        return m;
    }
    if (!strncmp(name, "http://", 7) || !strncmp(name, "https://", 8) || strstr(name, "://"))
        snprintf(err->message, sizeof err->message, "Turbine cannot import from URLs (%s); download the file and import it by path", name);
    else if (lib_is_native_module(name))
        snprintf(err->message, sizeof err->message, "module '%s' is not available in Turbine; use the Flywheel compiler for it", name);
    else
        snprintf(err->message, sizeof err->message, "cannot find module '%s'", name);
    return NULL;
}

/* ---------------------------------------------------------------- entry points */

void vm_init(VM *vm, int argc, char **argv) {
    memset(vm, 0, sizeof *vm);
    vm->stack = xmalloc(sizeof(Value) * STACK_MAX);
    vm->sp = vm->stack;
    vm->next_gc = 16u * 1024 * 1024;
    vm->error = val_null();
    table_init(&vm->strings);
    table_init(&vm->builtins);
    table_init(&vm->modules);
    va_init(&vm->module_list);
    for (int i = 0; i < TM_COUNT; i++)
        table_init(&vm->type_methods[i]);
    vm->argc = argc;
    vm->argv = argv;
    vm->empty_fragment = val_null();
    for (size_t i = 0; i < sizeof vm->desc_args / sizeof vm->desc_args[0]; i++)
        vm->desc_args[i] = val_null();
    vm->gc_off++;
    vm->s_init = str_intern_c(vm, "init");
    vm->s_to_string = str_intern_c(vm, "to_string");
    vm->s_length = str_intern_c(vm, "length");
    vm->s_message = str_intern_c(vm, "message");
    vm->s_name = str_intern_c(vm, "name");
    lib_core_init(vm);
    vm->gc_off--;
    const char *stress = getenv("TURBINE_GC_STRESS");
    if (stress && strcmp(stress, "0") != 0) {
        vm->gc_stress = true;
        vm->next_gc = 0;
    }
}
void vm_free(VM *vm) {
    table_free(vm, &vm->strings);
    table_free(vm, &vm->builtins);
    table_free(vm, &vm->modules);
    va_free(vm, &vm->module_list);
    for (int i = 0; i < TM_COUNT; i++)
        table_free(vm, &vm->type_methods[i]);
    free_all_objects(vm);
    free(vm->stack);
}

void vm_print_trace(VM *vm) { fputs(vm->trace, stderr); }

RunResult vm_run_main(VM *vm, const char *path, const char *source, size_t len, Value *result) {
    vm->main_path = path;
    CompileError err;
    memset(&err, 0, sizeof err);
    ObjModule *m = compile_file_module(vm, str_intern_c(vm, "main"), path, source, len, &err);
    if (!m) {
        fflush(stdout);
        fprintf(stderr, "%s:%d:%d: error: %s\n", err.file && err.file[0] != '<' ? path : (err.file ? err.file : path),
                err.line, err.col ? err.col : 1, err.message);
        return RUN_ERROR;
    }
    m->initialized = true;
    push_root(vm, val_obj(m));
    Value out;
    bool ok = vm_call(vm, m->init, 0, NULL, &out);
    pop_root(vm, 1);
    if (vm->exiting)
        return RUN_EXIT;
    if (!ok) {
        vm_report_error(vm, vm->error);
        return RUN_ERROR;
    }
    *result = out;
    return RUN_OK;
}
