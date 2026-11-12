/* Flywheel Lite: a bytecode virtual machine that runs Flywheel (.fw / .fly) source directly.
 * C11, no dependencies beyond the C standard library and libm. */
#ifndef LITE_H
#define LITE_H

/* POSIX and platform declarations (realpath, popen, clock_gettime, ...) under -std=c11. */
#if defined(_WIN32)
#define _CRT_RAND_S
#define _CRT_SECURE_NO_WARNINGS
#else
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LITE_VERSION "2.9 (beta-9)"
#define LITE_LANGUAGE "2.19"

#if defined(__GNUC__) || defined(__clang__)
#define TB_NORETURN __attribute__((noreturn))
#define TB_LIKELY(x) __builtin_expect(!!(x), 1)
#define TB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define TB_PRINTF(a, b) __attribute__((format(printf, a, b)))
#elif defined(_MSC_VER)
#define TB_NORETURN __declspec(noreturn)
#define TB_LIKELY(x) (x)
#define TB_UNLIKELY(x) (x)
#define TB_PRINTF(a, b)
#else
#define TB_NORETURN
#define TB_LIKELY(x) (x)
#define TB_UNLIKELY(x) (x)
#define TB_PRINTF(a, b)
#endif

typedef struct VM VM;
typedef struct Obj Obj;
typedef struct ObjString ObjString;

/* ---------------------------------------------------------------- values */

typedef enum { VAL_UNDEF, VAL_NULL, VAL_BOOL, VAL_INT, VAL_DOUBLE, VAL_CHAR, VAL_OBJ } ValueType;

typedef struct {
    uint8_t type;
    union {
        bool b;
        int64_t i;
        double d;
        uint32_t c;
        Obj *o;
    } as;
} Value;

static inline Value val_undef(void) { Value v; v.type = VAL_UNDEF; v.as.i = 0; return v; }
static inline Value val_null(void) { Value v; v.type = VAL_NULL; v.as.i = 0; return v; }
static inline Value val_bool(bool b) { Value v; v.type = VAL_BOOL; v.as.i = 0; v.as.b = b; return v; }
static inline Value val_int(int64_t i) { Value v; v.type = VAL_INT; v.as.i = i; return v; }
static inline Value val_double(double d) { Value v; v.type = VAL_DOUBLE; v.as.d = d; return v; }
static inline Value val_char(uint32_t c) { Value v; v.type = VAL_CHAR; v.as.i = 0; v.as.c = c; return v; }
static inline Value val_obj(void *o) { Value v; v.type = VAL_OBJ; v.as.o = (Obj *)o; return v; }

#define IS_UNDEF(v) ((v).type == VAL_UNDEF)
#define IS_NULL(v) ((v).type == VAL_NULL)
#define IS_BOOL(v) ((v).type == VAL_BOOL)
#define IS_INT(v) ((v).type == VAL_INT)
#define IS_DOUBLE(v) ((v).type == VAL_DOUBLE)
#define IS_NUM(v) ((v).type == VAL_INT || (v).type == VAL_DOUBLE)
#define IS_CHAR(v) ((v).type == VAL_CHAR)
#define IS_OBJ(v) ((v).type == VAL_OBJ)
#define AS_OBJ(v) ((v).as.o)
#define OBJ_TYPE(v) (AS_OBJ(v)->type)
#define IS_OBJT(v, t) (IS_OBJ(v) && OBJ_TYPE(v) == (t))
#define AS_NUM(v) (IS_INT(v) ? (double)(v).as.i : (v).as.d)

typedef enum {
    OBJ_STRING,
    OBJ_ARRAY,
    OBJ_TUPLE,
    OBJ_MAP,
    OBJ_SET,
    OBJ_FUNCTION,
    OBJ_CLOSURE,
    OBJ_UPVALUE,
    OBJ_NATIVE,
    OBJ_BOUND,
    OBJ_CLASS,
    OBJ_INSTANCE,
    OBJ_ENUM,
    OBJ_ENUMVAL,
    OBJ_MODULE,
    OBJ_ERROR,
    OBJ_RESULT,
    OBJ_ELEMENT,
    OBJ_JSON,
    OBJ_TYPE,
    OBJ_FILE,
    OBJ_WEAK,
    OBJ_COUNT
} ObjType;

struct Obj {
    uint8_t type;
    bool marked;
    Obj *next;
};

