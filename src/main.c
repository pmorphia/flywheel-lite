/* flywheel-lite: runs Flywheel source files directly on a bytecode virtual machine. */
#include "front.h"
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
#include <dirent.h>
#include <sys/wait.h>
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
    fputs("Usage: flywheel-lite run|r SOURCE [options] [-- program arguments]\n"
          "           run on the bytecode VM\n"
          "       flywheel-lite check|c SOURCE [options]\n"
          "           compile without running\n"
          "       flywheel-lite export|e SOURCE OUTPUT [options] [program arguments]\n"
          "           write the returned value to OUTPUT\n"
          "       flywheel-lite debug|db SOURCE [options] [-- program arguments]\n"
          "           print the returned value and run time\n"
          "       flywheel-lite test|t [files or directories]\n"
          "           run test_*.fw and *_test.fw files\n"
          "       flywheel-lite disasm SOURCE [options]\n"
          "           print the compiled bytecode\n"
          "       flywheel-lite update|u [curl arguments]\n"
          "           reinstall from flywheel.polymorphia.dev\n"
          "       flywheel-lite repl | selftest\n"
          "       flywheel-lite help|h|--help | version|v|--version\n"
          "\n"
          "SOURCE is a .fw/.fly path, or - for standard input; `flywheel-lite SOURCE [arguments]` runs it.\n"
          "Use -s CODE (--source CODE) instead of SOURCE to run code from the command line.\n"
          "\n"
          "Options:\n"
          "  -O0..-O3         accepted for compatibility (no effect)\n"
          "  -g               accepted for compatibility (no effect)\n"
          "  -o FILE          output file (export)\n"
          "\n"
          "build (b), desktop (d), serve (s), format (fmt, f), doc, and lsp need the Flywheel compiler.\n"
          "fwl is a short alias: fwl SOURCE, fwl repl, ...\n",
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

/* ---------------------------------------------------------------- test */

typedef struct {
    char **items;
    int count, cap;
} PathList;
static void paths_push(PathList *l, char *p) {
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = xrealloc(l->items, sizeof(char *) * (size_t)l->cap);
    }
    l->items[l->count++] = p;
}
static int path_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static bool ends_with(const char *s, size_t n, const char *suffix) {
    size_t k = strlen(suffix);
    return n >= k && !memcmp(s + n - k, suffix, k);
}
/* test_*.fw and *_test.fw (also .fly), as `flywheel test` finds them. */
static bool is_test_file(const char *name) {
    size_t n = strlen(name), stem;
    if (ends_with(name, n, ".fw"))
        stem = n - 3;
    else if (ends_with(name, n, ".fly"))
        stem = n - 4;
    else
        return false;
    return !strncmp(name, "test_", 5) || (stem > 5 && !memcmp(name + stem - 5, "_test", 5));
}
static char *join_path(const char *dir, const char *name) {
    size_t d = strlen(dir);
    bool slash = d && (dir[d - 1] == '/' || dir[d - 1] == '\\');
    char *p = xmalloc(d + strlen(name) + 2);
    sprintf(p, "%s%s%s", dir, slash ? "" : "/", name);
    return p;
}
/* Finds test files under dir, skipping build directories and hidden ones. */
static void collect_tests(const char *dir, PathList *out) {
    PathList names = {0};
#ifdef _WIN32
    char *pattern = join_path(dir, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do
            paths_push(&names, xstrdup(fd.cFileName));
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            paths_push(&names, xstrdup(e->d_name));
        closedir(d);
    }
#endif
    for (int i = 0; i < names.count; i++) {
        const char *name = names.items[i];
        if (!strcmp(name, ".") || !strcmp(name, ".."))
            continue;
        char *path = join_path(dir, name);
        int kind = tb_path_kind(path);
        if (kind == 2 && strcmp(name, "build") && name[0] != '.')
            collect_tests(path, out);
        else if (kind == 1 && is_test_file(name)) {
            paths_push(out, path);
            continue;
        }
        free(path);
    }
    for (int i = 0; i < names.count; i++)
        free(names.items[i]);
    free(names.items);
}
static void add_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }

/* Runs each test file with a generated runner appended, like `flywheel test`: every top-level
 * test_* function without parameters runs between the setup/teardown fixtures, and the file
 * fails if any test throws. */
