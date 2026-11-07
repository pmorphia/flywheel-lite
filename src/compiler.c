/* Single-pass compiler from the Flywheel AST to Flywheel Lite bytecode. */
#include "front.h"
#include "opcodes.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>

#define MAX_LOCALS 256
#define MAX_UPVALS 256

typedef struct {
    const char *name;
    int depth;
    bool captured, is_const;
    Value type;
} Local;

typedef struct {
    uint8_t index;
    bool is_local;
} Upval;

typedef struct {
    int start;
    int depth;
    int *breaks;
    int nbreaks, capbreaks;
} Loop;

typedef struct {
    NodeList *finalizer;
    int handlers;
    int loop_count;
} TryCtx;

typedef struct FuncState {
    struct FuncState *enclosing;
    ObjFunction *fn;
    Local locals[MAX_LOCALS];
    int nlocals;
    Upval upvals[MAX_UPVALS];
    int scope_depth;
    Loop loops[64];
    int loop_count;
    TryCtx tries[64];
    int ntries;
    bool is_method, is_script, is_component, in_class;
    Value ret_type;
} FuncState;

typedef struct {
    const char *name;
    ObjModule *module; /* NULL: native value in `value` */
    ObjModule *from;
    int slot;
    Value value;
    bool component;
} Wild;

typedef struct {
    const char *alias;
    ObjModule *module;
} Alias;

typedef struct {
    VM *vm;
    ObjModule *module;
    Program *prog;
    FuncState *fs;
    CompileError *err;
    jmp_buf jmp;
    const char *path;
    Arena *arena;
    Alias *aliases;
    int naliases;
    Wild *wilds;
    int nwilds;
    Value *global_types; /* per module slot */
    int nglobal_types;
    bool *global_const;
    int line, col;
    const char *unit_hint; /* declared type of the declaration being compiled, for 100m literals */
} Compiler;

TB_NORETURN static void cfail(Compiler *C, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(C->err->message, sizeof C->err->message, fmt, ap);
    va_end(ap);
    C->err->line = line;
    C->err->col = line == C->line ? C->col : 0;
    longjmp(C->jmp, 1);
}

/* ---------------------------------------------------------------- emission */

static Chunk *chunk(Compiler *C) { return &C->fs->fn->chunk; }
static void emit(Compiler *C, uint8_t b) { chunk_write(C->vm, chunk(C), b, C->line); }
static void emit2(Compiler *C, uint8_t a, uint8_t b) {
    emit(C, a);
    emit(C, b);
}
static void emit16(Compiler *C, int v) {
    emit(C, (uint8_t)((v >> 8) & 0xff));
    emit(C, (uint8_t)(v & 0xff));
}
static int make_const(Compiler *C, Value v) {
    int k = chunk_add_const(C->vm, chunk(C), v);
    if (k > 65535)
        cfail(C, C->line, "too many constants in one function");
    return k;
}
static void emit_const(Compiler *C, Value v) {
    emit(C, OP_CONST);
    emit16(C, make_const(C, v));
}
static int name_const(Compiler *C, const char *name) {
    return make_const(C, val_obj(str_intern_c(C->vm, name)));
}
static int new_cache(Compiler *C) {
    ObjFunction *f = C->fs->fn;
    return f->ncaches++;
}
static int emit_jump(Compiler *C, uint8_t op) {
    emit(C, op);
    emit16(C, 0xffff);
    return chunk(C)->count - 2;
}
static void patch_jump(Compiler *C, int at) {
    int jump = chunk(C)->count - at - 2;
    if (jump > 65535)
        cfail(C, C->line, "block too large to jump over");
    chunk(C)->code[at] = (uint8_t)((jump >> 8) & 0xff);
    chunk(C)->code[at + 1] = (uint8_t)(jump & 0xff);
}
static void emit_loop(Compiler *C, int start) {
    emit(C, OP_LOOP);
    int off = chunk(C)->count - start + 2;
    if (off > 65535)
        cfail(C, C->line, "loop body too large");
    emit16(C, off);
}
static void emit_int(Compiler *C, int64_t v) {
    if (v >= -32768 && v <= 32767) {
        emit(C, OP_INT);
        emit16(C, (int)(uint16_t)(int16_t)v);
    } else
        emit_const(C, val_int(v));
}

/* ---------------------------------------------------------------- types */

static bool type_converts(ObjTypeDesc *t) {
    switch (t->kind) {
    case TY_DOUBLE:
    case TY_FLOAT:
    case TY_TUPLE:
    case TY_HTUPLE:
    case TY_SET:
    case TY_WEAK:
        return true;
    case TY_INT:
        return t->bits < 64 || t->is_unsigned;
    case TY_NAMED: {
        /* float[ns] converts int[ns] values on assignment, as at declarations */
        char ns[96];
        bool real;
        return t->name && quantity_class_name(t->name->chars, ns, sizeof ns, &real) && real;
    }
    case TY_NULLABLE:
    case TY_ARRAY:
        return t->nargs && t->args[0]->convert;
    case TY_MAP:
        return t->nargs == 2 && (t->args[0]->convert || t->args[1]->convert);
    default:
        return false;
    }
}

typedef struct {
    Compiler *C;
    const char *s;
    size_t p;
} TypeReader;

static ObjTypeDesc *read_type(TypeReader *r);
static ObjModule *find_alias(Compiler *C, const char *name);
static bool json_alias(Compiler *C, const char *n, size_t len) {
    char *h = xstrndup(n, len);
    ObjModule *m = find_alias(C, h);
    free(h);
    return m && m->native && !strcmp(m->name->chars, "json");
}
static int find_wild_slot(Compiler *C, const char *name, ObjModule **m);
/* Named types resolve at compile time to a module global (and optional static member). */
static void resolve_named_type(Compiler *C, ObjTypeDesc *t, const char *n, size_t len) {
    t->slot = -1;
    t->module = NULL;
    t->member = NULL;
    const char *dot = memchr(n, '.', len);
    size_t hl = dot ? (size_t)(dot - n) : len;
    ObjString *head = str_intern(C->vm, n, hl);
    int slot = module_find(C->module, head);
    if (slot >= 0) {
        t->module = C->module;
        t->slot = slot;
        if (dot)
            t->member = str_intern(C->vm, dot + 1, len - hl - 1);
        return;
    }
    char *hs = xstrndup(n, hl);
    ObjModule *m = find_alias(C, hs);
    if (!m) {
        ObjModule *wm = NULL;
        int ws = find_wild_slot(C, hs, &wm);
        if (ws >= 0) {
            t->module = wm;
            t->slot = ws;
            if (dot)
                t->member = str_intern(C->vm, dot + 1, len - hl - 1);
        }
        free(hs);
        return;
    }
    free(hs);
    if (!dot || m->native)
        return;
    const char *rest = dot + 1;
    size_t rl = len - hl - 1;
    const char *dot2 = memchr(rest, '.', rl);
    size_t fl = dot2 ? (size_t)(dot2 - rest) : rl;
    int s = module_find(m, str_intern(C->vm, rest, fl));
    if (s < 0)
        return;
    t->module = m;
    t->slot = s;
    if (dot2)
        t->member = str_intern(C->vm, dot2 + 1, rl - fl - 1);
}
static void type_add_arg(VM *vm, ObjTypeDesc *t, ObjTypeDesc *a) {
    t->args = tb_realloc(vm, t->args, sizeof(ObjTypeDesc *) * (size_t)t->nargs,
                         sizeof(ObjTypeDesc *) * (size_t)(t->nargs + 1));
    t->args[t->nargs++] = a;
}
static const char *builtin_errors[] = {
    "Error", "FileError", "IndexError", "KeyError", "NumericError", "AssertionError", "JsonError",
    "CastError", "SelectorError", "InputError", "ProcessError", "DesktopError", "RegexError",
    "NetworkError", "ValueError", "HttpError", "DatabaseError", "NativeError", "ResultError",
    "RuntimeError", "TypeError", "ImportError", "MatchError", NULL};
static bool is_builtin_error(const char *n, size_t len) {
    for (int i = 0; builtin_errors[i]; i++)
        if (strlen(builtin_errors[i]) == len && !memcmp(builtin_errors[i], n, len))
            return true;
    return false;
}
static ObjTypeDesc *read_type(TypeReader *r) {
    VM *vm = r->C->vm;
    const char *s = r->s;
    ObjTypeDesc *t;
    if (s[r->p] == '[') {
        r->p++;
        t = new_type(vm, TY_TUPLE);
        for (;;) {
            type_add_arg(vm, t, read_type(r));
            if (s[r->p] == ',') {
                r->p++;
                continue;
            }
            if (s[r->p] == ']')
                r->p++;
            break;
        }
    } else {
        size_t b = r->p;
        while (s[r->p] && !strchr("<,>[]?", s[r->p]))
            r->p++;
        const char *n = s + b;
        size_t len = r->p - b;
        ObjTypeDesc *args[16];
        int nargs = 0;
        if (s[r->p] == '<') {
            r->p++;
            for (;;) {
                ObjTypeDesc *a = read_type(r);
                if (nargs < 16)
                    args[nargs++] = a;
                if (s[r->p] == ',') {
                    r->p++;
                    continue;
                }
                if (s[r->p] == '>')
                    r->p++;
                break;
            }
        }
#define NM(x) (len == strlen(x) && !memcmp(n, x, len))
        if (NM("function")) {
            t = new_type(vm, TY_FUNCTION);
        } else if (NM("int") || NM("i64")) {
            t = new_type(vm, TY_INT);
            t->bits = 64;
        } else if (NM("i8") || NM("i16") || NM("i32") || NM("u8") || NM("u16") || NM("u32") || NM("u64")) {
            t = new_type(vm, TY_INT);
            t->is_unsigned = n[0] == 'u';
            t->bits = atoi(n + 1);
        } else if (NM("double")) {
            t = new_type(vm, TY_DOUBLE);
        } else if (NM("float")) {
            t = new_type(vm, TY_FLOAT);
        } else if (NM("bool")) {
            t = new_type(vm, TY_BOOL);
        } else if (NM("char")) {
            t = new_type(vm, TY_CHAR);
        } else if (NM("string")) {
            t = new_type(vm, TY_STRING);
        } else if (NM("void")) {
            t = new_type(vm, TY_VOID);
        } else if (NM("element")) {
            t = new_type(vm, TY_ELEMENT);
        } else if (NM("fragment")) {
            t = new_type(vm, TY_FRAGMENT);
        } else if (NM("json.value") || (len > 6 && !memcmp(n + len - 6, ".value", 6) && json_alias(r->C, n, len - 6))) {
            t = new_type(vm, TY_JSON);
        } else if (NM("array") && nargs == 1) {
            t = new_type(vm, TY_ARRAY);
        } else if (NM("tuple") && nargs == 1) {
            t = new_type(vm, TY_HTUPLE);
        } else if (NM("map") && nargs == 2) {
            t = new_type(vm, TY_MAP);
        } else if (NM("set") && nargs == 1) {
            t = new_type(vm, TY_SET);
        } else if (NM("Result")) {
            t = new_type(vm, TY_RESULT);
        } else if (NM("weak")) {
            t = new_type(vm, TY_WEAK);
        } else if (is_builtin_error(n, len)) {
            t = new_type(vm, TY_ERROR);
            t->name = str_intern(vm, n, len);
        } else {
            t = new_type(vm, TY_NAMED);
            t->name = str_intern(vm, n, len);
            resolve_named_type(r->C, t, n, len);
        }
#undef NM
        if (t->kind != TY_FUNCTION && t->kind != TY_NAMED)
            for (int i = 0; i < nargs; i++)
                type_add_arg(vm, t, args[i]);
    }
    for (;;) {
        if (s[r->p] == '?') {
            r->p++;
            ObjTypeDesc *nt = new_type(vm, TY_NULLABLE);
            type_add_arg(vm, nt, t);
            t->convert = type_converts(t);
            t = nt;
        } else if (s[r->p] == '[' && s[r->p + 1] == ']') {
            r->p += 2;
            ObjTypeDesc *nt = new_type(vm, TY_ARRAY);
            t->convert = type_converts(t);
            type_add_arg(vm, nt, t);
            t = nt;
        } else
            break;
    }
    for (int i = 0; i < t->nargs; i++)
        t->args[i]->convert = type_converts(t->args[i]);
    t->convert = type_converts(t);
    return t;
}
static Value make_type(Compiler *C, const char *s) {
    if (!s || !*s)
        return val_null();
    TypeReader r = {C, s, 0};
    ObjTypeDesc *t = read_type(&r);
    return val_obj(t);
}
static bool type_needs_coerce(Value t) { return IS_OBJ(t) && AS_TYPE(t)->convert; }