struct ObjString {
    Obj obj;
    int len;        /* bytes */
    int clen;       /* Unicode scalars */
    uint32_t hash;
    char chars[];
};
#define IS_STRING(v) IS_OBJT(v, OBJ_STRING)
#define AS_STRING(v) ((ObjString *)AS_OBJ(v))
#define AS_CSTR(v) (AS_STRING(v)->chars)

typedef struct {
    Value *items;
    int count, cap;
} ValueArray;

/* Arrays and tuples share a representation; tuples have fixed arity. */
typedef struct {
    Obj obj;
    Value *items;
    int count, cap;
    bool frozen;
} ObjList;
#define IS_ARRAY(v) IS_OBJT(v, OBJ_ARRAY)
#define IS_TUPLE(v) IS_OBJT(v, OBJ_TUPLE)
#define IS_LIST(v) (IS_OBJ(v) && (OBJ_TYPE(v) == OBJ_ARRAY || OBJ_TYPE(v) == OBJ_TUPLE))
#define AS_LIST(v) ((ObjList *)AS_OBJ(v))

/* Insertion-ordered hash map; sets use the same structure with ignored values. */
typedef struct {
    Value key;
    Value value;
    uint32_t hash;
    bool live;
} MapEntry;
typedef struct {
    Obj obj;
    MapEntry *entries;
    int used, live, cap;
    int32_t *index;
    int index_cap;
    bool frozen;
} ObjMap;
#define IS_MAP(v) IS_OBJT(v, OBJ_MAP)
#define IS_SET(v) IS_OBJT(v, OBJ_SET)
#define AS_MAP(v) ((ObjMap *)AS_OBJ(v))

/* String-keyed table for names (globals, fields, methods). Keys are interned. */
typedef struct {
    ObjString *key;
    Value value;
} Entry;
typedef struct {
    int count, cap;
    Entry *entries;
} Table;

typedef struct {
    uint8_t *code;
    int *lines;
    int count, cap;
    ValueArray constants;
} Chunk;

typedef struct {
    void *klass;
    int index;
    Value value;
} InlineCache;

struct ObjModule;
struct ObjClass;

typedef struct ObjFunction {
    Obj obj;
    int arity;        /* declared parameters */
    int required;     /* parameters without defaults */
    int upvalue_count;
    bool variadic;
    Chunk chunk;
    ObjString *name;
    struct ObjModule *module;
    struct ObjClass *owner; /* defining class, for super (static methods: their class) */
    uint8_t access;         /* methods: 0 public, 1 private, 2 protected */
    InlineCache *caches;
    int ncaches;
} ObjFunction;

typedef struct ObjUpvalue {
    Obj obj;
    Value *location;
    Value closed;
    struct ObjUpvalue *next;
} ObjUpvalue;

typedef struct {
    Obj obj;
    ObjFunction *fn;
    ObjUpvalue **upvalues;
    int upvalue_count;
} ObjClosure;

/* Natives receive their arguments (for methods, args[0] is the receiver). Return false after
 * calling vm_throw to raise an error. */
typedef bool (*NativeFn)(VM *vm, int argc, Value *args, Value *out);
typedef struct {
    Obj obj;
    NativeFn fn;
    const char *name;
    int min_args, max_args; /* max -1: variadic */
    bool is_method;         /* first argument is the receiver */
    Value data;
} ObjNative;

typedef struct {
    Obj obj;
    Value receiver;
    Value method;
} ObjBound;

typedef struct ObjClass {
    Obj obj;
    ObjString *name;
    struct ObjClass *super;   /* the first base class */
    struct ObjClass **supers; /* every base class, in declaration order */
    int nsupers;
    struct ObjClass **mro;    /* C3 linearization: the class, then its bases */
    int nmro;
    struct ObjModule *module;
    int nfields;
    ObjString **field_names;
    Value *field_types;       /* OBJ_TYPE or null */
    bool *field_has_default;
    Value *field_consts;      /* constant default, or undef when computed by `defaults` */
    int own_start;            /* first field declared by this class */
    struct ObjClass **field_owner; /* the class declaring each field */
    uint8_t *field_access;    /* per field: 0 public, 1 private, 2 protected */
    Table static_access;      /* static member -> 1 private, 2 protected */
    Table field_index;
    Table methods;
    Table statics;
    struct ObjClass **interfaces;
    int ninterfaces;
    Value defaults;           /* closure assigning own field defaults, or null */
    Value singleton;          /* field-less enum member value */
    bool is_interface, is_enum, is_variant, finalized, native;
    int variant_index;
    Value m_init, m_to_string, m_eq, m_lt, m_le, m_gt, m_ge, m_hash, m_equals, m_next, m_iterator;
    bool has_destructor; /* this class or a base declares `destructor` */
} ObjClass;
#define AS_CLASS(v) ((ObjClass *)AS_OBJ(v))

