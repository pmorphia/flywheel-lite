/* flywheel-lite: runs Flywheel source files directly on a bytecode virtual machine. */
#include "opcodes.h"
#include "lite.h"
#include <errno.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

static char *read_all(FILE *f, size_t *len) {
    Buf b;
    buf_init(&b);
    char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0)
        buf_add(&b, tmp, n);
    if (!b.data)
        buf_add(&b, "", 0);
    *len = b.len;
    return b.data;
}
static char *read_path(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *s = read_all(f, len);
    fclose(f);
    return s;
}

static void usage(FILE *f) {
    fputs("Flywheel Lite " LITE_VERSION " - runs Flywheel (.fw, .fly) programs on a bytecode VM\n"
          "\n"
          "usage:\n"
          "  flywheel-lite FILE [ARGS...]           run a program\n"
          "  flywheel-lite run FILE [-- ARGS...]    run a program\n"
          "  flywheel-lite run -s CODE              run code given on the command line\n"
          "  flywheel-lite check FILE               compile without running\n"
          "  flywheel-lite debug FILE [ARGS...]     run, then print the top-level return value and run time\n"
          "  flywheel-lite export FILE OUTPUT       run, then write the top-level return value to OUTPUT\n"
          "  flywheel-lite disasm FILE              print the compiled bytecode\n"
          "  flywheel-lite repl                     interactive session\n"
          "  flywheel-lite --version                print the version\n"
          "\n"
          "fwl is a short alias: fwl FILE, fwl repl, ...\n"
          "A top-level `return n` exits with status n. Use - as FILE to read standard input.\n",
          f);
}

/* ---------------------------------------------------------------- disassembler */