/* ---------------------------------------------------------------- scopes */

static void begin_scope(Compiler *C) { C->fs->scope_depth++; }
static void pop_locals_to(Compiler *C, int depth, bool emit_code) {
    FuncState *fs = C->fs;
    while (fs->nlocals > 0 && fs->locals[fs->nlocals - 1].depth > depth) {
        if (emit_code)
            emit(C, fs->locals[fs->nlocals - 1].captured ? OP_CLOSE_UPVAL : OP_POP);
        fs->nlocals--;
    }
}
static void end_scope(Compiler *C) {
    C->fs->scope_depth--;
    pop_locals_to(C, C->fs->scope_depth, true);
}
/* Emits the pops for leaving to `depth` without forgetting the locals (break/continue). */
static void emit_pops_to(Compiler *C, int depth) {
    FuncState *fs = C->fs;
    for (int i = fs->nlocals - 1; i >= 0 && fs->locals[i].depth > depth; i--)
        emit(C, fs->locals[i].captured ? OP_CLOSE_UPVAL : OP_POP);
}
static int add_local(Compiler *C, const char *name, Value type, bool is_const) {
    FuncState *fs = C->fs;
    if (fs->nlocals >= MAX_LOCALS)
        cfail(C, C->line, "too many local variables in one function");
    Local *l = &fs->locals[fs->nlocals++];
    l->name = name;
    l->depth = fs->scope_depth;
    l->captured = false;
    l->is_const = is_const;
    l->type = type;
    return fs->nlocals - 1;
}
static int resolve_local(FuncState *fs, const char *name) {
    for (int i = fs->nlocals - 1; i >= 0; i--)
        if (fs->locals[i].name && !strcmp(fs->locals[i].name, name))
            return i;
    return -1;
}
static int add_upval(Compiler *C, FuncState *fs, uint8_t index, bool is_local) {
    int n = fs->fn->upvalue_count;
    for (int i = 0; i < n; i++)
        if (fs->upvals[i].index == index && fs->upvals[i].is_local == is_local)
            return i;
    if (n >= MAX_UPVALS)
        cfail(C, C->line, "too many captured variables in one closure");
    fs->upvals[n].index = index;
    fs->upvals[n].is_local = is_local;
    return fs->fn->upvalue_count++;
}
static int resolve_upval(Compiler *C, FuncState *fs, const char *name, Local **info) {
    if (!fs->enclosing)
        return -1;
    int l = resolve_local(fs->enclosing, name);
    if (l >= 0) {
        fs->enclosing->locals[l].captured = true;
        if (info)
            *info = &fs->enclosing->locals[l];
        return add_upval(C, fs, (uint8_t)l, true);
    }
    int u = resolve_upval(C, fs->enclosing, name, info);
    if (u >= 0)
        return add_upval(C, fs, (uint8_t)u, false);
    return -1;
}

/* ---------------------------------------------------------------- name resolution */

typedef enum { R_NONE, R_LOCAL, R_UPVAL, R_GLOBAL, R_MGLOBAL, R_CONST } RKind;
typedef struct {
    RKind kind;
    int index;
    ObjModule *module;
    Value value;
    Local *local;
} Resolved;

static const char *strip_type_args(Compiler *C, const char *name) {
    const char *lt = strchr(name, '<');
    if (!lt)
        return name;
    return arena_strndup(C->arena, name, (size_t)(lt - name));
}
static ObjModule *find_alias(Compiler *C, const char *name) {
    for (int i = C->naliases - 1; i >= 0; i--)
        if (!strcmp(C->aliases[i].alias, name))
            return C->aliases[i].module;
    return NULL;
}
static Resolved resolve(Compiler *C, const char *name) {
    Resolved r;
    memset(&r, 0, sizeof r);
    int l = resolve_local(C->fs, name);
    if (l >= 0) {
        r.kind = R_LOCAL;
        r.index = l;
        r.local = &C->fs->locals[l];
        return r;
    }
    Local *info = NULL;
    int u = resolve_upval(C, C->fs, name, &info);
    if (u >= 0) {
        r.kind = R_UPVAL;
        r.index = u;
        r.local = info;
        return r;
    }
    ObjString *s = str_intern_c(C->vm, name);
    int g = module_find(C->module, s);
    if (g >= 0) {
        r.kind = R_GLOBAL;
        r.index = g;
        return r;
    }
    ObjModule *m = find_alias(C, name);
    if (m) {
        r.kind = R_CONST;
        r.value = val_obj(m);
        return r;
    }
    for (int i = C->nwilds - 1; i >= 0; i--)
        if (!strcmp(C->wilds[i].name, name)) {
            if (C->wilds[i].module) {
                r.kind = R_MGLOBAL;
                r.module = C->wilds[i].module;
                r.index = C->wilds[i].slot;
            } else {
                r.kind = R_CONST;
                r.value = C->wilds[i].value;
            }
            return r;
        }
    Value v;
    if (table_get(&C->vm->builtins, s, &v)) {
        r.kind = R_CONST;
        r.value = v;
        return r;
    }
    r.kind = R_NONE;
    return r;
}
static int find_wild_slot(Compiler *C, const char *name, ObjModule **m) {
    for (int i = C->nwilds - 1; i >= 0; i--)
        if (C->wilds[i].module && !strcmp(C->wilds[i].name, name)) {
            *m = C->wilds[i].module;
            return C->wilds[i].slot;
        }
    return -1;
}
static void emit_get_resolved(Compiler *C, Resolved *r) {
    switch (r->kind) {
    case R_LOCAL:
        emit2(C, OP_GET_LOCAL, (uint8_t)r->index);
        break;
    case R_UPVAL:
        emit2(C, OP_GET_UPVAL, (uint8_t)r->index);
        break;
    case R_GLOBAL:
        emit(C, OP_GET_GLOBAL);
        emit16(C, r->index);
        break;
    case R_MGLOBAL:
        emit(C, OP_GET_MGLOBAL);
        emit16(C, make_const(C, val_obj(r->module)));
        emit16(C, r->index);
        break;
    case R_CONST:
        emit_const(C, r->value);
        break;
    default:
        break;
    }
}
static void compile_name(Compiler *C, const char *raw, int line) {
    const char *name = strip_type_args(C, raw);
    if (!strcmp(name, "inst"))
        cfail(C, line, "'inst' is only valid inside methods");
    Resolved r = resolve(C, name);
    if (r.kind == R_NONE) {
        if (strchr(raw, '<')) {
            /* generic builtin casts such as array<int> and map<string, int> */
            Value t = make_type(C, raw);
            ObjString *base = str_intern_c(C->vm, name);
            Value cast;
            if (table_get(&C->vm->builtins, base, &cast) && IS_OBJT(cast, OBJ_NATIVE)) {
                ObjNative *n = new_native(C->vm, AS_CSTR(val_obj(base)), ((ObjNative *)AS_OBJ(cast))->fn, 0, 1);
                n->data = t;
                emit_const(C, val_obj(n));
                return;
            }
        }
        cfail(C, line, "undeclared identifier '%s'", name);
    }
    if (r.kind == R_CONST && strchr(raw, '<') && IS_OBJT(r.value, OBJ_NATIVE) &&
        ((ObjNative *)AS_OBJ(r.value))->max_args <= 1 && IS_OBJT(((ObjNative *)AS_OBJ(r.value))->data, OBJ_TYPE)) {
        Value t = make_type(C, raw);
        ObjNative *orig = (ObjNative *)AS_OBJ(r.value);
        ObjNative *n = new_native(C->vm, orig->name, orig->fn, 0, 1);
        n->data = t;
        emit_const(C, val_obj(n));
        return;
    }
    emit_get_resolved(C, &r);
}

/* ---------------------------------------------------------------- expressions */

static void compile_expr(Compiler *C, Node *e);
static void compile_block(Compiler *C, NodeList *body);
static void compile_stmt(Compiler *C, Node *s);
static ObjFunction *compile_function(Compiler *C, FuncDecl *f, bool method, bool lambda,
                                     bool component, ObjClass *owner);

static bool is_range(Node *e) {
    return e->kind == N_BINARY && !e->grouped && (!strcmp(e->text, "..") || !strcmp(e->text, "..<"));
}
static uint8_t binary_op(const char *op) {
    static const struct {
        const char *s;
        uint8_t op;
    } ops[] = {{"+", OP_ADD}, {"-", OP_SUB}, {"*", OP_MUL}, {"/", OP_DIV}, {"//", OP_IDIV},
               {"%", OP_MOD}, {"==", OP_EQ}, {"!=", OP_NE}, {"<", OP_LT}, {"<=", OP_LE},
               {">", OP_GT},  {">=", OP_GE}, {"in", OP_IN}, {NULL, 0}};
    for (int i = 0; ops[i].s; i++)
        if (!strcmp(ops[i].s, op))
            return ops[i].op;
    return 0;
}

static void compile_args(Compiler *C, Node *call, int from) {
    int argc = call->args.count - from;
    if (argc > 255)
        cfail(C, call->line, "too many arguments");
    for (int i = from; i < call->args.count; i++)
        compile_expr(C, call->args.items[i]);
}