static int run_tests(PathList *paths, const char *exe_dir) {
    PathList files = {0};
    if (!paths->count)
        paths_push(paths, ".");
    for (int i = 0; i < paths->count; i++) {
        int kind = tb_path_kind(paths->items[i]);
        if (kind == 2) {
            collect_tests(paths->items[i], &files);
        } else if (kind == 1)
            paths_push(&files, xstrdup(paths->items[i]));
        else {
            fprintf(stderr, "flywheel-lite: no such test file or directory: %s\n", paths->items[i]);
            return 2;
        }
    }
    qsort(files.items, (size_t)files.count, sizeof(char *), path_cmp);
    if (!files.count) {
        fputs("flywheel-lite: no test files found (test_*.fw or *_test.fw)\n", stderr);
        return 1;
    }
    int failed = 0;
    for (int f = 0; f < files.count; f++) {
        const char *path = files.items[f];
        printf("%s\n", path);
        fflush(stdout);
        size_t len;
        char *src = read_path(path, &len);
        if (!src) {
            fprintf(stderr, "flywheel-lite: cannot read %s: %s\n", path, strerror(errno));
            failed++;
            continue;
        }
        Buf program;
        buf_init(&program);
        buf_add(&program, src, len);
        free(src);
        /* A file that does not parse gets no runner; running it reports the error. */
        Arena *arena = arena_new();
        Program prog;
        CompileError err;
        memset(&prog, 0, sizeof prog);
        memset(&err, 0, sizeof err);
        if (parse_program(arena, program.data, program.len, &prog, &err)) {
            bool fixture[4] = {false, false, false, false};
            static const char *fixtures[4] = {"setup", "teardown", "setup_all", "teardown_all"};
            int ntests = 0;
            for (int i = 0; i < prog.nfunctions; i++) {
                FuncDecl *fn = &prog.functions[i];
                if (fn->owner || fn->nparams || !fn->name)
                    continue;
                if (!strncmp(fn->name, "test_", 5))
                    ntests++;
                for (int k = 0; k < 4; k++)
                    if (!strcmp(fn->name, fixtures[k]))
                        fixture[k] = true;
            }
            add_str(&program, "\nint __fw_failed = 0\n");
            if (fixture[2])
                add_str(&program, "setup_all()\n");
            for (int i = 0; i < prog.nfunctions; i++) {
                FuncDecl *fn = &prog.functions[i];
                if (fn->owner || fn->nparams || !fn->name || strncmp(fn->name, "test_", 5))
                    continue;
                add_str(&program, "try\n");
                if (fixture[1])
                    add_str(&program, "    try\n");
                const char *indent = fixture[1] ? "        " : "    ";
                if (fixture[0]) {
                    add_str(&program, indent);
                    add_str(&program, "setup()\n");
                }
                add_str(&program, indent);
                add_str(&program, fn->name);
                add_str(&program, "()\n");
                if (fixture[1])
                    add_str(&program, "    finally\n        teardown()\n    end\n");
                add_str(&program, "    print \"  ok   ");
                add_str(&program, fn->name);
                add_str(&program, "\"\ncatch Error __fw_error\n    print \"  FAIL ");
                add_str(&program, fn->name);
                add_str(&program, ": \" + __fw_error.message\n    __fw_failed += 1\nend\n");
            }
            if (fixture[3])
                add_str(&program, "teardown_all()\n");
            char tail[160];
            snprintf(tail, sizeof tail,
                     "if __fw_failed > 0 do\n    throw AssertionError(string(__fw_failed) + \" of %d test(s) "
                     "failed\")\nend\n",
                     ntests);
            add_str(&program, tail);
        }
        arena_free(arena);
        VM *vm = xmalloc(sizeof(VM));
        vm_init(vm, 0, NULL);
        vm->exe_dir = exe_dir;
        Value result = val_null();
        RunResult r = vm_run_main(vm, path, program.data, program.len, &result);
        fflush(stdout);
        if (r == RUN_ERROR || (r == RUN_EXIT && vm->exit_code != 0))
            failed++;
        buf_free(&program);
    }
    printf("%d test file(s), %d failed\n", files.count, failed);
    return failed ? 1 : 0;
}

/* ---------------------------------------------------------------- main */