typedef struct {
    Obj obj;
    ObjClass *klass;
    int nfields;
    bool finalized; /* its destructors have run */
    Value fields[];
} ObjInstance;
#define AS_INSTANCE(v) ((ObjInstance *)AS_OBJ(v))

typedef struct ObjEnum {
    Obj obj;
    ObjString *name;
    int count;
    Value *members; /* OBJ_ENUMVAL */
} ObjEnum;
typedef struct {
    Obj obj;
    ObjEnum *e;
    int index;
    ObjString *name;
} ObjEnumVal;

typedef struct ObjModule {
    Obj obj;
    ObjString *name;
    ObjString *path;
    Table names;      /* name -> int slot */
    ValueArray values;
    ValueArray names_by_slot;
    bool *is_private;
    unsigned char *is_component_slot; /* 1: a component, 2: a void component */
    int private_cap;
    bool native, initialized, initializing, html, repl;
    Value init;       /* closure running the module body */
    Value result;
    /* Unit namespaces the module declares (UnitNs *, front.h), for the modules that import it. */
    void *unit_exports;
    int nunit_exports;
} ObjModule;
#define AS_MODULE(v) ((struct ObjModule *)AS_OBJ(v))

typedef struct {
    Obj obj;
    ObjString *type;
    ObjString *message;
    Value payload;
} ObjError;

typedef struct {
    Obj obj;
    bool ok;
    Value value;
} ObjResult;

typedef struct ObjElement {
    Obj obj;
    ObjString *tag; /* NULL: fragment */
    ValueArray attr_names;
    ValueArray attr_values;
    ValueArray children;
    struct ObjElement *parent;
} ObjElement;
#define AS_ELEMENT(v) ((ObjElement *)AS_OBJ(v))

typedef struct {
    Obj obj;
    Value v; /* plain tree: null/bool/int/double/string/array/map */
} ObjJson;

typedef enum {
    TY_ANY,
    TY_VOID,
    TY_BOOL,
    TY_INT,
    TY_DOUBLE,
    TY_FLOAT,
    TY_CHAR,
    TY_STRING,
    TY_ARRAY,
    TY_TUPLE,  /* fixed heterogeneous [A, B] */
    TY_HTUPLE, /* tuple<T> */
    TY_MAP,
    TY_SET,
    TY_NULLABLE,
    TY_FUNCTION,
    TY_RESULT,
    TY_ELEMENT,
    TY_FRAGMENT,
    TY_JSON,
    TY_ERROR,
    TY_WEAK,
    TY_NAMED
} TypeKind;

typedef struct ObjTypeDesc {
    Obj obj;
    uint8_t kind;
    int bits;      /* integers: 8..64 */
    bool is_unsigned;
    bool convert;  /* coercion may change the value */
    ObjString *name;
    struct ObjTypeDesc **args;
    int nargs;
    struct ObjModule *module; /* named types: module global slot, then a static member */
    int slot;
    ObjString *member;
} ObjTypeDesc;
#define AS_TYPE(v) ((ObjTypeDesc *)AS_OBJ(v))

typedef struct {
    Obj obj;
    FILE *fp;
    ObjString *path;
    bool closed, is_std;
} ObjFile;

typedef struct {
    Obj obj;
    Obj *target;
} ObjWeak;

/* ---------------------------------------------------------------- VM */

#define FRAMES_MAX 100000
#define STACK_MAX (2 * 1024 * 1024)
#define HANDLERS_MAX 4096

typedef struct {
    ObjClosure *closure;
    uint8_t *ip;
    Value *slots;
    int argc;
    bool ctor;
} CallFrame;

typedef struct {
    int frame;
    Value *sp;
    uint8_t *ip;
} Handler;

struct VM {
    CallFrame frames[FRAMES_MAX];
    int frame_count;
    Value *stack;
    Value *sp;
    Handler handlers[HANDLERS_MAX];
    int handler_count;
    ObjUpvalue *open_upvalues;