static bool member_on_module(Compiler *C, Node *m, ObjModule **out) {
    if (m->kind != N_MEMBER || m->args.items[0]->kind != N_NAME)
        return false;
    const char *name = m->args.items[0]->text;
    if (resolve_local(C->fs, name) >= 0)
        return false;
    Local *info;
    (void)info;
    /* captured locals shadow modules too */
    for (FuncState *fs = C->fs->enclosing; fs; fs = fs->enclosing)
        if (resolve_local(fs, name) >= 0)
            return false;
    if (module_find(C->module, str_intern_c(C->vm, name)) >= 0)
        return false;
    ObjModule *mod = find_alias(C, name);
    if (!mod)
        return false;
    *out = mod;
    return true;
}
static void compile_module_member(Compiler *C, ObjModule *mod, const char *raw, int line) {
    const char *member = strip_type_args(C, raw);
    ObjString *s = str_intern_c(C->vm, member);
    int slot = module_find(mod, s);
    if (slot < 0)
        cfail(C, line, "module '%s' has no member '%s'", mod->name->chars, member);
    if (mod->is_private && mod->is_private[slot])
        cfail(C, line, "'%s' is private to module '%s'", member, mod->name->chars);
    if (mod->native) {
        Value v = mod->values.items[slot];
        if (strchr(raw, '<') && IS_OBJT(v, OBJ_NATIVE)) {
            ObjNative *orig = (ObjNative *)AS_OBJ(v);
            ObjNative *n = new_native(C->vm, orig->name, orig->fn, orig->min_args, orig->max_args);
            n->data = make_type(C, strchr(raw, '<') + 1);
            emit_const(C, val_obj(n));
            return;
        }
        emit_const(C, v);
        return;
    }
    emit(C, OP_GET_MGLOBAL);
    emit16(C, make_const(C, val_obj(mod)));
    emit16(C, slot);
}

static int receiver_slot(Compiler *C, int line) {
    Resolved r = resolve(C, "inst");
    if (r.kind == R_LOCAL)
        return r.index;
    if (r.kind == R_UPVAL)
        return 1000 + r.index;
    cfail(C, line, "'inst' is only valid inside methods");
}
static void emit_receiver(Compiler *C, int line) {
    int s = receiver_slot(C, line);
    if (s >= 1000)
        emit2(C, OP_GET_UPVAL, (uint8_t)(s - 1000));
    else
        emit2(C, OP_GET_LOCAL, (uint8_t)s);
}

static void compile_call(Compiler *C, Node *e) {
    Node *callee = e->args.items[0];
    int argc = e->args.count - 1;
    ObjModule *mod;
    if (callee->kind == N_MEMBER && !callee->grouped && callee->args.items[0]->kind == N_SUPER) {
        emit_receiver(C, e->line);
        compile_args(C, e, 1);
        emit(C, OP_SUPER_INVOKE);
        emit16(C, name_const(C, strip_type_args(C, callee->text)));
        emit(C, (uint8_t)argc);
        return;
    }
    if (callee->kind == N_MEMBER && !callee->grouped && !member_on_module(C, callee, &mod)) {
        compile_expr(C, callee->args.items[0]);
        compile_args(C, e, 1);
        emit(C, OP_INVOKE);
        emit16(C, name_const(C, strip_type_args(C, callee->text)));
        emit(C, (uint8_t)argc);
        emit16(C, new_cache(C));
        return;
    }
    if (callee->kind == N_INST && !callee->grouped) {
        emit_receiver(C, e->line);
        compile_args(C, e, 1);
        emit(C, OP_INVOKE);
        emit16(C, name_const(C, callee->text));
        emit(C, (uint8_t)argc);
        emit16(C, new_cache(C));
        return;
    }
    compile_expr(C, callee);
    compile_args(C, e, 1);
    emit2(C, OP_CALL, (uint8_t)argc);
}

static bool find_component(Compiler *C, const char *tag, Resolved *out) {
    for (int i = 0; i < C->prog->ncomponents; i++)
        if (!strcmp(C->prog->components[i].name, tag)) {
            out->kind = R_GLOBAL;
            out->index = module_find(C->module, str_intern_c(C->vm, tag));
            return true;
        }
    for (int i = C->nwilds - 1; i >= 0; i--)
        if (C->wilds[i].component && !strcmp(C->wilds[i].name, tag)) {
            out->kind = R_MGLOBAL;
            out->module = C->wilds[i].module;
            out->index = C->wilds[i].slot;
            return true;
        }
    return false;
}
static bool known_tag(Compiler *C, const char *tag) {
    if (C->prog->html || C->prog->svg || C->module->html)
        return true;
    for (int i = 0; i < C->prog->nfamilies; i++)
        if (!strcmp(C->prog->families[i], tag))
            return true;
    return false;
}
static void compile_children(Compiler *C, Node *e, int from) {
    for (int i = from; i < e->args.count; i++)
        compile_expr(C, e->args.items[i]);
}
static void compile_element(Compiler *C, Node *e) {
    const char *tag = e->text;
    int nchildren = e->args.count - e->nnames;
    Resolved comp;
    if (tag[0] && find_component(C, tag, &comp)) {
        emit_get_resolved(C, &comp);
        for (int i = 0; i < e->nnames; i++) {
            emit_const(C, val_obj(str_intern_c(C->vm, e->names[i])));
            Node *v = e->args.items[i];
            if (v->kind == N_STRING && v->valueless)
                emit(C, OP_TRUE);
            else
                compile_expr(C, v);
        }
        emit_const(C, val_obj(str_intern_c(C->vm, "inner")));
        compile_children(C, e, e->nnames);
        emit(C, OP_ELEMENT);
        emit16(C, make_const(C, val_null()));
        emit16(C, make_const(C, val_obj(new_list(C->vm, OBJ_ARRAY, 0))));
        emit16(C, nchildren);
        emit(C, OP_MAP);
        emit16(C, e->nnames + 1);
        emit2(C, OP_CALL, 1);
        return;
    }
    if (tag[0] && !known_tag(C, tag))
        cfail(C, e->line, "unknown tag <%s>; import html.* (or svg.*) or declare a component", tag);
    ObjList *names = new_list(C->vm, OBJ_ARRAY, e->nnames);
    for (int i = 0; i < e->nnames; i++) {
        list_push(C->vm, names, val_obj(str_intern_c(C->vm, e->names[i])));
        Node *v = e->args.items[i];
        if (v->kind == N_STRING && v->valueless)
            emit_const(C, val_obj(str_intern_c(C->vm, "")));
        else
            compile_expr(C, v);
    }
    compile_children(C, e, e->nnames);
    emit(C, OP_ELEMENT);
    emit16(C, make_const(C, tag[0] ? val_obj(str_intern_c(C->vm, tag)) : val_null()));
    emit16(C, make_const(C, val_obj(names)));
    emit16(C, nchildren);
}

static void compile_lambda(Compiler *C, Node *e) {
    ObjFunction *fn = compile_function(C, e->fn, false, true, false, NULL);
    (void)fn;
}

/* `T x = array(v)` (also tuple, map, set): an explicit cast without type arguments uses the
 * expected type. */
static bool compile_cast_with_type(Compiler *C, Node *e, Value type) {
    if (!IS_OBJ(type) || e->kind != N_CALL || e->args.count != 2)
        return false;
    Node *callee = e->args.items[0];
    if (callee->kind != N_NAME || callee->grouped || strchr(callee->text, '<'))
        return false;
    const char *n = callee->text;
    ObjTypeDesc *t = AS_TYPE(type);
    if (t->kind == TY_NULLABLE)
        t = t->args[0];
    bool fits = (!strcmp(n, "array") && t->kind == TY_ARRAY) ||
                (!strcmp(n, "tuple") && (t->kind == TY_TUPLE || t->kind == TY_HTUPLE)) ||
                (!strcmp(n, "map") && t->kind == TY_MAP) || (!strcmp(n, "set") && t->kind == TY_SET);
    if (!fits)
        return false;
    Resolved r = resolve(C, n);
    if (r.kind != R_CONST || !IS_OBJT(r.value, OBJ_NATIVE))
        return false;
    ObjNative *orig = (ObjNative *)AS_OBJ(r.value);
    ObjNative *cast = new_native(C->vm, orig->name, orig->fn, 0, 1);
    cast->data = val_obj(t);
    emit_const(C, val_obj(cast));
    compile_expr(C, e->args.items[1]);
    emit2(C, OP_CALL, 1);
    return true;
}

/* ---------------------------------------------------------------- units (see lower.c) */

static UnitNs *unit_namespace(Compiler *C, const char *name) {
    for (int i = 0; i < C->prog->nunitns; i++)
        if (!strcmp(C->prog->unitns[i].name, name))
            return &C->prog->unitns[i];
    return NULL;
}
static int unit_slot(UnitNs *ns, const char *unit) {
    for (int i = 0; i < ns->nunits; i++)
        if (!strcmp(ns->units[i], unit))
            return i;
    return -1;
}
static Node *synth(Compiler *C, NodeKind kind, const char *text, Node *at) {
    Node *n = arena_alloc(C->arena, sizeof(Node));
    n->kind = kind;
    n->text = text;
    n->line = at->line;
    n->col = at->col;
    n->len = -1;
    return n;
}
/* 100m, 0.32km, 15time.s: the namespace comes from the spelling, the declared type, or the only
 * namespace with the unit. The literal constructs the namespace's hidden class. */