static bool is_source_option(const char *a) {
    return !strcmp(a, "-s") || !strcmp(a, "--source") || !strcmp(a, "-e") || !strncmp(a, "--source=", 9);
}

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
    /* Short aliases, as in the Flywheel compiler; fmt is the old name of format. */
    static const char *aliases[][2] = {
        {"r", "run"},    {"b", "build"},   {"c", "check"},  {"d", "desktop"}, {"s", "serve"},
        {"e", "export"}, {"db", "debug"},  {"t", "test"},   {"f", "format"},  {"fmt", "format"},
        {"u", "update"}, {"h", "help"},    {"v", "version"},
    };
    for (size_t i = 0; i < sizeof aliases / sizeof *aliases; i++)
        if (!strcmp(cmd, aliases[i][0])) {
            cmd = aliases[i][1];
            break;
        }
    if (!strcmp(cmd, "--version") || !strcmp(cmd, "-v") || !strcmp(cmd, "version")) {
        printf("flywheel-lite %s (Flywheel %s)\n", LITE_VERSION, LITE_LANGUAGE);
        return 0;
    }
    if (!strcmp(cmd, "--help") || !strcmp(cmd, "-h") || !strcmp(cmd, "help")) {
        usage(stdout);
        return 0;
    }
    static const char *compiler_only[] = {"build", "desktop", "serve", "format", "doc", "lsp"};
    for (size_t i = 0; i < sizeof compiler_only / sizeof *compiler_only; i++)
        if (!strcmp(cmd, compiler_only[i])) {
            fprintf(stderr, "flywheel-lite: `%s` needs the Flywheel compiler: use `flywheel %s`\n", cmd, cmd);
            return 2;
        }
    if (!strcmp(cmd, "new") || !strcmp(cmd, "get") || !strcmp(cmd, "add") || !strcmp(cmd, "install")) {
        fprintf(stderr, "flywheel-lite: packages are managed by Pulley: use `pulley %s` (or `ppm %s`)\n", cmd, cmd);
        return 2;
    }
    if (!strcmp(cmd, "update")) {
        /* curl -fsSL https://flywheel.polymorphia.dev/lite-install.sh [arguments] | bash */
#ifndef _WIN32
        const char *script = "curl -fsSL https://flywheel.polymorphia.dev/lite-install.sh \"$@\" | bash";
#endif
        fflush(stdout);
#ifdef _WIN32
        Buf b; /* cmd.exe runs the pipeline */
        buf_init(&b);
        add_str(&b, "curl -fsSL https://flywheel.polymorphia.dev/lite-install.sh");
        for (int i = 2; i < argc; i++) {
            add_str(&b, " \"");
            add_str(&b, argv[i]);
            buf_addc(&b, '"');
        }
        add_str(&b, " | bash");
        int status = system(b.data);
        return status == -1 ? 1 : status;
#else
        char **args = xmalloc(sizeof(char *) * (size_t)(argc + 3));
        int n = 0;
        args[n++] = "sh";
        args[n++] = "-c";
        args[n++] = (char *)script;
        args[n++] = "sh";
        for (int i = 2; i < argc; i++)
            args[n++] = argv[i];
        args[n] = NULL;
        execvp("sh", args);
        fprintf(stderr, "flywheel-lite: update needs sh, curl, and bash: %s\n", strerror(errno));
        return 1;
#endif
    }
    const char *exe_dir = exe_dir_of(argv[0]);
    if (!strcmp(cmd, "selftest")) {
#ifdef LITE_TEST_DIR
#ifdef _WIN32
        const char *python = "python";
#else
        const char *python = "python3";
#endif
        Buf b;
        buf_init(&b);
        add_str(&b, python);
        add_str(&b, " \"" LITE_TEST_DIR "/run_tests.py\" \"");
        add_str(&b, argv[0]);
        buf_addc(&b, '"');
        fflush(stdout);
        int status = system(b.data);
        if (status == -1) {
            fprintf(stderr, "flywheel-lite: cannot run %s; Python 3 is required for the test harness\n", python);
            return 1;
        }
#ifndef _WIN32
        if (WIFEXITED(status))
            status = WEXITSTATUS(status);
#endif
        return status;
#else
        fputs("flywheel-lite: selftest needs a build from a source checkout\n", stderr);
        return 1;
#endif
    }
    if (!strcmp(cmd, "test")) {
        PathList paths = {0};
        for (int i = 2; i < argc; i++) {
            const char *a = argv[i];
            if (strlen(a) == 3 && a[0] == '-' && a[1] == 'O' && a[2] >= '0' && a[2] <= '3')
                continue;
            if (!strcmp(a, "-g"))
                continue;
            if (!strcmp(a, "--coverage") || !strcmp(a, "--race")) {
                fprintf(stderr, "flywheel-lite: %s needs the Flywheel compiler: use `flywheel test %s`\n", a, a);
                return 2;
            }
            if (a[0] == '-') {
                fprintf(stderr, "flywheel-lite: unknown or incomplete option: %s\n", a);
                return 2;
            }
            paths_push(&paths, (char *)a);
        }
        return run_tests(&paths, exe_dir);
    }

    enum { RUN, CHECK, DEBUG, EXPORT, DISASM, REPL } mode = RUN;
    bool implicit = false; /* flywheel-lite SOURCE [ARGS...]: everything after SOURCE goes to the program */
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
    else if (cmd[0] == '-' && cmd[1] && !is_source_option(cmd)) {
        usage(stderr);
        return 2;
    } else
        implicit = !is_source_option(cmd); /* flywheel-lite -s CODE behaves as flywheel-lite run -s CODE */

    VM *vm = xmalloc(sizeof(VM));
    if (mode == REPL) {
        vm_init(vm, 0, NULL);
        vm->exe_dir = exe_dir;
        return repl(vm);
    }

    const char *path = NULL, *code = NULL, *output = NULL;
    int nprogram = 0;
    char **program_args = xmalloc(sizeof(char *) * (size_t)argc);
    if (implicit) {
        path = argv[1];
        int i = 2;
        if (i < argc && !strcmp(argv[i], "--"))
            i++;
        for (; i < argc; i++)
            program_args[nprogram++] = argv[i];
    } else {
        /* Options may appear anywhere before `--`, as with the Flywheel compiler. */
        const char **positionals = xmalloc(sizeof(char *) * (size_t)argc);
        int npos = 0;
        char **rest = NULL;
        int nrest = 0;
        for (int i = cmd[0] == '-' ? 1 : 2; i < argc; i++) {
            const char *a = argv[i];
            if (!strcmp(a, "--")) {
                rest = argv + i + 1;
                nrest = argc - i - 1;
                break;
            }
            if (!strncmp(a, "--source=", 9)) {
                code = a + 9;
                continue;
            }
            if (is_source_option(a) && i + 1 < argc) {
                code = argv[++i];
                continue;
            }
            if (strlen(a) == 3 && a[0] == '-' && a[1] == 'O' && a[2] >= '0' && a[2] <= '3')
                continue;
            if (!strcmp(a, "-g"))
                continue;
            if (!strcmp(a, "-o") && i + 1 < argc && mode == EXPORT) {
                output = argv[++i];
                continue;
            }
            if (!strcmp(a, "-") || a[0] != '-') {
                positionals[npos++] = a;
                continue;
            }
            fprintf(stderr, "flywheel-lite: unknown or incomplete option: %s\n", a);
            return 2;
        }
        int next = 0;
        if (!code && next < npos)
            path = positionals[next++];
        if (mode == EXPORT && !output && next < npos)
            output = positionals[next++];
        while (next < npos)
            program_args[nprogram++] = (char *)positionals[next++];
        for (int i = 0; i < nrest; i++)
            program_args[nprogram++] = rest[i];
        if (!path && !code) {
            usage(stderr);
            return 2;
        }
        if (mode == EXPORT && !output) {
            fputs("flywheel-lite: export needs an output file\n", stderr);
            return 2;
        }
    }

    char *source = NULL;
    size_t len = 0;
    if (code) {
        path = "<source>";
        source = xstrdup(code);
        len = strlen(code);
    } else if (!strcmp(path, "-")) {
        source = read_all(stdin, &len);
        path = "<stdin>";
    } else {
        source = read_path(path, &len);
        if (!source) {
            fprintf(stderr, "flywheel-lite: cannot read %s: %s\n", path, strerror(errno));
            return 1;
        }
    }
    vm_init(vm, nprogram, program_args);
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
    int code_ = finish(vm, r, result);
    /* Leave the heap to the OS: freeing every object at exit only costs time. */
    return code_;
}