    Obj *objects;
    size_t bytes_allocated, next_gc;
    int gc_off;
    Obj **gray;
    int gray_count, gray_cap;
    Value roots[256];
    int root_count;

    Table strings;
    Table builtins;
    Table modules;         /* path or native name -> module */
    ValueArray module_list;
    Value error;           /* pending error while unwinding */
    bool has_error;

    ObjClass *error_class; /* unused placeholder for builtin errors */
    ObjClass *task_class;  /* thread.task, made by `spawn` */
    ValueArray finalizers; /* unreachable instances whose destructors run at the next safepoint */
    bool finalizing;
    ObjString *s_init, *s_to_string, *s_length, *s_message, *s_name;

    int argc;
    char **argv;
    const char *main_path;
    bool exiting;
    int exit_code;
    int native_depth;
    uint64_t rng[4];
    bool rng_seeded;
    const char *stdlib_dir;
    const char *exe_dir;
    Value desc_args[8];
    Table type_methods[16];
    char trace[4096];
    Obj *traced;
    Value empty_fragment;
    bool gc_stress;
};

typedef enum { RUN_OK, RUN_ERROR, RUN_EXIT } RunResult;

enum {
    TM_STRING,
    TM_ARRAY,
    TM_TUPLE,
    TM_MAP,
    TM_SET,
    TM_RESULT,
    TM_ERROR,
    TM_ELEMENT,
    TM_JSON,
    TM_FILE,
    TM_CHAR,
    TM_INT,
    TM_DOUBLE,
    TM_ENUMVAL,
    TM_WEAK,
    TM_ANY,
    TM_COUNT
};

/* ---------------------------------------------------------------- memory.c */

void *tb_realloc(VM *vm, void *ptr, size_t old, size_t size);
#define TB_ALLOC(vm, type, n) ((type *)tb_realloc(vm, NULL, 0, sizeof(type) * (size_t)(n)))
#define TB_FREE(vm, type, ptr, n) tb_realloc(vm, ptr, sizeof(type) * (size_t)(n), 0)
#define GROW_CAP(c) ((c) < 8 ? 8 : (c) * 2)
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

Obj *alloc_obj(VM *vm, size_t size, ObjType type);
void gc_collect(VM *vm);
void vm_run_finalizers(VM *vm);
/* Runs the destructors of every instance still alive, at exit. */
void vm_finalize_all(VM *vm);
static inline void vm_maybe_gc(VM *vm) {
    if (TB_UNLIKELY(vm->bytes_allocated > vm->next_gc) && !vm->gc_off) {
        gc_collect(vm);
        if (vm->finalizers.count && !vm->finalizing)
            vm_run_finalizers(vm);
    }
}
void push_root(VM *vm, Value v);
void pop_root(VM *vm, int n);
void free_all_objects(VM *vm);

void va_init(ValueArray *a);
void va_push(VM *vm, ValueArray *a, Value v);
void va_free(VM *vm, ValueArray *a);

uint32_t hash_bytes(const char *s, size_t n);
ObjString *str_copy(VM *vm, const char *chars, size_t len);
ObjString *str_cstr(VM *vm, const char *chars);
ObjString *str_intern(VM *vm, const char *chars, size_t len);
ObjString *str_intern_c(VM *vm, const char *chars);
ObjString *str_take_buf(VM *vm, char *chars, size_t len); /* copies then frees chars */
ObjString *str_fmt(VM *vm, const char *fmt, ...) TB_PRINTF(2, 3);
int utf8_count(const char *s, size_t n);

ObjList *new_list(VM *vm, ObjType type, int cap);
void list_push(VM *vm, ObjList *l, Value v);
ObjMap *new_map(VM *vm, ObjType type);
ObjFunction *new_function(VM *vm);
ObjClosure *new_closure(VM *vm, ObjFunction *fn);
ObjUpvalue *new_upvalue(VM *vm, Value *slot);
ObjNative *new_native(VM *vm, const char *name, NativeFn fn, int min, int max);
ObjBound *new_bound(VM *vm, Value receiver, Value method);
ObjClass *new_class(VM *vm, ObjString *name);
ObjInstance *new_instance(VM *vm, ObjClass *klass);
ObjEnum *new_enum(VM *vm, ObjString *name, int count);
ObjModule *new_module(VM *vm, ObjString *name);
ObjError *new_error(VM *vm, ObjString *type, ObjString *message);
ObjResult *new_result(VM *vm, bool ok, Value v);
ObjElement *new_element(VM *vm, ObjString *tag);
ObjJson *new_json(VM *vm, Value v);
ObjTypeDesc *new_type(VM *vm, TypeKind kind);
ObjFile *new_file(VM *vm, FILE *fp, ObjString *path);
ObjWeak *new_weak(VM *vm, Obj *target);