static void compile_quantity(Compiler *C, Node *e) {
    char unit[128], member[128] = "";
    snprintf(unit, sizeof unit, "%s", e->text);
    UnitNs *ns = NULL;
    char *dot = strchr(unit, '.');
    if (dot) {
        *dot = 0;
        ns = unit_namespace(C, unit);
        if (ns)
            memmove(unit, dot + 1, strlen(dot + 1) + 1);
        else
            snprintf(member, sizeof member, "%s", dot + 1); /* 15s.value */
        if (ns && unit_slot(ns, unit) < 0)
            cfail(C, e->line, "unit namespace '%s' has no unit '%s'", ns->name, unit);
    }
    /* the declared type: int[ns], int[ns]?, or int[ns][] */
    char hint_ns[96] = "";
    bool hint_real = false;
    if (C->unit_hint) {
        char base[160];
        snprintf(base, sizeof base, "%s", C->unit_hint);
        char *cut = strpbrk(base, "?[");
        if (cut)
            *cut = 0;
        if (!quantity_class_name(base, hint_ns, sizeof hint_ns, &hint_real))
            hint_ns[0] = 0;
    }
    UnitNs *wanted = hint_ns[0] ? unit_namespace(C, hint_ns) : NULL;
    if (!ns && wanted && unit_slot(wanted, unit) >= 0)
        ns = wanted;
    if (!ns) {
        UnitNs *found[2] = {NULL, NULL};
        int n = 0;
        for (int i = 0; i < C->prog->nunitns; i++)
            if (unit_slot(&C->prog->unitns[i], unit) >= 0 && n < 2)
                found[n++] = &C->prog->unitns[i];
        if (!n)
            cfail(C, e->line, "unknown unit '%s' after %s%s", unit, e->args.items[0]->text,
                  C->prog->nunitns ? "" : "; declare units with unitns and unit, such as `unitns length for int, "
                                          "float` and `unit length.m = 1`");
        if (n > 1)
            cfail(C, e->line, "unit '%s' is ambiguous: %s and %s both have it; write %s%s.%s", unit, found[0]->name,
                  found[1]->name, e->args.items[0]->text, found[0]->name, unit);
        ns = found[0];
    }
    Node *number = e->args.items[0];
    bool real = number->kind == N_REAL || !ns->integer || (wanted == ns && hint_real);
    if (real && !ns->real)
        cfail(C, e->line, "int[%s] cannot hold %s; declare unitns %s for int, float", ns->name, number->text,
              ns->name);
    char cls[160];
    snprintf(cls, sizeof cls, "__fwu_%s__%s", ns->name, real ? "double" : "int");
    Node *call = synth(C, N_CALL, "", e);
    nl_push(C->arena, &call->args, synth(C, N_NAME, arena_strndup(C->arena, cls, strlen(cls)), e));
    Node *amount = number;
    if (real && number->kind == N_INT) {
        char text[96];
        snprintf(text, sizeof text, "%s.0", number->text);
        amount = synth(C, N_REAL, arena_strndup(C->arena, text, strlen(text)), e);
    }
    nl_push(C->arena, &call->args, amount);
    char index[16];
    snprintf(index, sizeof index, "%d", unit_slot(ns, unit));
    nl_push(C->arena, &call->args, synth(C, N_INT, arena_strndup(C->arena, index, strlen(index)), e));
    if (!member[0]) {
        compile_expr(C, call);
        return;
    }
    Node *access = synth(C, N_MEMBER, arena_strndup(C->arena, member, strlen(member)), e);
    nl_push(C->arena, &access->args, call);
    compile_expr(C, access);
}
/* `x in cm` / `x in time.min` when the right side names a unit rather than a value: the value
 * converted to that unit (its __fwu_to_named method). */
static const char *conversion_unit(Compiler *C, Node *target) {
    if (!C->prog->nunitns)
        return NULL;
    if (target->kind == N_NAME && resolve(C, target->text).kind == R_NONE) {
        for (int i = 0; i < C->prog->nunitns; i++)
            if (unit_slot(&C->prog->unitns[i], target->text) >= 0)
                return target->text;
        return NULL;
    }
    if (target->kind == N_MEMBER && !target->grouped && target->args.items[0]->kind == N_NAME) {
        UnitNs *ns = unit_namespace(C, target->args.items[0]->text);
        Resolved r = resolve(C, target->args.items[0]->text);
        if (ns && (r.kind == R_NONE || (r.kind == R_CONST && IS_OBJT(r.value, OBJ_MODULE)))) {
            if (unit_slot(ns, target->text) < 0)
                cfail(C, target->line, "unit namespace '%s' has no unit '%s'", ns->name, target->text);
            return target->text;
        }
    }
    return NULL;
}

static void compile_expr(Compiler *C, Node *e) {
    int saved_line = C->line, saved_col = C->col;
    C->line = e->line;
    C->col = e->col;
    switch (e->kind) {
    case N_INT: {
        errno = 0;
        char *end;
        unsigned long long v = strtoull(e->text, &end, 10);
        if (errno == ERANGE)
            cfail(C, e->line, "integer literal %s is too large", e->text);
        if (v > (unsigned long long)INT64_MAX) {
            /* u64 literals beyond the signed range are kept as doubles */
            emit_const(C, val_double((double)v));
        } else
            emit_int(C, (int64_t)v);
        break;
    }
    case N_REAL:
        emit_const(C, val_double(strtod(e->text, NULL)));
        break;
    case N_STRING:
        emit_const(C, val_obj(str_intern(C->vm, e->text, e->len >= 0 ? (size_t)e->len : strlen(e->text))));
        break;
    case N_CHAR: {
        int adv;
        uint32_t cp = utf8_decode(e->text, (int)strlen(e->text), &adv);
        emit_const(C, val_char(cp));
        break;
    }
    case N_BOOL:
        emit(C, !strcmp(e->text, "true") ? OP_TRUE : OP_FALSE);
        break;
    case N_NULL:
        emit(C, OP_NULL);
        break;
    case N_INTERP: {
        if (e->args.count > 65535)
            cfail(C, e->line, "string has too many parts");
        for (int i = 0; i < e->args.count; i++)
            compile_expr(C, e->args.items[i]);
        emit(C, OP_STRING);
        emit16(C, e->args.count);
        break;
    }
    case N_FORMAT:
        compile_expr(C, e->args.items[0]);
        emit(C, OP_FORMAT);
        emit16(C, make_const(C, val_obj(str_intern_c(C->vm, e->text))));
        break;
    case N_NAME:
        compile_name(C, e->text, e->line);
        break;
    case N_MEMBER: {
        ObjModule *mod;
        Node *obj = e->args.items[0];
        if (member_on_module(C, e, &mod)) {
            compile_module_member(C, mod, e->text, e->line);
            break;
        }
        if (obj->kind == N_SUPER) {
            emit_receiver(C, e->line);
            emit(C, OP_SUPER_GET);
            emit16(C, name_const(C, strip_type_args(C, e->text)));
            break;
        }
        compile_expr(C, obj);
        emit(C, OP_GET_MEMBER);
        emit16(C, name_const(C, strip_type_args(C, e->text)));
        emit16(C, new_cache(C));
        break;
    }
    case N_INST:
        emit_receiver(C, e->line);
        emit(C, OP_GET_MEMBER);
        emit16(C, name_const(C, e->text));
        emit16(C, new_cache(C));
        break;
    case N_ATTR: {
        if (!C->fs->is_component)
            cfail(C, e->line, "'attr' is only valid in component templates");
        Resolved r = resolve(C, "$attrs");
        emit_get_resolved(C, &r);
        emit(C, OP_ATTR);
        emit16(C, name_const(C, e->text));
        break;
    }
    case N_SUPER:
        cfail(C, e->line, "'super' must be followed by a method, as in super.init(...)");
    case N_CALL:
        compile_call(C, e);
        break;
    case N_UNITLIT:
        compile_quantity(C, e);
        break;
    case N_INDEX: {
        Node *key = e->args.items[1];
        compile_expr(C, e->args.items[0]);
        if (is_range(key)) {
            compile_expr(C, key->args.items[0]);
            compile_expr(C, key->args.items[1]);
            emit2(C, OP_SLICE, !strcmp(key->text, "..") ? 1 : 0);
        } else {
            compile_expr(C, key);
            emit(C, OP_GET_INDEX);
        }
        break;
    }
    case N_UNARY:
        if (!strcmp(e->text, "-") && e->args.items[0]->kind == N_INT && !e->args.items[0]->grouped) {
            unsigned long long v = strtoull(e->args.items[0]->text, NULL, 10);
            if (v <= (unsigned long long)INT64_MAX + 1ull) {
                emit_int(C, v == (unsigned long long)INT64_MAX + 1ull ? INT64_MIN : -(int64_t)v);
                break;
            }
        }
        compile_expr(C, e->args.items[0]);
        emit(C, !strcmp(e->text, "-") ? OP_NEG : !strcmp(e->text, "+") ? OP_POS : OP_NOT);
        break;
    case N_BINARY: {
        const char *op = e->text;
        if (!strcmp(op, "and") || !strcmp(op, "or")) {
            compile_expr(C, e->args.items[0]);
            int j = emit_jump(C, !strcmp(op, "and") ? OP_AND : OP_OR);
            compile_expr(C, e->args.items[1]);
            patch_jump(C, j);
            break;
        }
        if (!strcmp(op, "??")) {
            compile_expr(C, e->args.items[0]);
            int j = emit_jump(C, OP_JUMP_NOT_NULL);
            compile_expr(C, e->args.items[1]);
            patch_jump(C, j);
            break;
        }
        const char *unit = !strcmp(op, "in") ? conversion_unit(C, e->args.items[1]) : NULL;
        if (unit) {
            Node *call = synth(C, N_CALL, "", e);
            Node *method = synth(C, N_MEMBER, "__fwu_to_named", e);
            nl_push(C->arena, &method->args, e->args.items[0]);
            nl_push(C->arena, &call->args, method);
            nl_push(C->arena, &call->args, synth(C, N_STRING, unit, e));
            compile_call(C, call);
            break;
        }
        compile_expr(C, e->args.items[0]);
        compile_expr(C, e->args.items[1]);
        if (!strcmp(op, "..") || !strcmp(op, "..<")) {
            emit2(C, OP_RANGE, !strcmp(op, "..") ? 1 : 0);
            break;
        }
        uint8_t code = binary_op(op);
        if (!code)
            cfail(C, e->line, "unknown operator '%s'", op);
        emit(C, code);
        break;
    }
    case N_CHAIN: {
        /* a < b <= c: [a b] CHAIN -> [b bool]; stop at the first false. */
        int fails[64], nf = 0;
        compile_expr(C, e->args.items[0]);
        for (int i = 1; i < e->args.count; i++) {
            compile_expr(C, e->args.items[i]);
            uint8_t code = binary_op(e->names[i - 1]);
            if (i == e->args.count - 1) {
                emit(C, code);
            } else {
                emit2(C, OP_CHAIN, code);
                if (nf == 64)
                    cfail(C, e->line, "comparison chain is too long");
                fails[nf++] = emit_jump(C, OP_AND); /* true: pops the bool, leaving [b] */
            }
        }
        if (nf) {
            int done = emit_jump(C, OP_JUMP);
            for (int i = 0; i < nf; i++)
                patch_jump(C, fails[i]);
            /* [b false] -> [false] */
            emit(C, OP_SWAP);
            emit(C, OP_POP);
            patch_jump(C, done);
        }
        break;
    }
    case N_IS:
        compile_expr(C, e->args.items[0]);
        emit(C, OP_IS);
        emit16(C, make_const(C, make_type(C, e->text)));
        break;
    case N_AS:
        compile_expr(C, e->args.items[0]);
        emit(C, OP_AS);
        emit16(C, make_const(C, make_type(C, e->text)));
        break;
    case N_ARRAY:
        if (e->args.count > 65535)
            cfail(C, e->line, "array literal too large");
        for (int i = 0; i < e->args.count; i++)
            compile_expr(C, e->args.items[i]);
        emit(C, OP_ARRAY);
        emit16(C, e->args.count);
        break;
    case N_MAP:
        for (int i = 0; i < e->args.count; i++)
            compile_expr(C, e->args.items[i]);
        emit(C, OP_MAP);
        emit16(C, e->args.count / 2);
        break;
    case N_SET:
        for (int i = 0; i < e->args.count; i++)
            compile_expr(C, e->args.items[i]);
        emit(C, OP_SET);
        emit16(C, e->args.count);
        break;
    case N_LAMBDA:
        compile_lambda(C, e);
        break;
    case N_ELEMENT:
        compile_element(C, e);
        break;
    case N_JSON:
        emit(C, OP_JSON);
        emit16(C, make_const(C, val_obj(str_intern_c(C->vm, e->text))));
        break;
    default:
        cfail(C, e->line, "unsupported expression");
    }
    C->line = saved_line;
    C->col = saved_col;
}

