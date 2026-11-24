/* flywheel-lite: runs Flywheel source files directly on a bytecode virtual machine. */
#include "front.h"
#include "opcodes.h"
#include "lite.h"
#include <errno.h>
#include <sys/stat.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#include <process.h>
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
    fputs("Usage: flywheel-lite [flags] SOURCE [program arguments] [-- program arguments]\n"
          "\n"
          "SOURCE is a .fw/.fly file, a directory with a source.fw, or - for standard input. Without\n"
          "a mode, SOURCE is run. Short flags combine: `flywheel-lite main.fw -vt`. fwl is a short\n"
          "name for flywheel-lite.\n"
          "\n"
          "Modes:\n"
          "  -r, --run            run on the bytecode VM (the default)\n"
          "  -c, --check          compile without running\n"
          "  -e, --export         write the returned value to OUTPUT (-o OUTPUT, or the next\n"
          "                       argument)\n"
          "  -D, --debug          print the returned value and run time\n"
          "  -t, --test           test mode: run the file's test_* functions first (combines with\n"
          "                       the other modes); alone, tests every test_*.fw and *_test.fw in\n"
          "                       the given files and directories (default .)\n"
          "  -n, --new NAME       create a project with Pulley (--game: a game)\n"
          "      --disasm         print the compiled bytecode\n"
          "      --repl           interactive session\n"
          "      --selftest       run Flywheel Lite's test suite\n"
          "  -E, --error SOURCE   print the last error SOURCE stopped with: its message and\n"
          "                       locations, without the source lines\n"
          "\n"
          "Run first, in this order:\n"
          "  -u, --update         reinstall from flywheel.polymorphia.dev\n"
          "  -v, --version        print the version\n"
          "  -h, --help           print this help\n"
          "\n"
          "Options:\n"
          "  -x, --source CODE    run CODE instead of SOURCE\n"
          "  -o, --output FILE    output file (export)\n"
          "  -O0..-O3, -g, --hot  accepted for compatibility (no effect)\n"
          "\n"
          "-b/--build, -d/--desktop, -s/--serve, -f/--format, --doc, --lsp, --race, --coverage, and\n"
          "--emit-ir need the Flywheel compiler.\n",
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
        case OP_PICK:
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
        case OP_SET_PATH:
        case OP_SET_MEMBER: {
            int k = U16();
            i += 2;
            printf(" %s", k < c->constants.count && IS_STRING(c->constants.items[k]) ? AS_CSTR(c->constants.items[k]) : "?");
            if (op == OP_SET_PATH) {
                int n = c->code[i++];
                int off = U16();
                printf(" pop %d -> %d", n, i + off);
            }
            break;
        }
        case OP_INVOKE: {
            int k = U16();
            int argc = c->code[i++];
            i += 2;
            printf(" %s/%d", AS_CSTR(c->constants.items[k]), argc);
            break;
        }
        case OP_SUPER_INVOKE:
        case OP_SUPER_INVOKE_AT: {
            int k = U16();
            int argc = c->code[i++];
            printf(" %s/%d", AS_CSTR(c->constants.items[k]), argc);
            break;
        }
        case OP_SUPER_GET:
        case OP_DEFINE_STATIC: {
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
        if (!vm->report)
            vm->report = diag_new();
        diag_compile_error(vm->report, path, err.line, err.col, err.message, src, len);
        diag_record(path, vm->report);
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
/* test_*.fw and *_test.fw (also .fly), as `flywheel --test` finds them. */
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

/* Test mode: appends a function that calls every top-level test_* function without parameters
 * between the setup/teardown fixtures and fails if any throws. The compiler calls it before the
 * program's first top-level `return`, or at its end (vm->test_mode). */
static void add_test_runner(Buf *program) {
    /* A file that does not parse gets no runner; running it reports the error. */
    Arena *arena = arena_new();
    Program prog;
    CompileError err;
    memset(&prog, 0, sizeof prog);
    memset(&err, 0, sizeof err);
    buf_addc(program, 0); /* the lexer and excerpts read NUL-terminated text */
    program->len--;
    if (parse_program(arena, program->data, program->len, &prog, &err)) {
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
        add_str(program, "\nfunction __fw_run_tests() returns void\nint __fw_failed = 0\n");
        if (fixture[2])
            add_str(program, "setup_all()\n");
        for (int i = 0; i < prog.nfunctions; i++) {
            FuncDecl *fn = &prog.functions[i];
            if (fn->owner || fn->nparams || !fn->name || strncmp(fn->name, "test_", 5))
                continue;
            const char *indent = fixture[1] ? "        " : "    ";
            add_str(program, "try\n");
            if (fixture[1])
                add_str(program, "    try\n");
            if (fixture[0])
                buf_printf(program, "%ssetup()\n", indent);
            buf_printf(program, "%s%s()\n", indent, fn->name);
            if (fixture[1])
                add_str(program, "    finally\n        teardown()\n    end\n");
            buf_printf(program,
                       "    print \"  ok   %s\"\ncatch Error __fw_error\n    print \"  FAIL %s: \" + "
                       "__fw_error.message\n    __fw_failed += 1\nend\n",
                       fn->name, fn->name);
        }
        if (fixture[3])
            add_str(program, "teardown_all()\n");
        buf_printf(program,
                   "if __fw_failed > 0 do\n    throw AssertionError(string(__fw_failed) + \" of %d test(s) "
                   "failed\")\nend\nend\n",
                   ntests);
    }
    arena_free(arena);
}

/* Runs each test file in test mode, like `flywheel --test`. */
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
        add_test_runner(&program);
        VM *vm = xmalloc(sizeof(VM));
        vm_init(vm, 0, NULL);
        vm->exe_dir = exe_dir;
        vm->test_mode = true;
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

/* The parsed command line, as the Flywheel compiler's: every command is a flag. Modes say what to
 * do with SOURCE, the rest adjust it, and short flags combine (`-vt`). */
enum {
    M_RUN = 1, M_CHECK = 2, M_EXPORT = 4, M_DEBUG = 8, M_TEST = 16, M_NEW = 32, M_DISASM = 64,
    M_REPL = 128, M_SELFTEST = 256, M_BUILD = 512, M_DESKTOP = 1024, M_SERVE = 2048, M_FORMAT = 4096,
    M_DOC = 8192, M_LSP = 16384
};
#define M_COMPILER (M_BUILD | M_DESKTOP | M_SERVE | M_FORMAT | M_DOC | M_LSP)
static const struct {
    char letter;
    const char *name;
    int mode;
} modes[] = {
    {'r', "run", M_RUN},         {'b', "build", M_BUILD},   {'c', "check", M_CHECK},   {'d', "desktop", M_DESKTOP},
    {'s', "serve", M_SERVE},     {'e', "export", M_EXPORT}, {'D', "debug", M_DEBUG},   {'t', "test", M_TEST},
    {'f', "format", M_FORMAT},   {'n', "new", M_NEW},       {0, "doc", M_DOC},         {0, "repl", M_REPL},
    {0, "lsp", M_LSP},           {0, "selftest", M_SELFTEST}, {0, "disasm", M_DISASM}, {0, "built", M_BUILD},
};
#define NMODES (sizeof modes / sizeof *modes)
typedef struct {
    int modes;
    bool update, version, help, game, last_error, hot;
    const char *code, *output;
    const char *compiler_option; /* an option only the compiler has */
    PathList positionals, rest;  /* rest: program arguments after -- */
} Cli;

static bool cli_fail(const char *fmt, const char *arg) {
    fputs("flywheel-lite: ", stderr);
    fprintf(stderr, fmt, arg);
    fputs(" (see flywheel-lite --help)\n", stderr);
    return false;
}
static bool parse_long(Cli *cli, int argc, char **argv, int *i) {
    const char *arg = argv[*i], *name = arg + 2, *equals = strchr(name, '=');
    char key[64];
    snprintf(key, sizeof key, "%.*s", equals ? (int)(equals - name) : (int)strlen(name), name);
    static const char *valued[] = {"source", "output", "emit-ir", "return-file", "port", "host", NULL};
    for (int k = 0; valued[k]; k++)
        if (!strcmp(key, valued[k])) {
            const char *v = equals ? equals + 1 : *i + 1 < argc ? argv[++*i] : NULL;
            if (!v)
                return cli_fail("%s expects a value", arg);
            if (k == 0)
                cli->code = v;
            else if (k == 1)
                cli->output = v;
            else
                cli->compiler_option = arg;
            return true;
        }
    if (equals)
        return cli_fail("unknown option %s", arg);
    for (size_t k = 0; k < NMODES; k++)
        if (!strcmp(key, modes[k].name)) {
            cli->modes |= modes[k].mode;
            return true;
        }
    struct {
        const char *name;
        bool *flag;
    } flags[] = {{"update", &cli->update}, {"version", &cli->version}, {"help", &cli->help},
                 {"game", &cli->game},     {"error", &cli->last_error}, {"hot", &cli->hot}};
    for (size_t k = 0; k < sizeof flags / sizeof *flags; k++)
        if (!strcmp(key, flags[k].name)) {
            *flags[k].flag = true;
            return true;
        }
    if (strcmp(key, "race") && strcmp(key, "coverage"))
        return cli_fail("unknown option %s", arg);
    cli->compiler_option = arg;
    return true;
}
/* Short flags: -vt, -O2g, -o FILE or -oFILE, -x CODE. */
static bool parse_short(Cli *cli, int argc, char **argv, int *i) {
    const char *arg = argv[*i];
    for (const char *p = arg + 1; *p; p++) {
        bool mode = false;
        for (size_t k = 0; k < NMODES && !mode; k++)
            if (modes[k].letter == *p) {
                cli->modes |= modes[k].mode;
                mode = true;
            }
        if (mode)
            continue;
        switch (*p) {
        case 'u': cli->update = true; break;
        case 'v': cli->version = true; break;
        case 'h': cli->help = true; break;
        case 'E': cli->last_error = true; break;
        case 'g': break;
        case 'O':
            if (p[1] < '0' || p[1] > '3')
                return cli_fail("-O expects a level 0..3 (-O2)%s", "");
            p++;
            break;
        case 'o':
        case 'x': {
            const char *v = p[1] ? p + 1 : *i + 1 < argc ? argv[++*i] : NULL;
            if (!v)
                return cli_fail(*p == 'o' ? "-o expects a value%s" : "-x expects a value%s", "");
            if (*p == 'o')
                cli->output = v;
            else
                cli->code = v;
            return true;
        }
        default: {
            char what[2] = {*p, 0}, shown[128];
            snprintf(shown, sizeof shown, "-%s%s%.80s", what, arg[2] ? " in " : "", arg[2] ? arg : "");
            return cli_fail("unknown option %s", shown);
        }
        }
    }
    return true;
}
static bool parse_cli(Cli *cli, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "--")) {
            while (++i < argc)
                paths_push(&cli->rest, argv[i]);
        } else if (a[0] != '-' || !a[1])
            paths_push(&cli->positionals, a); /* includes `-`, standard input */
        else if (!(a[1] == '-' ? parse_long(cli, argc, argv, &i) : parse_short(cli, argc, argv, &i)))
            return false;
    }
    return true;
}
/* Runs a program with the terminal's streams; its exit status, or -1 when it cannot start. */
static int run_program(char **args) {
    fflush(stdout);
#ifdef _WIN32
    return (int)_spawnvp(_P_WAIT, args[0], (const char *const *)args);
#else
    pid_t pid = fork();
    if (pid == 0) {
        execvp(args[0], args);
        _exit(127);
    }
    int status;
    if (pid < 0 || waitpid(pid, &status, 0) < 0)
        return -1;
    if (!WIFEXITED(status))
        return 128 + WTERMSIG(status);
    return WEXITSTATUS(status) == 127 ? -1 : WEXITSTATUS(status);
#endif
}
/* -u: reinstalls; with more on the command line, the rest then runs in the new flywheel-lite. */
static int update(char **argv, bool more) {
    char *install[] = {"sh", "-c", "curl -fsSL https://flywheel.polymorphia.dev/lite-install.sh | bash", NULL};
#ifdef _WIN32
    (void)install;
    fflush(stdout);
    int status = system("curl -fsSL https://flywheel.polymorphia.dev/lite-install.sh | bash");
#else
    int status = run_program(install);
#endif
    if (status == -1) {
        fputs("flywheel-lite: --update needs sh, curl, and bash\n", stderr);
        return 1;
    }
    if (status || !more)
        return status;
#ifdef _WIN32
    _putenv_s("FLYWHEEL_LITE_UPDATED", "1");
#else
    setenv("FLYWHEEL_LITE_UPDATED", "1", 1);
#endif
    status = run_program(argv);
    return status == -1 ? 1 : status;
}
/* A directory stands for its entry point: `flywheel-lite app/` runs app/source.fw. */
static const char *source_entry(const char *path) {
    if (tb_path_kind(path) != 2)
        return path;
    return join_path(path, "source.fw");
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
    Cli cli;
    memset(&cli, 0, sizeof cli);
    if (!parse_cli(&cli, argc, argv))
        return 2;
    PathList *pos = &cli.positionals;
    bool work = cli.modes || pos->count || cli.code;
    if (cli.update) {
        const char *updated = getenv("FLYWHEEL_LITE_UPDATED");
        if (!updated || !*updated)
            return update(argv, work || cli.version || cli.help || cli.last_error);
    }
    if (cli.version)
        printf("flywheel-lite %s (Flywheel %s)\n", LITE_VERSION, LITE_LANGUAGE);
    if (cli.help)
        usage(stdout);
    fflush(stdout);
    if ((cli.version || cli.help || cli.update) && !work)
        return 0;
    for (size_t k = 0; k < NMODES; k++)
        if (cli.modes & modes[k].mode & M_COMPILER) {
            fprintf(stderr, "flywheel-lite: --%s needs the Flywheel compiler: use `flywheel --%s`\n", modes[k].name,
                    modes[k].name);
            return 2;
        }
    if (cli.compiler_option) {
        fprintf(stderr, "flywheel-lite: %s needs the Flywheel compiler\n", cli.compiler_option);
        return 2;
    }
    bool test = (cli.modes & M_TEST) != 0;
    int mode = cli.modes & ~M_TEST;
    if (mode & (mode - 1)) {
        const char *sep = "flywheel-lite: ";
        for (size_t k = 0; k < NMODES; k++)
            if (mode & modes[k].mode) {
                fprintf(stderr, "%s--%s", sep, modes[k].name);
                sep = " and ";
            }
        fputs(" cannot be combined\n", stderr);
        return 2;
    }
    if (!mode)
        mode = M_RUN;
    if (cli.last_error) {
        if (pos->count != 1 || cli.modes || cli.code) {
            fputs("usage: flywheel-lite --error SOURCE\n", stderr);
            return 2;
        }
        return diag_show_last(source_entry(pos->items[0]));
    }
    const char *exe_dir = exe_dir_of(argv[0]);
    if (mode == M_SELFTEST) {
#ifdef LITE_TEST_DIR
#ifdef _WIN32
        char *args[] = {"python", "\"" LITE_TEST_DIR "/run_tests.py\"", argv[0], NULL};
#else
        char *args[] = {"python3", LITE_TEST_DIR "/run_tests.py", argv[0], NULL};
#endif
        int status = run_program(args);
        if (status == -1) {
            fputs("flywheel-lite: Python 3 is required for the test harness\n", stderr);
            return 1;
        }
        return status;
#else
        fputs("flywheel-lite: --selftest needs a build from a source checkout\n", stderr);
        return 1;
#endif
    }
    if (mode == M_NEW) {
        if (!pos->count) {
            fputs("usage: flywheel-lite --new [--game] NAME\n", stderr);
            return 2;
        }
        PathList args = {0};
        paths_push(&args, "pulley");
        paths_push(&args, "new");
        for (int i = 0; i < pos->count; i++)
            paths_push(&args, pos->items[i]);
        if (cli.game)
            paths_push(&args, "--game");
        paths_push(&args, NULL);
        int status = run_program(args.items);
        if (status == -1) {
            fputs("flywheel-lite: new projects are created by Pulley, which is not installed "
                  "(see https://flywheel.polymorphia.dev)\n",
                  stderr);
            return 2;
        }
        return status;
    }
    /* The commands became flags; say so rather than failing to find a file named `run`. */
    if (!cli.code && pos->count && tb_path_kind(pos->items[0]) == 0) {
        static const char *renamed[][2] = {
            {"r", "run"},    {"b", "build"},   {"c", "check"},   {"d", "desktop"}, {"s", "serve"},
            {"e", "export"}, {"db", "debug"},  {"t", "test"},    {"f", "format"},  {"fmt", "format"},
            {"u", "update"}, {"h", "help"},    {"v", "version"}, {"update", "update"}, {"help", "help"},
            {"version", "version"}};
        static const char *packages[] = {"get", "add", "install", "pull"};
        const char *first = pos->items[0], *name = NULL;
        for (size_t i = 0; i < sizeof packages / sizeof *packages; i++)
            if (!strcmp(first, packages[i])) {
                fputs("flywheel-lite: packages are managed by Pulley: use `pulley pull` (or `ppm pull`)\n", stderr);
                return 2;
            }
        for (size_t i = 0; i < sizeof renamed / sizeof *renamed; i++)
            if (!strcmp(first, renamed[i][0]))
                name = renamed[i][1];
        for (size_t k = 0; k < NMODES; k++)
            if (!strcmp(first, modes[k].name) && strcmp(first, "built"))
                name = modes[k].name;
        if (name) {
            fprintf(stderr, "flywheel-lite: commands are flags now: use `flywheel-lite --%s` (see flywheel-lite --help)\n",
                    name);
            return 2;
        }
    }
    if (mode == M_RUN && test && !cli.code)
        return run_tests(pos, exe_dir);

    VM *vm = xmalloc(sizeof(VM));
    if (mode == M_REPL) {
        vm_init(vm, 0, NULL);
        vm->exe_dir = exe_dir;
        return repl(vm);
    }
    /* Positionals: the source file (unless -x), then for export the output file if -o was not
     * given, then program arguments. */
    const char *path = NULL, *code = cli.code, *output = cli.output;
    int next = 0;
    if (!code && next < pos->count)
        path = pos->items[next++];
    if (mode == M_EXPORT && !output && next < pos->count)
        output = pos->items[next++];
    if (mode == M_CHECK && next < pos->count) {
        fprintf(stderr, "flywheel-lite: unexpected argument: %s\n", pos->items[next]);
        return 2;
    }
    if ((!path && !code) || (mode == M_EXPORT && !output)) {
        usage(stderr);
        return 2;
    }
    PathList program_args = {0};
    for (; next < pos->count; next++)
        paths_push(&program_args, pos->items[next]);
    for (int i = 0; i < cli.rest.count; i++)
        paths_push(&program_args, cli.rest.items[i]);
    if (cli.hot)
        fputs("flywheel-lite: --hot needs the Flywheel compiler; running without hot reload\n", stderr);

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
        const char *entry = source_entry(path);
        if (entry != path && tb_path_kind(entry) != 1) {
            fprintf(stderr, "flywheel-lite: %s is a directory without a source.fw entry point\n", path);
            return 2;
        }
        path = entry;
        source = read_path(path, &len);
        if (!source) {
            fprintf(stderr, "flywheel-lite: cannot read %s: %s\n", path, strerror(errno));
            return 1;
        }
    }
    if (test) {
        Buf program;
        buf_init(&program);
        buf_add(&program, source, len);
        add_test_runner(&program);
        buf_addc(&program, 0);
        source = program.data;
        len = program.len - 1;
    }
    vm_init(vm, program_args.count, program_args.items);
    vm->test_mode = test;
    vm->check_only = mode == M_CHECK || mode == M_DISASM;
    vm->exe_dir = exe_dir;
    if (mode == M_CHECK || mode == M_DISASM)
        return compile_only(vm, path, source, len, mode == M_DISASM);

    double start = 0;
    if (mode == M_DEBUG)
        start = (double)clock() / CLOCKS_PER_SEC;
    Value result = val_null();
    RunResult r = vm_run_main(vm, path, source, len, &result);
    fflush(stdout);
    if (r == RUN_ERROR && vm->report && !code && strcmp(path, "<stdin>") != 0)
        diag_record(path, vm->report);
    if (r == RUN_OK && (mode == M_DEBUG || mode == M_EXPORT)) {
        Buf b;
        buf_init(&b);
        if (!value_to_buf(vm, result, &b, false, 0)) {
            vm_report_error(vm, vm->error);
            if (!code && strcmp(path, "<stdin>") != 0)
                diag_record(path, vm->report);
            return 1;
        }
        if (mode == M_DEBUG) {
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