void table_init(Table *t);
void table_free(VM *vm, Table *t);
bool table_get(Table *t, ObjString *key, Value *out);
bool table_set(VM *vm, Table *t, ObjString *key, Value v); /* true if new */
bool table_delete(Table *t, ObjString *key);
ObjString *table_find_string(Table *t, const char *chars, size_t len, uint32_t hash);
void table_copy(VM *vm, Table *from, Table *to);

int module_slot(VM *vm, ObjModule *m, ObjString *name); /* add or find */
int module_find(ObjModule *m, ObjString *name);         /* -1 when missing */
void module_set_native(VM *vm, ObjModule *m, const char *name, NativeFn fn, int min, int max);
void module_set_value(VM *vm, ObjModule *m, const char *name, Value v);

void chunk_init(Chunk *c);
void chunk_write(VM *vm, Chunk *c, uint8_t byte, int line);
int chunk_add_const(VM *vm, Chunk *c, Value v);

/* ---------------------------------------------------------------- value.c */

typedef struct {
    char *data;
    size_t len, cap;
} Buf;
void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_add(Buf *b, const char *s, size_t n);
void buf_adds(Buf *b, const char *s);
void buf_addc(Buf *b, char c);
void buf_printf(Buf *b, const char *fmt, ...) TB_PRINTF(2, 3);
void buf_utf8(Buf *b, uint32_t cp);

const char *obj_kind_name(Value v);
bool values_same(Value a, Value b);
bool values_equal(VM *vm, Value a, Value b, bool *out);       /* may call user operator== */
bool value_hash(VM *vm, Value v, uint32_t *out);
bool value_to_buf(VM *vm, Value v, Buf *b, bool quoted, int depth);
ObjString *value_to_string(VM *vm, Value v);                  /* NULL on error */
bool value_format_spec(VM *vm, Value v, const char *spec, Buf *out);
void format_double(double d, Buf *b);
bool value_compare(VM *vm, Value a, Value b, int *out);       /* -1/0/1, may raise */
const char *type_name_of(VM *vm, Value v, Buf *b);
uint32_t utf8_decode(const char *s, int len, int *adv);
int utf8_offset(ObjString *s, int index);                     /* byte offset of char index */
uint32_t str_char_at(ObjString *s, int index);
void html_escape(Buf *b, const char *s, size_t n, bool attr);
bool element_render(VM *vm, Value v, Buf *b);

/* ---------------------------------------------------------------- map ops (value.c) */

bool map_get(VM *vm, ObjMap *m, Value key, Value *out, bool *found);
bool map_set(VM *vm, ObjMap *m, Value key, Value v);
bool map_remove(VM *vm, ObjMap *m, Value key, bool *found);
bool map_has(VM *vm, ObjMap *m, Value key, bool *found);
void map_clear(VM *vm, ObjMap *m);
ObjList *map_keys(VM *vm, ObjMap *m);
ObjList *map_values(VM *vm, ObjMap *m);

/* ---------------------------------------------------------------- lexer / parser / compiler */

typedef enum { T_ID, T_INT, T_REAL, T_STRING, T_CHAR, T_MARKUP, T_JSON, T_OP, T_NL, T_EOF } TokKind;

typedef struct {
    TokKind kind;
    const char *text;
    int len;
    int line, col;
    int depth;
    bool spaced;
} Token;

typedef struct {
    Token *items;
    int count, cap;
} TokenList;

typedef struct Arena Arena;
void *arena_alloc(Arena *a, size_t n);
char *arena_strndup(Arena *a, const char *s, size_t n);
Arena *arena_new(void);
void arena_free(Arena *a);

typedef struct {
    const char *file;
    const char *source;
    char message[1024];
    int line, col;
} CompileError;

bool lex_source(Arena *arena, const char *src, size_t len, TokenList *out, CompileError *err,
                int line_offset);