/* ---------------------------------------------------------------- try / exits */

/* Runs pending finalizers (innermost first) for contexts at index >= stop. */
static void unwind_tries(Compiler *C, int stop) {
    FuncState *fs = C->fs;
    int saved = fs->ntries;
    TryCtx copy[64];
    memcpy(copy, fs->tries, sizeof(TryCtx) * (size_t)saved);
    for (int i = saved - 1; i >= stop; i--) {
        for (int h = 0; h < copy[i].handlers; h++)
            emit(C, OP_POP_HANDLER);
        if (copy[i].finalizer) {
            fs->ntries = i; /* exits inside the finalizer see only outer contexts */
            begin_scope(C);
            compile_block(C, copy[i].finalizer);
            end_scope(C);
        }
    }
    fs->ntries = saved;
    memcpy(fs->tries, copy, sizeof(TryCtx) * (size_t)saved);
}

static void compile_return(Compiler *C, Node *s) {
    FuncState *fs = C->fs;
    if (s->expr) {
        if (!compile_cast_with_type(C, s->expr, fs->ret_type))
            compile_expr(C, s->expr);
    } else
        emit(C, OP_NULL);
    if (s->expr && type_needs_coerce(fs->ret_type)) {
        emit(C, OP_COERCE);
        emit16(C, make_const(C, fs->ret_type));
        emit16(C, make_const(C, val_obj(str_intern_c(C->vm, "return value"))));
    }
    if (fs->ntries) {
        begin_scope(C);
        add_local(C, "$ret", val_null(), true);
        unwind_tries(C, 0);
        fs->scope_depth--;
        fs->nlocals--; /* the value stays on the stack for OP_RETURN */
    }
    emit(C, OP_RETURN);
}

static void compile_try(Compiler *C, Node *s) {
    FuncState *fs = C->fs;
    bool has_fin = s->has_else, has_catch = s->ncatches > 0;
    if (fs->ntries >= 64)
        cfail(C, s->line, "try statements nested too deeply");
    int ctx = fs->ntries++;
    fs->tries[ctx].finalizer = has_fin ? &s->finalizer : NULL;
    fs->tries[ctx].handlers = 0;
    fs->tries[ctx].loop_count = fs->loop_count;
    int fin_handler = -1, catch_handler = -1;
    if (has_fin) {
        fin_handler = emit_jump(C, OP_TRY);
        fs->tries[ctx].handlers++;
    }
    if (has_catch) {
        catch_handler = emit_jump(C, OP_TRY);
        fs->tries[ctx].handlers++;
    }
    begin_scope(C);
    compile_block(C, &s->body);
    end_scope(C);
    if (has_catch) {
        emit(C, OP_POP_HANDLER);
        fs->tries[ctx].handlers--;
        int skip = emit_jump(C, OP_JUMP);
        patch_jump(C, catch_handler);
        begin_scope(C);
        int err = add_local(C, "$err", val_null(), true);
        int ends[256], nends = 0;
        for (int i = 0; i < s->ncatches && i < 256; i++) {
            const char *t = s->catch_types[i];
            bool all = !strcmp(t, "Error") || !strcmp(t, "\"Error\"");
            int next = -1;
            if (!all) {
                emit2(C, OP_GET_LOCAL, (uint8_t)err);
                emit(C, OP_CATCH_MATCH);
                if (t[0] == '"')
                    emit16(C, make_const(C, val_obj(str_intern_c(C->vm, arena_strndup(C->arena, t + 1, strlen(t) - 2)))));
                else
                    emit16(C, make_const(C, make_type(C, t)));
                next = emit_jump(C, OP_JUMP_IF_FALSE);
            }
            begin_scope(C);
            emit2(C, OP_GET_LOCAL, (uint8_t)err);
            add_local(C, s->catch_names[i], val_null(), false);
            compile_block(C, &s->catches[i]);
            end_scope(C);
            ends[nends++] = emit_jump(C, OP_JUMP);
            if (next >= 0)
                patch_jump(C, next);
        }
        emit2(C, OP_GET_LOCAL, (uint8_t)err);
        emit(C, OP_THROW);
        for (int i = 0; i < nends; i++)
            patch_jump(C, ends[i]);
        end_scope(C);
        patch_jump(C, skip);
    }
    fs->ntries--;
    if (has_fin) {
        emit(C, OP_POP_HANDLER);
        begin_scope(C);
        compile_block(C, &s->finalizer);
        end_scope(C);
        int end = emit_jump(C, OP_JUMP);
        patch_jump(C, fin_handler);
        begin_scope(C);
        int ferr = add_local(C, "$ferr", val_null(), true);
        begin_scope(C);
        compile_block(C, &s->finalizer);
        end_scope(C);
        emit2(C, OP_GET_LOCAL, (uint8_t)ferr);
        emit(C, OP_THROW);
        fs->scope_depth--;
        fs->nlocals--;
        patch_jump(C, end);
    }
}

static void compile_break(Compiler *C, Node *s, bool is_break) {
    FuncState *fs = C->fs;
    if (!fs->loop_count)
        cfail(C, s->line, "'%s' outside a loop", is_break ? "break" : "continue");
    Loop *loop = &fs->loops[fs->loop_count - 1];
    int stop = fs->ntries;
    while (stop > 0 && fs->tries[stop - 1].loop_count >= fs->loop_count)
        stop--;
    unwind_tries(C, stop);
    emit_pops_to(C, loop->depth);
    if (is_break) {
        if (loop->nbreaks == loop->capbreaks) {
            loop->capbreaks = GROW_CAP(loop->capbreaks);
            loop->breaks = xrealloc(loop->breaks, sizeof(int) * (size_t)loop->capbreaks);
        }
        loop->breaks[loop->nbreaks++] = emit_jump(C, OP_JUMP);
    } else
        emit_loop(C, loop->start);
}
static Loop *push_loop(Compiler *C, int start) {
    FuncState *fs = C->fs;
    if (fs->loop_count >= 64)
        cfail(C, C->line, "loops nested too deeply");
    Loop *l = &fs->loops[fs->loop_count++];
    l->start = start;
    l->depth = fs->scope_depth;
    l->breaks = NULL;
    l->nbreaks = l->capbreaks = 0;
    return l;
}
static void pop_loop(Compiler *C) {
    Loop *l = &C->fs->loops[--C->fs->loop_count];
    for (int i = 0; i < l->nbreaks; i++)
        patch_jump(C, l->breaks[i]);
    free(l->breaks);
}

/* ---------------------------------------------------------------- match */

static bool is_destructure_call(Node *p) {
    if (p->kind != N_CALL || p->grouped)
        return false;
    Node *c = p->args.items[0];
    if (c->kind == N_MEMBER)
        return true;
    return c->kind == N_NAME && isupper((unsigned char)c->text[0]);
}
static bool bare_name(Node *p) {
    return p->kind == N_NAME && !p->grouped && !strchr(p->text, '<') && strcmp(p->text, "_") != 0;
}
/* Allocates binding locals and temporaries for nested positions. */
static void prealloc_pattern(Compiler *C, Node *p, bool nested) {
    if (nested && bare_name(p)) {
        emit(C, OP_NULL);
        p->slot = add_local(C, p->text, val_null(), false);
        return;
    }
    if (is_destructure_call(p)) {
        for (int i = 1; i < p->args.count; i++) {
            Node *a = p->args.items[i];
            if (a->kind == N_NAME && !strcmp(a->text, "_"))
                continue;
            if (!bare_name(a)) {
                emit(C, OP_NULL);
                a->slot = add_local(C, "$pt", val_null(), false);
            }
            prealloc_pattern(C, a, true);
        }
    } else if (p->kind == N_ARRAY) {
        for (int i = 0; i < p->args.count; i++) {
            Node *a = p->args.items[i];
            if (a->kind == N_NAME && !strcmp(a->text, "_"))
                continue;
            if (!bare_name(a)) {
                emit(C, OP_NULL);
                a->slot = add_local(C, "$pt", val_null(), false);
            }
            prealloc_pattern(C, a, true);
        }
    }
}
typedef struct {
    int *items;
    int count, cap;
} Jumps;
static void jumps_add(Jumps *j, int at) {
    if (j->count == j->cap) {
        j->cap = GROW_CAP(j->cap);
        j->items = xrealloc(j->items, sizeof(int) * (size_t)j->cap);
    }
    j->items[j->count++] = at;
}
static void compile_pattern(Compiler *C, Node *p, int subj, bool nested, Jumps *fails) {
    if (p->kind == N_NAME && !strcmp(p->text, "_"))
        return;
    if (nested && bare_name(p)) {
        emit2(C, OP_GET_LOCAL, (uint8_t)subj);
        emit2(C, OP_SET_LOCAL, (uint8_t)p->slot);
        return;
    }
    if (is_destructure_call(p) || p->kind == N_ARRAY) {
        bool call = p->kind == N_CALL;
        int from = call ? 1 : 0;
        emit2(C, OP_GET_LOCAL, (uint8_t)subj);
        if (call) {
            compile_expr(C, p->args.items[0]);
            emit(C, OP_MATCH_VALUE);
        } else {
            emit(C, OP_LEN_IS);
            emit16(C, p->args.count);
            emit(C, 0);
        }
        jumps_add(fails, emit_jump(C, OP_JUMP_IF_FALSE));
        for (int i = from; i < p->args.count; i++) {
            Node *a = p->args.items[i];
            if (a->kind == N_NAME && !strcmp(a->text, "_"))
                continue;
            emit2(C, OP_GET_LOCAL, (uint8_t)subj);
            if (call)
                emit2(C, OP_FIELD_AT, (uint8_t)(i - 1));
            else {
                emit_int(C, i);
                emit(C, OP_GET_INDEX);
            }
            emit2(C, OP_SET_LOCAL, (uint8_t)a->slot);
            if (!bare_name(a))
                compile_pattern(C, a, a->slot, true, fails);
        }
        return;
    }
    emit2(C, OP_GET_LOCAL, (uint8_t)subj);
    if (p->kind == N_NULL) {
        emit(C, OP_NULL);
        emit(C, OP_EQ);
    } else if (is_range(p)) {
        compile_expr(C, p->args.items[0]);
        compile_expr(C, p->args.items[1]);
        emit2(C, OP_IN_RANGE, !strcmp(p->text, "..") ? 1 : 0);
    } else if (!nested && p->kind == N_NAME && !p->grouped) {
        const char *name = strip_type_args(C, p->text);
        Resolved r = resolve(C, name);
        if (r.kind == R_NONE)
            emit(C, OP_NULL);
        else
            emit_get_resolved(C, &r);
        emit(C, OP_MATCH_ENUM);
        emit16(C, name_const(C, name));
    } else {
        compile_expr(C, p);
        emit(C, OP_MATCH_VALUE);
    }
    jumps_add(fails, emit_jump(C, OP_JUMP_IF_FALSE));
}