#define OPNAME(name) #name,
static const char *op_names[] = {OPCODES(OPNAME)};
#undef OPNAME
static void disasm_function(VM *vm, ObjFunction *fn, int depth) {
    Chunk *c = &fn->chunk;
    printf("%*s== %s (arity %d, upvalues %d) ==\n", depth * 2, "", fn->name ? fn->name->chars : "?", fn->arity,
           fn->upvalue_count);
    int i = 0;
    while (i < c->count) {
        uint8_t op = c->code[i];
        printf("%*s%5d  line %-4d %-16s", depth * 2, "", i, c->lines[i], op < OP__COUNT ? op_names[op] + 3 : "?");
        i++;
#define U16() (i += 2, (c->code[i - 2] << 8) | c->code[i - 1])
        switch (op) {
        case OP_CONST:
        case OP_GET_GLOBAL:
        case OP_SET_GLOBAL:
        case OP_IS:
        case OP_AS:
        case OP_FORMAT:
        case OP_CATCH_MATCH:
        case OP_INHERIT:
        case OP_IMPLEMENT:
        case OP_FINALIZE:
        case OP_MATCH_ENUM:
        case OP_ATTR:
        case OP_DESTRUCTURE:
        case OP_IMPORT:
        case OP_JSON:
        case OP_FIELD_UNSET:
        case OP_TYPE_ERROR:
        case OP_ARRAY:
        case OP_MAP:
        case OP_SET:
        case OP_STRING: {
            int k = U16();
            printf(" %d", k);
            if ((op == OP_CONST || op == OP_ATTR || op == OP_MATCH_ENUM || op == OP_CATCH_MATCH) &&
                k < c->constants.count) {
                Buf b;
                buf_init(&b);
                value_to_buf(vm, c->constants.items[k], &b, true, 0);
                printf("  (%.60s)", b.data ? b.data : "");
                buf_free(&b);
            }
            break;
        }
        case OP_INT:
            printf(" %d", (int16_t)U16());
            break;
        case OP_GET_LOCAL:
        case OP_SET_LOCAL:
        case OP_GET_UPVAL:
        case OP_SET_UPVAL:
        case OP_CALL:
        case OP_CHAIN:
        case OP_RANGE:
        case OP_SLICE:
        case OP_IN_RANGE:
        case OP_FIELD_AT:
        case OP_RESULT_IS:
        case OP_RANGE_PREP:
            printf(" %d", c->code[i++]);
            break;
        case OP_GET_MGLOBAL:
        case OP_SET_MGLOBAL: {
            int m = U16();
            int s = U16();
            printf(" module %d slot %d", m, s);
            break;
        }
        case OP_GET_MEMBER:
        case OP_SET_MEMBER: {
            int k = U16();
            i += 2;
            printf(" %s", k < c->constants.count && IS_STRING(c->constants.items[k]) ? AS_CSTR(c->constants.items[k]) : "?");
            break;
        }
        case OP_INVOKE: {
            int k = U16();
            int argc = c->code[i++];
            i += 2;
            printf(" %s/%d", AS_CSTR(c->constants.items[k]), argc);
            break;
        }
        case OP_SUPER_INVOKE: {
            int k = U16();
            int argc = c->code[i++];
            printf(" %s/%d", AS_CSTR(c->constants.items[k]), argc);
            break;
        }
        case OP_SUPER_GET: {
            int k = U16();
            printf(" %s", AS_CSTR(c->constants.items[k]));
            break;
        }
        case OP_COERCE: {
            i += 2;
            i += 2;
            break;
        }
        case OP_COERCE_LOCAL: {
            int s = c->code[i++];
            i += 2;
            i += 2;
            printf(" %d", s);
            break;
        }
        case OP_JUMP:
        case OP_JUMP_IF_FALSE:
        case OP_AND:
        case OP_OR:
        case OP_JUMP_NOT_NULL:
        case OP_TRY: {
            int off = U16();
            printf(" -> %d", i + off);
            break;
        }
        case OP_LOOP: {
            int off = U16();
            printf(" -> %d", i - off);
            break;
        }
        case OP_ARG_PRESENT: {
            int idx = c->code[i++];
            int off = U16();
            printf(" %d -> %d", idx, i + off);
            break;
        }
        case OP_FOR_PREP:
            printf(" %d %d", c->code[i], c->code[i + 1]);
            i += 2;
            break;
        case OP_FOR_NEXT:
        case OP_RANGE_NEXT: {
            int a = c->code[i], b = c->code[i + 1];
            i += 2;
            int off = U16();
            printf(" %d %d -> %d", a, b, i + off);
            break;
        }
        case OP_LEN_IS:
            printf(" %d", U16());
            i++;
            break;
        case OP_ELEMENT:
            i += 2;
            i += 2;
            printf(" children %d", U16());
            break;
        case OP_CLOSURE: {
            int k = U16();
            ObjFunction *f = (ObjFunction *)AS_OBJ(c->constants.items[k]);
            printf(" %s", f->name->chars);
            i += 2 * f->upvalue_count;
            break;
        }
        default:
            break;
        }
#undef U16
        putchar('\n');
    }
    for (int k = 0; k < c->constants.count; k++) {
        Value v = c->constants.items[k];
        if (IS_OBJT(v, OBJ_FUNCTION))
            disasm_function(vm, (ObjFunction *)AS_OBJ(v), depth + 1);
        else if (IS_OBJT(v, OBJ_CLOSURE))
            disasm_function(vm, ((ObjClosure *)AS_OBJ(v))->fn, depth + 1);
        else if (IS_OBJT(v, OBJ_CLASS)) {
            ObjClass *kl = AS_CLASS(v);
            for (int e = 0; e < kl->methods.cap; e++)
                if (kl->methods.entries[e].key && IS_OBJT(kl->methods.entries[e].value, OBJ_CLOSURE))
                    disasm_function(vm, ((ObjClosure *)AS_OBJ(kl->methods.entries[e].value))->fn, depth + 1);
            if (IS_OBJT(kl->defaults, OBJ_CLOSURE))
                disasm_function(vm, ((ObjClosure *)AS_OBJ(kl->defaults))->fn, depth + 1);
        }
    }
}

/* ---------------------------------------------------------------- helpers */

static const char *exe_dir_of(const char *argv0) {
    const char *slash = strrchr(argv0, '/');
#ifdef _WIN32
    const char *bs = strrchr(argv0, '\\');
    if (bs && (!slash || bs > slash))
        slash = bs;
#endif
    if (!slash)
        return NULL;
    return xstrndup(argv0, (size_t)(slash - argv0));
}

static int finish(VM *vm, RunResult r, Value result) {
    fflush(stdout);
    if (r == RUN_ERROR)
        return 1;
    if (IS_INT(result))
        return (int)result.as.i;
    (void)vm;
    return 0;
}