ObjFunction *compile_module(VM *vm, ObjModule *module, const char *source, size_t len,
                            const char *path, CompileError *err);

/* ---------------------------------------------------------------- vm.c */

void vm_init(VM *vm, int argc, char **argv);
void vm_free(VM *vm);
RunResult vm_run_main(VM *vm, const char *path, const char *source, size_t len, Value *result);
bool vm_call(VM *vm, Value callee, int argc, Value *args, Value *out);
bool vm_invoke_method(VM *vm, Value receiver, ObjString *name, int argc, Value *args, Value *out,
                      bool *found);
bool vm_throw(VM *vm, const char *type, const char *fmt, ...) TB_PRINTF(3, 4);
bool vm_throw_value(VM *vm, Value v);
ObjModule *vm_load_module(VM *vm, const char *name, const char *from_path, bool *native,
                          CompileError *err);
bool vm_construct(VM *vm, ObjClass *klass, int argc, Value *args, Value *out);
bool vm_init_fields(VM *vm, ObjClass *klass, ObjInstance *inst, int argc, Value *args);
bool vm_get_member(VM *vm, Value obj, ObjString *name, Value *out);
bool vm_is_instance(ObjClass *klass, ObjClass *of);
Value vm_find_method(ObjClass *klass, ObjString *name);
bool vm_coerce(VM *vm, Value v, ObjTypeDesc *t, Value *out, const char *what);
/* The unit namespace of a hidden quantity class (__fwu_<ns>__int or __fwu_<ns>__double; lower.c). */
bool quantity_class_name(const char *name, char *ns, size_t nscap, bool *real);
/* "<module>__<ns>" with non-identifier characters replaced: names equivalence methods. */
void unit_key(char *out, size_t cap, const char *module, const char *ns);
/* `x in u` for a unit u of x's namespace or of one that `unitconv A = B` relates (vm.c). */
bool vm_quantity_in(VM *vm, Value x, Value unit, Value *out);
void free_unit_exports(void *exports, int count); /* compiler.c */
bool vm_type_test(VM *vm, Value v, ObjTypeDesc *t);
bool vm_cast(VM *vm, ObjTypeDesc *t, Value v, Value *out);
ObjString *error_type_name(VM *vm, Value err);
ObjString *error_message(VM *vm, Value err);
void vm_report_error(VM *vm, Value err);
Value vm_resolve_named(VM *vm, ObjTypeDesc *t);
void vm_print_trace(VM *vm);

/* ---------------------------------------------------------------- libraries */

void lib_core_init(VM *vm);
bool lib_builtin_method(VM *vm, Value self, ObjString *name, int argc, Value *args, Value *out,
                        bool *found);
bool lib_builtin_property(VM *vm, Value self, ObjString *name, Value *out, bool *found);
bool lib_has_builtin_method(VM *vm, Value self, ObjString *name);
bool lib_bound_builtin(VM *vm, Value self, ObjString *name, Value *out);
bool lib_set_property(VM *vm, Value obj, ObjString *name, Value v, bool *found);
bool lib_class_from_json(VM *vm, int argc, Value *args, Value *out);
bool lib_print_value(VM *vm, FILE *f, Value v);
void lib_html_init(VM *vm);
void lib_html_module(VM *vm, ObjModule *m);
bool lib_element_property(VM *vm, Value self, ObjString *name, Value *out, bool *found);
bool lib_element_set_property(VM *vm, Value self, ObjString *name, Value v, bool *found);
ObjModule *lib_native_module(VM *vm, const char *name);
bool lib_is_native_module(const char *name);
bool lib_json_parse(VM *vm, const char *s, size_t n, Value *out);
bool lib_json_encode(VM *vm, Value v, Buf *b, int indent, int level, bool sorted);
bool lib_element_new(VM *vm, ObjString *tag, ObjList *names, Value *attrs, int nchildren,
                     Value *children, Value *out);
const char *lib_embedded_module(const char *name);
bool seq_items(VM *vm, Value v, ObjList **out); /* array/tuple/set/map keys/string chars */
bool to_int64(VM *vm, Value v, int64_t *out, const char *what);
bool to_double(VM *vm, Value v, double *out, const char *what);
uint64_t rng_next(VM *vm);
int tb_path_kind(const char *path); /* 0 missing, 1 file, 2 directory */

#endif