static void compile_match(Compiler *C, Node *s) {
    FuncState *fs = C->fs;
    begin_scope(C);
    compile_expr(C, s->expr);
    int subj = add_local(C, "$subject", val_null(), true);
    Jumps ends = {0};
    for (int i = 0; i < s->ncatches; i++) {
        begin_scope(C);
        int base_locals = fs->nlocals;
        const char *ptype = s->catch_types[i];
        Jumps fails = {0};
        int to_body[64], nbody = 0;
        if (ptype) {
            /* `case Type name` / `case ok v` / `case error e` */
            emit(C, OP_NULL);
            int bind = add_local(C, s->catch_names[i], val_null(), false);
            emit2(C, OP_GET_LOCAL, (uint8_t)subj);
            bool ok = !strcmp(ptype, "ok"), er = !strcmp(ptype, "error");
            if (ok || er) {
                emit2(C, OP_RESULT_IS, ok ? 1 : 0);
                jumps_add(&fails, emit_jump(C, OP_JUMP_IF_FALSE));
                emit2(C, OP_GET_LOCAL, (uint8_t)subj);
                emit(C, OP_RESULT_VALUE);
            } else {
                emit(C, OP_IS);
                emit16(C, make_const(C, make_type(C, ptype)));
                jumps_add(&fails, emit_jump(C, OP_JUMP_IF_FALSE));
                emit2(C, OP_GET_LOCAL, (uint8_t)subj);
            }
            emit2(C, OP_SET_LOCAL, (uint8_t)bind);
        } else {
            NodeList *pats = &s->patterns[i];
            for (int a = 0; a < pats->count; a++)
                prealloc_pattern(C, pats->items[a], false);
            for (int a = 0; a < pats->count; a++) {
                Jumps alt = {0};
                compile_pattern(C, pats->items[a], subj, false, &alt);
                if (a + 1 < pats->count) {
                    if (nbody < 64)
                        to_body[nbody++] = emit_jump(C, OP_JUMP);
                    for (int k = 0; k < alt.count; k++)
                        patch_jump(C, alt.items[k]);
                    free(alt.items);
                } else {
                    for (int k = 0; k < alt.count; k++)
                        jumps_add(&fails, alt.items[k]);
                    free(alt.items);
                }
            }
        }
        for (int k = 0; k < nbody; k++)
            patch_jump(C, to_body[k]);
        if (s->guards[i]) {
            compile_expr(C, s->guards[i]);
            jumps_add(&fails, emit_jump(C, OP_JUMP_IF_FALSE));
        }
        begin_scope(C);
        compile_block(C, &s->catches[i]);
        end_scope(C);
        /* leave the case: pop its bindings */
        emit_pops_to(C, fs->scope_depth - 1);
        jumps_add(&ends, emit_jump(C, OP_JUMP));
        for (int k = 0; k < fails.count; k++)
            patch_jump(C, fails.items[k]);
        free(fails.items);
        end_scope(C);
        (void)base_locals;
    }
    if (s->has_else) {
        begin_scope(C);
        compile_block(C, &s->other);
        end_scope(C);
    } else
        emit(C, OP_NO_MATCH);
    for (int k = 0; k < ends.count; k++)
        patch_jump(C, ends.items[k]);
    free(ends.items);
    end_scope(C);
}

/* ---------------------------------------------------------------- statements */

static void emit_coerce(Compiler *C, Value type, const char *what) {
    emit(C, OP_COERCE);
    emit16(C, make_const(C, type));
    emit16(C, make_const(C, val_obj(str_intern_c(C->vm, what))));
}

static void compile_assign(Compiler *C, Node *s) {
    Node *t = s->target;
    const char *op = s->op;
    bool compound = strcmp(op, "=") != 0;
    uint8_t code = 0;
    if (compound) {
        char base[4] = {0};
        size_t bl = strlen(op) - 1;
        memcpy(base, op, bl < 3 ? bl : 3);
        code = binary_op(base);
    }
    if (t->kind == N_NAME && !t->grouped) {
        const char *name = t->text;
        Resolved r = resolve(C, name);
        if (r.kind == R_NONE)
            cfail(C, s->line, "undeclared identifier '%s' (declare it with var or a type)", name);
        if (r.kind == R_CONST)
            cfail(C, s->line, "cannot assign to '%s'", name);
        Value type = val_null();
        bool is_const = false;
        if ((r.kind == R_LOCAL || r.kind == R_UPVAL) && r.local) {
            type = r.local->type;
            is_const = r.local->is_const;
        } else if (r.kind == R_GLOBAL && r.index < C->nglobal_types) {
            type = C->global_types[r.index];
            is_const = C->global_const[r.index];
        } else if (r.kind == R_MGLOBAL)
            cfail(C, s->line, "cannot assign to '%s' imported from another module", name);
        if (is_const)
            cfail(C, s->line, "cannot assign to constant '%s'", name);
        if (compound)
            emit_get_resolved(C, &r);
        compile_expr(C, s->expr);
        if (compound)
            emit(C, code);
        if (type_needs_coerce(type))
            emit_coerce(C, type, name);
        switch (r.kind) {
        case R_LOCAL:
            emit2(C, OP_SET_LOCAL, (uint8_t)r.index);
            break;
        case R_UPVAL:
            emit2(C, OP_SET_UPVAL, (uint8_t)r.index);
            break;
        default:
            emit(C, OP_SET_GLOBAL);
            emit16(C, r.index);
        }
        return;
    }
    if (t->kind == N_INST || t->kind == N_MEMBER) {
        ObjModule *mod;
        if (t->kind == N_MEMBER && member_on_module(C, t, &mod)) {
            int slot = module_find(mod, str_intern_c(C->vm, t->text));
            if (slot < 0 || mod->native)
                cfail(C, s->line, "cannot assign to '%s.%s'", mod->name->chars, t->text);
            if (compound)
                compile_module_member(C, mod, t->text, s->line);
            compile_expr(C, s->expr);
            if (compound)
                emit(C, code);
            emit(C, OP_SET_MGLOBAL);
            emit16(C, make_const(C, val_obj(mod)));
            emit16(C, slot);
            return;
        }
        if (t->kind == N_INST)
            emit_receiver(C, s->line);
        else
            compile_expr(C, t->args.items[0]);
        int name = name_const(C, t->text);
        if (compound) {
            emit(C, OP_DUP);
            emit(C, OP_GET_MEMBER);
            emit16(C, name);
            emit16(C, new_cache(C));
        }
        compile_expr(C, s->expr);
        if (compound)
            emit(C, code);
        emit(C, OP_SET_MEMBER);
        emit16(C, name);
        emit16(C, new_cache(C));
        return;
    }
    if (t->kind == N_INDEX) {
        compile_expr(C, t->args.items[0]);
        compile_expr(C, t->args.items[1]);
        if (compound) {
            emit(C, OP_DUP2);
            emit(C, OP_GET_INDEX);
        }
        compile_expr(C, s->expr);
        if (compound)
            emit(C, code);
        emit(C, OP_SET_INDEX);
        return;
    }
    cfail(C, s->line, "invalid assignment target");
}

static void compile_for(Compiler *C, Node *s) {
    begin_scope(C);
    Node *it = s->expr;
    if (is_range(it) && !s->op) {
        compile_expr(C, it->args.items[0]);
        compile_expr(C, it->args.items[1]);
        int slot = add_local(C, "$cur", val_null(), false);
        add_local(C, "$end", val_null(), false);
        emit2(C, OP_RANGE_PREP, (uint8_t)slot);
        int start = chunk(C)->count;
        push_loop(C, start);
        emit(C, OP_RANGE_NEXT);
        emit(C, (uint8_t)slot);
        emit(C, !strcmp(it->text, "..") ? 1 : 0);
        int exit = chunk(C)->count;
        emit16(C, 0xffff);
        begin_scope(C);
        add_local(C, s->name, val_null(), false);
        begin_scope(C);
        compile_block(C, &s->body);
        end_scope(C);
        end_scope(C);
        emit_loop(C, start);
        patch_jump(C, exit);
        pop_loop(C);
    } else {
        compile_expr(C, it);
        int slot = add_local(C, "$seq", val_null(), false);
        emit(C, OP_NULL);
        add_local(C, "$idx", val_null(), false);
        emit(C, OP_NULL);
        add_local(C, "$aux", val_null(), false);
        int names = s->op ? 2 : 1;
        emit(C, OP_FOR_PREP);
        emit(C, (uint8_t)slot);
        emit(C, (uint8_t)names);
        int start = chunk(C)->count;
        push_loop(C, start);
        emit(C, OP_FOR_NEXT);
        emit(C, (uint8_t)slot);
        emit(C, (uint8_t)names);
        int exit = chunk(C)->count;
        emit16(C, 0xffff);
        begin_scope(C);
        add_local(C, s->name, val_null(), false);
        if (s->op)
            add_local(C, s->op, val_null(), false);
        begin_scope(C);
        compile_block(C, &s->body);
        end_scope(C);
        end_scope(C);
        emit_loop(C, start);
        patch_jump(C, exit);
        pop_loop(C);
    }
    end_scope(C);
}