static int compile_only(VM *vm, const char *path, const char *src, size_t len, bool disasm) {
    ObjModule *m = new_module(vm, str_intern_c(vm, "main"));
    m->path = str_cstr(vm, path);
    vm->main_path = path;
    table_set(vm, &vm->modules, str_intern_c(vm, path), val_obj(m));
    va_push(vm, &vm->module_list, val_obj(m));
    CompileError err;
    memset(&err, 0, sizeof err);
    ObjFunction *fn = compile_module(vm, m, src, len, path, &err);
    if (!fn) {
        fprintf(stderr, "%s:%d:%d: error: %s\n", path, err.line, err.col ? err.col : 1, err.message);
        return 1;
    }
    if (disasm)
        disasm_function(vm, fn, 0);
    return 0;
}

/* ---------------------------------------------------------------- repl */

static int repl(VM *vm) {
    bool tty = isatty(fileno(stdin));
    if (tty)
        printf("Flywheel Lite %s (Flywheel %s) - type `exit` or press Ctrl-D to leave\n", LITE_VERSION, LITE_LANGUAGE);
    /* One module holds the session's globals; import lines are replayed before each input
     * because import aliases are compile-time names. */
    ObjModule *session_module = new_module(vm, str_intern_c(vm, "main"));
    session_module->repl = true;
    session_module->initialized = true;
    session_module->path = str_cstr(vm, "<repl>");
    vm->main_path = "<repl>";
    push_root(vm, val_obj(session_module));
    Buf imports;
    buf_init(&imports);
    Buf pending;
    buf_init(&pending);
    for (;;) {
        if (tty)
            fputs(pending.len ? "... " : ">>> ", stdout);
        fflush(stdout);
        Buf line;
        buf_init(&line);
        int c;
        bool any = false;
        while ((c = fgetc(stdin)) != EOF) {
            any = true;
            if (c == '\n')
                break;
            buf_addc(&line, (char)c);
        }
        if (!any) {
            buf_free(&line);
            putchar('\n');
            break;
        }
        if (!pending.len && line.data && (!strcmp(line.data, "exit") || !strcmp(line.data, "quit"))) {
            buf_free(&line);
            break;
        }
        buf_add(&pending, line.data ? line.data : "", line.len);
        buf_addc(&pending, '\n');
        buf_free(&line);
        /* Each input runs as a program made of the session's declarations plus the new
         * input; declarations are kept, other statements are not replayed. */
        Buf program;
        buf_init(&program);
        buf_add(&program, imports.data ? imports.data : "", imports.len);
        size_t mark = program.len;
        buf_add(&program, pending.data, pending.len);
        CompileError err;
        memset(&err, 0, sizeof err);
        ObjFunction *fn = compile_module(vm, session_module, program.data, program.len, "<repl>", &err);
        if (!fn) {
            int lines_before = 0;
            for (size_t i = 0; i < mark; i++)
                if (program.data[i] == '\n')
                    lines_before++;
            bool incomplete = strstr(err.message, "end of file") != NULL || strstr(err.message, "unterminated") != NULL ||
                              strstr(err.message, "unclosed") != NULL;
            if (!incomplete) {
                fprintf(stderr, "error: %s (line %d)\n", err.message, err.line - lines_before);
                pending.len = 0;
            }
            buf_free(&program);
            continue;
        }
        /* run only the new input: recompile session + input but silence old output by
         * running the session part once at declaration time. */
        Value out;
        vm->gc_off++;
        ObjClosure *cl = new_closure(vm, fn);
        vm->gc_off--;
        bool ok = vm_call(vm, val_obj(cl), 0, NULL, &out);
        fflush(stdout);
        if (!ok) {
            vm_report_error(vm, vm->error);
            vm->has_error = false;
        } else {
            for (const char *p = pending.data; p && *p;) {
                const char *e = strchr(p, '\n');
                size_t n = e ? (size_t)(e - p + 1) : strlen(p);
                const char *q = p;
                while (*q == ' ' || *q == '\t')
                    q++;
                if (!strncmp(q, "import ", 7) || !strncmp(q, "import\t", 7))
                    buf_add(&imports, p, n);
                p += n;
            }
            if (!IS_NULL(out)) {
                if (!lib_print_value(vm, stdout, out)) {
                    vm_report_error(vm, vm->error);
                    vm->has_error = false;
                }
                putchar('\n');
            }
        }
        fflush(stdout);
        pending.len = 0;
        buf_free(&program);
    }
    pop_root(vm, 1);
    buf_free(&imports);
    buf_free(&pending);
    return 0;
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8); /* strings are UTF-8 */
#endif
    if (!isatty(fileno(stdout)))
        setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    const char *cmd = argv[1];
    int argi = 2;
    if (!strcmp(cmd, "--version") || !strcmp(cmd, "-v") || !strcmp(cmd, "version")) {
        printf("flywheel-lite %s (Flywheel %s)\n", LITE_VERSION, LITE_LANGUAGE);
        return 0;
    }
    if (!strcmp(cmd, "--help") || !strcmp(cmd, "-h") || !strcmp(cmd, "help")) {
        usage(stdout);
        return 0;
    }
    enum { RUN, CHECK, DEBUG, EXPORT, DISASM, REPL } mode = RUN;
    if (!strcmp(cmd, "run"))
        mode = RUN;
    else if (!strcmp(cmd, "check"))
        mode = CHECK;
    else if (!strcmp(cmd, "debug"))
        mode = DEBUG;
    else if (!strcmp(cmd, "export"))
        mode = EXPORT;
    else if (!strcmp(cmd, "disasm"))
        mode = DISASM;
    else if (!strcmp(cmd, "repl"))
        mode = REPL;
    else
        argi = 1; /* flywheel-lite FILE ... */

    VM *vm = xmalloc(sizeof(VM));
    const char *exe_dir = exe_dir_of(argv[0]);
    if (mode == REPL) {
        vm_init(vm, 0, NULL);
        vm->exe_dir = exe_dir;
        int r = repl(vm);
        return r;
    }
    if (argi >= argc) {
        usage(stderr);
        return 2;
    }
    const char *path;
    char *source = NULL;
    size_t len = 0;
    if (!strcmp(argv[argi], "-s") || !strcmp(argv[argi], "-e")) {
        if (argi + 1 >= argc) {
            fputs("flywheel-lite: -s needs code\n", stderr);
            return 2;
        }
        path = "<command line>";
        const char *code = argv[argi + 1];
        source = xstrdup(code);
        len = strlen(code);
        argi += 2;
    } else {
        path = argv[argi++];
        if (!strcmp(path, "-")) {
            source = read_all(stdin, &len);
            path = "<stdin>";
        } else {
            source = read_path(path, &len);
            if (!source) {
                fprintf(stderr, "flywheel-lite: cannot read %s: %s\n", path, strerror(errno));
                return 1;
            }
        }
    }
    const char *output = NULL;
    if (mode == EXPORT) {
        if (argi >= argc) {
            fputs("flywheel-lite: export needs an output file\n", stderr);
            return 2;
        }
        output = argv[argi++];
    }
    if (argi < argc && !strcmp(argv[argi], "--"))
        argi++;
    vm_init(vm, argc - argi, argv + argi);
    vm->exe_dir = exe_dir;
    if (mode == CHECK || mode == DISASM)
        return compile_only(vm, path, source, len, mode == DISASM);

    double start = 0;
    if (mode == DEBUG)
        start = (double)clock() / CLOCKS_PER_SEC;
    Value result = val_null();
    RunResult r = vm_run_main(vm, path, source, len, &result);
    fflush(stdout);
    if (r == RUN_OK && (mode == DEBUG || mode == EXPORT)) {
        Buf b;
        buf_init(&b);
        if (!value_to_buf(vm, result, &b, false, 0)) {
            vm_report_error(vm, vm->error);
            return 1;
        }
        if (mode == DEBUG) {
            printf("%s\n", b.data ? b.data : "");
            fprintf(stderr, "flywheel-lite: ran in %.3f s (CPU)\n", (double)clock() / CLOCKS_PER_SEC - start);
        } else {
            FILE *f = fopen(output, "wb");
            if (!f) {
                fprintf(stderr, "flywheel-lite: cannot write %s: %s\n", output, strerror(errno));
                return 1;
            }
            fwrite(b.data ? b.data : "", 1, b.len, f);
            fclose(f);
        }
        buf_free(&b);
        fflush(stdout);
        return 0;
    }
    int code = finish(vm, r, result);
    /* Leave the heap to the OS: freeing every object at exit only costs time. */
    return code;
}