static void compile_stmt(Compiler *C, Node *s) {
    FuncState *fs = C->fs;
    C->line = s->line;
    C->col = s->col;
    switch (s->kind) {
    case S_EXPR: {
        Node *e = s->target;
        compile_expr(C, e);
        if (e->kind == N_NAME || e->kind == N_MEMBER || e->kind == N_INST)
            emit(C, OP_CALL_STMT);
        emit(C, OP_POP);
        break;
    }
    case S_DECLARE: {
        Value type = s->type ? make_type(C, s->type) : val_null();
        bool is_const = s->op && !strcmp(s->op, "const");
        const char *hint = C->unit_hint;
        C->unit_hint = s->type;
        if (!compile_cast_with_type(C, s->expr, type))
            compile_expr(C, s->expr);
        C->unit_hint = hint;
        if (IS_OBJ(type))
            emit_coerce(C, type, s->name);
        if (fs->is_script && fs->scope_depth == 0) {
            int slot = module_slot(C->vm, C->module, str_intern_c(C->vm, s->name));
            if (slot < C->nglobal_types) {
                C->global_types[slot] = type;
                C->global_const[slot] = is_const;
            }
            emit(C, OP_SET_GLOBAL);
            emit16(C, slot);
        } else {
            for (int i = fs->nlocals - 1; i >= 0 && fs->locals[i].depth == fs->scope_depth; i--)
                if (fs->locals[i].name && !strcmp(fs->locals[i].name, s->name) && s->name[0] != '$')
                    cfail(C, s->line, "'%s' is already declared in this scope", s->name);
            add_local(C, s->name, type, is_const);
        }
        break;
    }
    case S_DESTRUCTURE: {
        Resolved r = resolve(C, s->name);
        emit_get_resolved(C, &r);
        emit(C, OP_DESTRUCTURE);
        emit16(C, atoi(s->op));
        emit(C, OP_POP);
        break;
    }
    case S_ASSIGN:
        compile_assign(C, s);
        break;
    case S_IF: {
        compile_expr(C, s->expr);
        int skip = emit_jump(C, OP_JUMP_IF_FALSE);
        begin_scope(C);
        compile_block(C, &s->body);
        end_scope(C);
        if (s->other.count) {
            int end = emit_jump(C, OP_JUMP);
            patch_jump(C, skip);
            begin_scope(C);
            compile_block(C, &s->other);
            end_scope(C);
            patch_jump(C, end);
        } else
            patch_jump(C, skip);
        break;
    }
    case S_WHILE: {
        int start = chunk(C)->count;
        push_loop(C, start);
        compile_expr(C, s->expr);
        int exit = emit_jump(C, OP_JUMP_IF_FALSE);
        begin_scope(C);
        compile_block(C, &s->body);
        end_scope(C);
        emit_loop(C, start);
        patch_jump(C, exit);
        pop_loop(C);
        break;
    }
    case S_FOR:
        compile_for(C, s);
        break;
    case S_RETURN:
        compile_return(C, s);
        break;
    case S_BREAK:
        compile_break(C, s, true);
        break;
    case S_CONTINUE:
        compile_break(C, s, false);
        break;
    case S_THROW:
        compile_expr(C, s->expr);
        if (s->target) {
            compile_expr(C, s->target);
            emit(C, s->op && !strcmp(s->op, "with") ? OP_THROW_WITH : OP_THROW_NAMED);
        } else
            emit(C, OP_THROW);
        break;
    case S_TRY:
        compile_try(C, s);
        break;
    case S_MATCH:
        compile_match(C, s);
        break;
    case S_DEFER:
        cfail(C, s->line, "defer must be inside a function or block");
    default:
        cfail(C, s->line, "unsupported statement");
    }
}

static void compile_block(Compiler *C, NodeList *body) {
    for (int i = 0; i < body->count; i++)
        compile_stmt(C, body->items[i]);
}

/* ---------------------------------------------------------------- functions */

static void init_fs(Compiler *C, FuncState *fs, ObjFunction *fn) {
    memset(fs, 0, sizeof *fs);
    fs->enclosing = C->fs;
    fs->fn = fn;
    fs->ret_type = val_null();
    fn->module = C->module;
    C->fs = fs;
}
static ObjFunction *end_fs(Compiler *C) {
    FuncState *fs = C->fs;
    emit(C, OP_NULL);
    emit(C, OP_RETURN);
    ObjFunction *fn = fs->fn;
    if (fn->ncaches) {
        fn->caches = TB_ALLOC(C->vm, InlineCache, fn->ncaches);
        memset(fn->caches, 0, sizeof(InlineCache) * (size_t)fn->ncaches);
    }
    C->fs = fs->enclosing;
    return fn;
}

static ObjFunction *compile_function(Compiler *C, FuncDecl *f, bool method, bool lambda,
                                     bool component, ObjClass *owner) {
    FuncState *fs = xmalloc(sizeof(FuncState));
    ObjFunction *fn = new_function(C->vm);
    int saved_line = C->line;
    FuncState *parent = C->fs;
    init_fs(C, fs, fn);
    fs->is_method = method;
    fs->is_component = component;
    fs->in_class = method || (lambda && parent && parent->in_class);
    fn->owner = owner ? owner : (lambda && parent ? parent->fn->owner : NULL);
    fn->name = str_intern_c(C->vm, f->name ? f->name : "lambda");
    fn->arity = f->nparams;
    fn->required = 0;
    for (int i = 0; i < f->nparams; i++)
        if (!f->params[i].value)
            fn->required = i + 1;
    C->line = f->line;
    fs->scope_depth = 1;
    add_local(C, method ? "inst" : "", val_null(), true);
    for (int i = 0; i < f->nparams; i++) {
        Value t = make_type(C, f->params[i].type);
        add_local(C, f->params[i].name, t, false);
    }
    for (int i = 0; i < f->nparams; i++) {
        Param *p = &f->params[i];
        if (p->value) {
            emit2(C, OP_ARG_PRESENT, (uint8_t)i);
            int skip = chunk(C)->count;
            emit16(C, 0xffff);
            compile_expr(C, p->value);
            emit2(C, OP_SET_LOCAL, (uint8_t)(i + 1));
            patch_jump(C, skip);
        }
        Value t = fs->locals[i + 1].type;
        if (type_needs_coerce(t)) {
            emit(C, OP_COERCE_LOCAL);
            emit(C, (uint8_t)(i + 1));
            emit16(C, make_const(C, t));
            emit16(C, make_const(C, val_obj(str_intern_c(C->vm, p->name))));
        }
    }
    fs->ret_type = make_type(C, f->result);
    compile_block(C, &f->body);
    end_fs(C);
    if (lambda) {
        emit(C, OP_CLOSURE);
        emit16(C, make_const(C, val_obj(fn)));
        for (int i = 0; i < fn->upvalue_count; i++) {
            emit(C, fs->upvals[i].is_local ? 1 : 0);
            emit(C, fs->upvals[i].index);
        }
    }
    free(fs);
    C->line = saved_line;
    return fn;
}

static bool constant_node(Node *e, Compiler *C, Value *out) {
    switch (e->kind) {
    case N_INT: {
        unsigned long long v = strtoull(e->text, NULL, 10);
        if (v > (unsigned long long)INT64_MAX)
            return false;
        *out = val_int((int64_t)v);
        return true;
    }
    case N_REAL:
        *out = val_double(strtod(e->text, NULL));
        return true;
    case N_STRING:
        *out = val_obj(str_intern_c(C->vm, e->text));
        return true;
    case N_BOOL:
        *out = val_bool(!strcmp(e->text, "true"));
        return true;
    case N_NULL:
        *out = val_null();
        return true;
    case N_CHAR: {
        int adv;
        *out = val_char(utf8_decode(e->text, (int)strlen(e->text), &adv));
        return true;
    }
    case N_UNARY:
        if (!strcmp(e->text, "-") && constant_node(e->args.items[0], C, out)) {
            if (IS_INT(*out) && out->as.i != INT64_MIN) {
                out->as.i = -out->as.i;
                return true;
            }
            if (IS_DOUBLE(*out)) {
                out->as.d = -out->as.d;
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

/* ---------------------------------------------------------------- classes */

typedef struct {
    ClassDecl *decl;
    ObjClass *klass;
    bool emitted;
} ClassEntry;

static ObjClass *build_class(Compiler *C, ClassDecl *d) {
    VM *vm = C->vm;
    ObjClass *k = new_class(vm, str_intern_c(vm, d->name));
    k->module = C->module;
    k->is_interface = d->is_interface;
    k->is_variant = d->variant_of != NULL;
    k->is_enum = d->nvariants > 0;
    k->nfields = d->nfields;
    k->own_start = 0;
    k->field_names = TB_ALLOC(vm, ObjString *, d->nfields ? d->nfields : 1);
    k->field_types = TB_ALLOC(vm, Value, d->nfields ? d->nfields : 1);
    k->field_has_default = TB_ALLOC(vm, bool, d->nfields ? d->nfields : 1);
    k->field_consts = TB_ALLOC(vm, Value, d->nfields ? d->nfields : 1);
    bool computed = false;
    for (int i = 0; i < d->nfields; i++) {
        Param *p = &d->fields[i];
        k->field_names[i] = str_intern_c(vm, p->name);
        Value t = make_type(C, p->type);
        k->field_types[i] = t;
        k->field_has_default[i] = p->value != NULL;
        k->field_consts[i] = val_undef();
        if (p->value) {
            Value cv;
            if (constant_node(p->value, C, &cv))
                k->field_consts[i] = cv;
            else
                computed = true;
        }
    }
    if (computed) {
        /* A method assigning computed defaults to fields that are still unset. */
        FuncState *fs = xmalloc(sizeof(FuncState));
        ObjFunction *fn = new_function(vm);
        init_fs(C, fs, fn);
        fs->is_method = true;
        fs->in_class = true;
        fn->owner = k;
        fn->name = str_intern_c(vm, "$defaults");
        fs->scope_depth = 1;
        add_local(C, "inst", val_null(), true);
        for (int i = 0; i < d->nfields; i++) {
            Param *p = &d->fields[i];
            if (!p->value || !IS_UNDEF(k->field_consts[i]))
                continue;
            C->line = p->line;
            int name = name_const(C, p->name);
            emit2(C, OP_GET_LOCAL, 0);
            emit(C, OP_FIELD_UNSET);
            emit16(C, name);
            int skip = emit_jump(C, OP_JUMP_IF_FALSE);
            emit2(C, OP_GET_LOCAL, 0);
            compile_expr(C, p->value);
            emit(C, OP_SET_MEMBER);
            emit16(C, name);
            emit16(C, new_cache(C));
            patch_jump(C, skip);
        }
        end_fs(C);
        free(fs);
        k->defaults = val_obj(new_closure(vm, fn));
    }
    for (int i = 0; i < d->nmethods; i++) {
        FuncDecl *m = &d->methods[i];
        if (m->is_abstract)
            continue;
        ObjFunction *fn = compile_function(C, m, true, false, false, k);
        ObjClosure *cl = new_closure(vm, fn);
        table_set(vm, &k->methods, str_intern_c(vm, m->name), val_obj(cl));
    }
    return k;
}

static void emit_type_value(Compiler *C, const char *type, int line) {
    const char *name = strip_type_args(C, type);
    const char *dot = strchr(name, '.');
    if (dot) {
        char *head = arena_strndup(C->arena, name, (size_t)(dot - name));
        ObjModule *mod = find_alias(C, head);
        if (mod && resolve_local(C->fs, head) < 0 && module_find(C->module, str_intern_c(C->vm, head)) < 0) {
            compile_module_member(C, mod, dot + 1, line);
            return;
        }
        compile_name(C, head, line);
        emit(C, OP_GET_MEMBER);
        emit16(C, name_const(C, dot + 1));
        emit16(C, new_cache(C));
        return;
    }
    compile_name(C, name, line);
}

static void emit_class(Compiler *C, ClassEntry *entries, int n, int i) {
    ClassEntry *e = &entries[i];
    if (e->emitted)
        return;
    e->emitted = true;
    ClassDecl *d = e->decl;
    /* define a base declared in this module first */
    if (d->base) {
        const char *bn = strip_type_args(C, d->base);
        for (int j = 0; j < n; j++)
            if (!strcmp(entries[j].decl->name, bn))
                emit_class(C, entries, n, j);
    }
    for (int k = 0; k < d->ninterfaces; k++) {
        const char *in = strip_type_args(C, d->interfaces[k]);
        for (int j = 0; j < n; j++)
            if (!strcmp(entries[j].decl->name, in))
                emit_class(C, entries, n, j);
    }
    C->line = d->line;
    int kc = make_const(C, val_obj(e->klass));
    if (d->base) {
        emit_type_value(C, d->base, d->line);
        emit(C, OP_INHERIT);
        emit16(C, kc);
    }
    for (int k = 0; k < d->ninterfaces; k++) {
        emit_type_value(C, d->interfaces[k], d->line);
        emit(C, OP_IMPLEMENT);
        emit16(C, kc);
    }
    emit(C, OP_FINALIZE);
    emit16(C, kc);
    if (d->variant_of) {
        emit(C, OP_POP);
    } else {
        int slot = module_find(C->module, e->klass->name);
        emit(C, OP_SET_GLOBAL);
        emit16(C, slot);
    }
}

/* ---------------------------------------------------------------- modules */

static void add_wild(Compiler *C, Wild w) {
    C->wilds = xrealloc(C->wilds, sizeof(Wild) * (size_t)(C->nwilds + 1));
    C->wilds[C->nwilds++] = w;
}
static void add_alias(Compiler *C, const char *alias, ObjModule *m) {
    C->aliases = xrealloc(C->aliases, sizeof(Alias) * (size_t)(C->naliases + 1));
    C->aliases[C->naliases].alias = alias;
    C->aliases[C->naliases].module = m;
    C->naliases++;
}

static void declare_global(Compiler *C, const char *name, int line) {
    ObjString *s = str_intern_c(C->vm, name);
    if (module_find(C->module, s) >= 0 && !C->module->repl)
        cfail(C, line, "'%s' is declared more than once", name);
    module_slot(C->vm, C->module, s);
}

static void process_imports(Compiler *C) {
    Program *p = C->prog;
    for (int i = 0; i < p->nimports; i++) {
        ImportDecl *im = &p->imports[i];
        C->line = im->line;
        if (im->kind)
            cfail(C, im->line, "native '%s' imports need the Flywheel compiler; Flywheel Lite does not build native code", im->kind);
        bool native = false;
        CompileError sub;
        memset(&sub, 0, sizeof sub);
        ObjModule *m = vm_load_module(C->vm, im->name, C->path, &native, &sub);
        if (!m) {
            if (sub.file) {
                *C->err = sub;
                longjmp(C->jmp, 1);
            }
            cfail(C, im->line, "%s", sub.message[0] ? sub.message : "cannot import module");
        }
        add_alias(C, im->alias, m);
        if (im->wildcard) {
            if (!strcmp(im->name, "html") || !strcmp(im->name, "svg"))
                C->module->html = true;
            for (int s = 0; s < m->names_by_slot.count; s++) {
                if (m->is_private && m->is_private[s])
                    continue;
                ObjString *nm = AS_STRING(m->names_by_slot.items[s]);
                if (nm->chars[0] == '$')
                    continue;
                Wild w;
                memset(&w, 0, sizeof w);
                w.name = arena_strndup(C->arena, nm->chars, (size_t)nm->len);
                bool dup = false;
                for (int k = 0; k < C->nwilds; k++)
                    if (!strcmp(C->wilds[k].name, w.name)) {
                        if (C->wilds[k].from != m)
                            cfail(C, im->line, "wildcard imports both define '%s'", w.name);
                        dup = true;
                    }
                if (dup)
                    continue;
                w.from = m;
                if (m->native) {
                    w.value = m->values.items[s];
                } else {
                    w.module = m;
                    w.slot = s;
                    w.component = m->is_component_slot && m->is_component_slot[s];
                }
                add_wild(C, w);
            }
        }
    }
}

ObjFunction *compile_module(VM *vm, ObjModule *module, const char *source, size_t len,
                            const char *path, CompileError *err) {
    Compiler *C = xmalloc(sizeof(Compiler));
    memset(C, 0, sizeof *C);
    C->vm = vm;
    C->module = module;
    C->err = err;
    C->path = path;
    C->arena = arena_new();
    err->file = path;
    err->source = source;
    Program prog;
    vm->gc_off++;
    ObjFunction *result = NULL;
    FuncState *fs = NULL;
    if (!parse_program(C->arena, source, len, &prog, err))
        goto done;
    C->prog = &prog;
    if (setjmp(C->jmp))
        goto done;
    process_imports(C);
    /* Predeclare top-level names so functions can refer to later declarations. */
    for (int i = 0; i < prog.nfunctions; i++)
        declare_global(C, prog.functions[i].name, prog.functions[i].line);
    for (int i = 0; i < prog.nclasses; i++)
        if (!prog.classes[i].variant_of)
            declare_global(C, prog.classes[i].name, prog.classes[i].line);
    for (int i = 0; i < prog.nenums; i++)
        declare_global(C, prog.enums[i].name, prog.enums[i].line);
    for (int i = 0; i < prog.ncomponents; i++) {
        if (module->repl && module_find(module, str_intern_c(vm, prog.components[i].name)) >= 0)
            continue;
        declare_global(C, prog.components[i].name, prog.components[i].line);
        module->is_component_slot[module_find(module, str_intern_c(vm, prog.components[i].name))] = true;
    }
    for (int i = 0; i < prog.body.count; i++) {
        Node *s = prog.body.items[i];
        if (s->kind == S_DECLARE) {
            ObjString *nm = str_intern_c(vm, s->name);
            if (module_find(module, nm) >= 0 && !module->repl)
                cfail(C, s->line, "'%s' is declared more than once", s->name);
            module_slot(vm, module, nm);
        }
    }
    for (int i = 0; i < prog.nprivates; i++) {
        int s = module_find(module, str_intern_c(vm, prog.privates[i]));
        if (s >= 0)
            module->is_private[s] = true;
    }
    C->nglobal_types = module->values.count;
    C->global_types = xmalloc(sizeof(Value) * (size_t)(C->nglobal_types + 1));
    C->global_const = xmalloc((size_t)C->nglobal_types + 1);
    for (int i = 0; i < C->nglobal_types; i++) {
        C->global_types[i] = val_null();
        C->global_const[i] = false;
    }
    /* top-level declarations' types and const-ness apply inside functions compiled earlier */
    for (int i = 0; i < prog.body.count; i++) {
        Node *s = prog.body.items[i];
        if (s->kind != S_DECLARE)
            continue;
        int slot = module_find(module, str_intern_c(vm, s->name));
        if (slot >= 0 && slot < C->nglobal_types) {
            C->global_const[slot] = s->op && !strcmp(s->op, "const");
            if (s->type)
                C->global_types[slot] = make_type(C, s->type);
        }
    }

    fs = xmalloc(sizeof(FuncState));
    ObjFunction *script = new_function(vm);
    init_fs(C, fs, script);
    fs->is_script = true;
    script->name = str_intern_c(vm, "<module>");
    add_local(C, "", val_null(), true);

    /* Imported modules initialize first. */
    for (int i = 0; i < C->naliases; i++) {
        ObjModule *m = C->aliases[i].module;
        if (m->native)
            continue;
        emit(C, OP_IMPORT);
        emit16(C, make_const(C, val_obj(m)));
        emit(C, OP_POP);
    }
    /* Enums */
    for (int i = 0; i < prog.nenums; i++) {
        EnumDecl *d = &prog.enums[i];
        ObjEnum *en = new_enum(vm, str_intern_c(vm, d->name), d->nmembers);
        for (int m = 0; m < d->nmembers; m++) {
            ObjEnumVal *ev = (ObjEnumVal *)alloc_obj(vm, sizeof(ObjEnumVal), OBJ_ENUMVAL);
            ev->e = en;
            ev->index = m;
            ev->name = str_intern_c(vm, d->members[m]);
            en->members[m] = val_obj(ev);
        }
        C->line = d->line;
        emit_const(C, val_obj(en));
        emit(C, OP_SET_GLOBAL);
        emit16(C, module_find(module, en->name));
    }
    /* Functions */
    for (int i = 0; i < prog.nfunctions; i++) {
        ObjFunction *fn = compile_function(C, &prog.functions[i], false, false, false, NULL);
        ObjClosure *cl = new_closure(vm, fn);
        C->line = prog.functions[i].line;
        emit_const(C, val_obj(cl));
        emit(C, OP_SET_GLOBAL);
        emit16(C, module_find(module, fn->name));
    }
    /* Components: functions of one attribute map. */
    for (int i = 0; i < prog.ncomponents; i++) {
        ComponentDecl *cd = &prog.components[i];
        FuncDecl f;
        memset(&f, 0, sizeof f);
        f.name = cd->name;
        f.line = cd->line;
        Param param;
        memset(&param, 0, sizeof param);
        param.name = "$attrs";
        f.params = &param;
        f.nparams = 1;
        Node ret;
        memset(&ret, 0, sizeof ret);
        ret.kind = S_RETURN;
        ret.line = cd->line;
        ret.expr = cd->markup;
        Node *items[1] = {&ret};
        f.body.items = items;
        f.body.count = 1;
        ObjFunction *fn = compile_function(C, &f, false, false, true, NULL);
        ObjClosure *cl = new_closure(vm, fn);
        emit_const(C, val_obj(cl));
        emit(C, OP_SET_GLOBAL);
        emit16(C, module_find(module, fn->name));
    }
    /* Classes, bases first. */
    if (prog.nclasses) {
        ClassEntry *entries = xmalloc(sizeof(ClassEntry) * (size_t)prog.nclasses);
        for (int i = 0; i < prog.nclasses; i++) {
            entries[i].decl = &prog.classes[i];
            entries[i].klass = build_class(C, &prog.classes[i]);
            entries[i].emitted = false;
        }
        for (int i = 0; i < prog.nclasses; i++)
            emit_class(C, entries, prog.nclasses, i);
        free(entries);
    }
    if (module->repl && prog.body.count && prog.body.items[prog.body.count - 1]->kind == S_EXPR) {
        /* the REPL shows the value of a trailing expression */
        prog.body.count--;
        compile_block(C, &prog.body);
        Node *last = prog.body.items[prog.body.count];
        C->line = last->line;
        compile_expr(C, last->target);
        if (last->target->kind == N_NAME || last->target->kind == N_MEMBER || last->target->kind == N_INST)
            emit(C, OP_CALL_STMT);
        emit(C, OP_RETURN);
    } else
        compile_block(C, &prog.body);
    result = end_fs(C);
done:
    free(fs);
    free(C->wilds);
    free(C->aliases);
    free(C->global_types);
    free(C->global_const);
    arena_free(C->arena);
    free(C);
    vm->gc_off--;
    return result;
}
