/* Native library modules: math, fs, os, time, io, testing, collections, encoding, uuid, html,
 * gc. Platform code is isolated here (POSIX: Linux and macOS; Windows: the CRT and Win32). */
#include "lite.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define popen _popen
#define pclose _pclose
#else
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#define NATIVE(name) static bool name(VM *vm, int argc, Value *args, Value *out)

static Value sv(VM *vm, const char *s, size_t n) { return val_obj(str_copy(vm, s, n)); }
static Value svc(VM *vm, const char *s) { return val_obj(str_copy(vm, s, strlen(s))); }
static bool want_str(VM *vm, Value v, const char *fn) {
    if (IS_STRING(v))
        return true;
    return vm_throw(vm, "TypeError", "%s expects a string", fn);
}
#define STR(i, fn)                                                                                \
    if (!want_str(vm, args[i], fn))                                                               \
    return false
static bool num_arg(VM *vm, Value v, double *d, const char *fn) { return to_double(vm, v, d, fn); }

/* ---------------------------------------------------------------- platform helpers */

int tb_path_kind(const char *p) {
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(p, &st) != 0)
        return 0;
    return (st.st_mode & _S_IFDIR) ? 2 : 1;
#else
    struct stat st;
    if (stat(p, &st) != 0)
        return 0;
    return S_ISDIR(st.st_mode) ? 2 : 1;
#endif
}
static int64_t file_size(const char *p) {
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(p, &st) != 0)
        return -1;
    return (int64_t)st.st_size;
#else
    struct stat st;
    if (stat(p, &st) != 0)
        return -1;
    return (int64_t)st.st_size;
#endif
}
static int make_dir(const char *p) {
#ifdef _WIN32
    return _mkdir(p);
#else
    return mkdir(p, 0777);
#endif
}
static int remove_dir(const char *p) {
#ifdef _WIN32
    return _rmdir(p);
#else
    return rmdir(p);
#endif
}
static char *current_dir(void) {
    char buf[4096];
#ifdef _WIN32
    if (!_getcwd(buf, sizeof buf))
        return xstrdup(".");
    for (char *c = buf; *c; c++)
        if (*c == '\\')
            *c = '/';
#else
    if (!getcwd(buf, sizeof buf))
        return xstrdup(".");
#endif
    return xstrdup(buf);
}
/* Sorted directory entries (without . and ..). */
static ObjList *list_dir(VM *vm, const char *path, bool *ok) {
    ObjList *l = new_list(vm, OBJ_ARRAY, 0);
    push_root(vm, val_obj(l));
    *ok = true;
#ifdef _WIN32
    char pattern[4096];
    snprintf(pattern, sizeof pattern, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        *ok = false;
    } else {
        do {
            if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
                list_push(vm, l, svc(vm, fd.cFileName));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(path);
    if (!d) {
        *ok = false;
    } else {
        struct dirent *e;
        while ((e = readdir(d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
                list_push(vm, l, svc(vm, e->d_name));
        closedir(d);
    }
#endif
    pop_root(vm, 1);
    /* sort by bytes */
    for (int i = 1; i < l->count; i++) {
        Value x = l->items[i];
        int j = i - 1;
        while (j >= 0 && strcmp(AS_CSTR(l->items[j]), AS_CSTR(x)) > 0) {
            l->items[j + 1] = l->items[j];
            j--;
        }
        l->items[j + 1] = x;
    }
    return l;
}
static double now_monotonic(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}
static double now_wall(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (double)(t - 116444736000000000ull) / 1e7;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}
static void sleep_seconds(double s) {
    if (s <= 0)
        return;
#ifdef _WIN32
    Sleep((DWORD)(s * 1000.0));
#else
    struct timespec ts;
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
#endif
}
static bool tm_utc(time_t t, struct tm *out) {
#ifdef _WIN32
    return gmtime_s(out, &t) == 0;
#else
    return gmtime_r(&t, out) != NULL;
#endif
}
static bool tm_local(time_t t, struct tm *out) {
#ifdef _WIN32
    return localtime_s(out, &t) == 0;
#else
    return localtime_r(&t, out) != NULL;
#endif
}
static void secure_random(void *dst, size_t n) {
    unsigned char *p = dst;
#ifdef _WIN32
    for (size_t i = 0; i < n; i += 4) {
        unsigned int r;
        rand_s(&r);
        for (size_t k = 0; k < 4 && i + k < n; k++)
            p[i + k] = (unsigned char)(r >> (8 * k));
    }
#else
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got = f ? fread(p, 1, n, f) : 0;
    if (f)
        fclose(f);
    for (size_t i = got; i < n; i++)
        p[i] = (unsigned char)(rand() & 0xff);
#endif
}

/* ---------------------------------------------------------------- random numbers */

static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
static void seed_rng(VM *vm, uint64_t seed) {
    for (int i = 0; i < 4; i++) {
        seed += 0x9e3779b97f4a7c15ull;
        uint64_t z = seed;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        vm->rng[i] = z ^ (z >> 31);
    }
    vm->rng_seeded = true;
}
uint64_t rng_next(VM *vm) {
    if (!vm->rng_seeded) {
        uint64_t s;
        secure_random(&s, sizeof s);
        seed_rng(vm, s ^ (uint64_t)(now_wall() * 1e6));
    }
    uint64_t *r = vm->rng;
    uint64_t result = rotl(r[1] * 5, 7) * 9, t = r[1] << 17;
    r[2] ^= r[0];
    r[3] ^= r[1];
    r[1] ^= r[2];
    r[0] ^= r[3];
    r[2] ^= t;
    r[3] = rotl(r[3], 45);
    return result;
}

/* ---------------------------------------------------------------- math */

#define MATH1(name, expr)                                                                         \
    NATIVE(name) {                                                                                \
        (void)argc;                                                                               \
        double x;                                                                                 \
        if (!num_arg(vm, args[0], &x, #name))                                                     \
            return false;                                                                         \
        *out = val_double(expr);                                                                  \
        return true;                                                                              \
    }
MATH1(m_sqrt, sqrt(x))
MATH1(m_sin, sin(x))
MATH1(m_cos, cos(x))
MATH1(m_tan, tan(x))
MATH1(m_asin, asin(x))
MATH1(m_acos, acos(x))
MATH1(m_atan, atan(x))
MATH1(m_exp, exp(x))
MATH1(m_log10, log10(x))
MATH1(m_log2, log2(x))
MATH1(m_floor, floor(x))
MATH1(m_ceil, ceil(x))
MATH1(m_round, round(x))
MATH1(m_trunc, trunc(x))
MATH1(m_cbrt, cbrt(x))
NATIVE(m_log) {
    double x, b;
    if (!num_arg(vm, args[0], &x, "log"))
        return false;
    if (argc > 1) {
        if (!num_arg(vm, args[1], &b, "log"))
            return false;
        *out = val_double(log(x) / log(b));
    } else
        *out = val_double(log(x));
    return true;
}
#define MATH2(name, expr)                                                                         \
    NATIVE(name) {                                                                                \
        (void)argc;                                                                               \
        double x, y;                                                                              \
        if (!num_arg(vm, args[0], &x, #name) || !num_arg(vm, args[1], &y, #name))                 \
            return false;                                                                         \
        *out = val_double(expr);                                                                  \
        return true;                                                                              \
    }
MATH2(m_atan2, atan2(x, y))
MATH2(m_hypot, hypot(x, y))
MATH2(m_pow, pow(x, y))
NATIVE(m_is_nan) {
    (void)argc;
    double x;
    if (!num_arg(vm, args[0], &x, "is_nan"))
        return false;
    *out = val_bool(isnan(x));
    return true;
}
NATIVE(m_is_finite) {
    (void)argc;
    double x;
    if (!num_arg(vm, args[0], &x, "is_finite"))
        return false;
    *out = val_bool(isfinite(x));
    return true;
}
NATIVE(m_abs) {
    (void)argc;
    if (IS_INT(args[0])) {
        if (args[0].as.i == INT64_MIN)
            return vm_throw(vm, "NumericError", "abs of the minimum integer overflows");
        *out = val_int(args[0].as.i < 0 ? -args[0].as.i : args[0].as.i);
        return true;
    }
    double x;
    if (!num_arg(vm, args[0], &x, "abs"))
        return false;
    *out = val_double(fabs(x));
    return true;
}
static bool min_max(VM *vm, int argc, Value *args, Value *out, bool want_max) {
    Value *items = args;
    int n = argc;
    if (argc == 1 && IS_LIST(args[0])) {
        items = AS_LIST(args[0])->items;
        n = AS_LIST(args[0])->count;
    }
    if (n == 0)
        return vm_throw(vm, "ValueError", "%s of nothing", want_max ? "max" : "min");
    Value best = items[0];
    for (int i = 1; i < n; i++) {
        int c;
        if (!value_compare(vm, items[i], best, &c))
            return false;
        if (want_max ? c > 0 : c < 0)
            best = items[i];
    }
    *out = best;
    return true;
}
NATIVE(m_min) { return min_max(vm, argc, args, out, false); }
NATIVE(m_max) { return min_max(vm, argc, args, out, true); }
NATIVE(m_clamp) {
    (void)argc;
    int c;
    Value v = args[0];
    if (!value_compare(vm, v, args[1], &c))
        return false;
    if (c < 0)
        v = args[1];
    if (!value_compare(vm, v, args[2], &c))
        return false;
    if (c > 0)
        v = args[2];
    *out = v;
    return true;
}
NATIVE(m_random) {
    (void)argc;
    (void)args;
    *out = val_double((double)(rng_next(vm) >> 11) * (1.0 / 9007199254740992.0));
    return true;
}
NATIVE(m_random_int) {
    (void)argc;
    if (!IS_INT(args[0]) || !IS_INT(args[1]))
        return vm_throw(vm, "TypeError", "random_int expects ints");
    int64_t lo = args[0].as.i, hi = args[1].as.i;
    if (hi < lo)
        return vm_throw(vm, "ValueError", "random_int: high is below low");
    uint64_t span = (uint64_t)hi - (uint64_t)lo + 1;
    uint64_t r = span ? rng_next(vm) % span : rng_next(vm);
    *out = val_int((int64_t)((uint64_t)lo + r));
    return true;
}
NATIVE(m_seed) {
    (void)argc;
    int64_t s;
    if (!to_int64(vm, args[0], &s, "seed"))
        return false;
    seed_rng(vm, (uint64_t)s);
    *out = val_null();
    return true;
}

static void make_math(VM *vm, ObjModule *m) {
    module_set_value(vm, m, "PI", val_double(3.14159265358979323846));
    module_set_value(vm, m, "TAU", val_double(6.28318530717958647692));
    module_set_value(vm, m, "E", val_double(2.71828182845904523536));
    module_set_value(vm, m, "INF", val_double(INFINITY));
    module_set_value(vm, m, "NAN", val_double(NAN));
    struct {
        const char *n;
        NativeFn f;
        int a, b;
    } fns[] = {{"sqrt", m_sqrt, 1, 1},   {"sin", m_sin, 1, 1},       {"cos", m_cos, 1, 1},
               {"tan", m_tan, 1, 1},     {"asin", m_asin, 1, 1},     {"acos", m_acos, 1, 1},
               {"atan", m_atan, 1, 1},   {"atan2", m_atan2, 2, 2},   {"hypot", m_hypot, 2, 2},
               {"exp", m_exp, 1, 1},     {"log", m_log, 1, 2},       {"log10", m_log10, 1, 1},
               {"log2", m_log2, 1, 1},   {"pow", m_pow, 2, 2},       {"floor", m_floor, 1, 1},
               {"ceil", m_ceil, 1, 1},   {"round", m_round, 1, 1},   {"trunc", m_trunc, 1, 1},
               {"cbrt", m_cbrt, 1, 1},   {"is_nan", m_is_nan, 1, 1}, {"is_finite", m_is_finite, 1, 1},
               {"abs", m_abs, 1, 1},     {"min", m_min, 1, -1},      {"max", m_max, 1, -1},
               {"clamp", m_clamp, 3, 3}, {"random", m_random, 0, 0}, {"random_int", m_random_int, 2, 2},
               {"seed", m_seed, 1, 1},   {NULL, NULL, 0, 0}};
    for (int i = 0; fns[i].n; i++)
        module_set_native(vm, m, fns[i].n, fns[i].f, fns[i].a, fns[i].b);
}

/* ---------------------------------------------------------------- fs */

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    Buf b;
    buf_init(&b);
    char tmp[16384];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0)
        buf_add(&b, tmp, n);
    fclose(f);
    if (!b.data)
        buf_add(&b, "", 0);
    *len = b.len;
    return b.data;
}
static bool file_error(VM *vm, const char *what, const char *path) {
    return vm_throw(vm, "FileError", "%s '%s'", what, path);
}
NATIVE(fs_exists) {
    (void)argc;
    STR(0, "fs.exists");
    *out = val_bool(tb_path_kind(AS_CSTR(args[0])) != 0);
    return true;
}
NATIVE(fs_is_dir) {
    (void)argc;
    STR(0, "fs.is_dir");
    *out = val_bool(tb_path_kind(AS_CSTR(args[0])) == 2);
    return true;
}
NATIVE(fs_is_file) {
    (void)argc;
    STR(0, "fs.is_file");
    *out = val_bool(tb_path_kind(AS_CSTR(args[0])) == 1);
    return true;
}
NATIVE(fs_read) {
    (void)argc;
    STR(0, "fs.read");
    size_t len;
    if (tb_path_kind(AS_CSTR(args[0])) == 2) {
        errno = EISDIR;
        return file_error(vm, "cannot read", AS_CSTR(args[0]));
    }
    char *text = slurp(AS_CSTR(args[0]), &len);
    if (!text)
        return file_error(vm, "cannot read", AS_CSTR(args[0]));
    *out = val_obj(str_take_buf(vm, text, len));
    return true;
}
NATIVE(fs_try_read) {
    (void)argc;
    STR(0, "fs.try_read");
    size_t len;
    char *text = slurp(AS_CSTR(args[0]), &len);
    if (!text) {
        ObjString *m = str_fmt(vm, "cannot read '%s'", AS_CSTR(args[0]));
        push_root(vm, val_obj(m));
        ObjError *e = new_error(vm, str_intern_c(vm, "FileError"), m);
        pop_root(vm, 1);
        *out = val_obj(new_result(vm, false, val_obj(e)));
        return true;
    }
    Value s = val_obj(str_take_buf(vm, text, len));
    *out = val_obj(new_result(vm, true, s));
    return true;
}
static bool write_file(VM *vm, Value *args, const char *mode) {
    STR(0, "fs.write");
    FILE *f = fopen(AS_CSTR(args[0]), mode);
    if (!f)
        return file_error(vm, "cannot write", AS_CSTR(args[0]));
    bool ok = lib_print_value(vm, f, args[1]);
    if (fclose(f) != 0 && ok)
        return file_error(vm, "cannot write", AS_CSTR(args[0]));
    return ok;
}
NATIVE(fs_write) {
    (void)argc;
    *out = val_null();
    return write_file(vm, args, "wb");
}
NATIVE(fs_append) {
    (void)argc;
    *out = val_null();
    return write_file(vm, args, "ab");
}
NATIVE(fs_size) {
    (void)argc;
    STR(0, "fs.size");
    int64_t n = file_size(AS_CSTR(args[0]));
    if (n < 0)
        return file_error(vm, "cannot stat", AS_CSTR(args[0]));
    *out = val_int(n);
    return true;
}
NATIVE(fs_list) {
    (void)argc;
    STR(0, "fs.list");
    bool ok;
    ObjList *l = list_dir(vm, AS_CSTR(args[0]), &ok);
    if (!ok)
        return file_error(vm, "cannot list", AS_CSTR(args[0]));
    *out = val_obj(l);
    return true;
}
static bool walk_into(VM *vm, const char *root, const char *rel, ObjList *acc) {
    char path[4096];
    snprintf(path, sizeof path, "%s%s%s", root, rel[0] ? "/" : "", rel);
    bool ok;
    ObjList *names = list_dir(vm, path, &ok);
    if (!ok)
        return file_error(vm, "cannot list", path);
    push_root(vm, val_obj(names));
    for (int i = 0; i < names->count; i++) {
        char sub[4096];
        snprintf(sub, sizeof sub, "%.2000s%s%.1000s", rel, rel[0] ? "/" : "", AS_CSTR(names->items[i]));
        list_push(vm, acc, svc(vm, sub));
        char full[4096];
        snprintf(full, sizeof full, "%.1000s/%.3000s", root, sub);
        if (tb_path_kind(full) == 2 && !walk_into(vm, root, sub, acc)) {
            pop_root(vm, 1);
            return false;
        }
    }
    pop_root(vm, 1);
    return true;
}
NATIVE(fs_walk) {
    (void)argc;
    STR(0, "fs.walk");
    ObjList *acc = new_list(vm, OBJ_ARRAY, 0);
    push_root(vm, val_obj(acc));
    bool ok = walk_into(vm, AS_CSTR(args[0]), "", acc);
    pop_root(vm, 1);
    *out = val_obj(acc);
    return ok;
}
static bool mkdirs(const char *p) {
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", p);
    size_t n = strlen(buf);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == '\\' || buf[i] == 0) {
            char c = buf[i];
            buf[i] = 0;
            if (buf[0] && tb_path_kind(buf) == 0 && make_dir(buf) != 0 && errno != EEXIST)
                return false;
            buf[i] = c;
        }
    }
    return tb_path_kind(p) == 2;
}
NATIVE(fs_mkdir) {
    (void)argc;
    STR(0, "fs.mkdir");
    if (!mkdirs(AS_CSTR(args[0])))
        return file_error(vm, "cannot create directory", AS_CSTR(args[0]));
    *out = val_null();
    return true;
}
static bool copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb");
    if (!a)
        return false;
    FILE *b = fopen(to, "wb");
    if (!b) {
        fclose(a);
        return false;
    }
    char tmp[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(tmp, 1, sizeof tmp, a)) > 0)
        if (fwrite(tmp, 1, n, b) != n)
            ok = false;
    fclose(a);
    if (fclose(b) != 0)
        ok = false;
    return ok;
}
static bool copy_tree(VM *vm, const char *from, const char *to) {
    if (tb_path_kind(from) != 2)
        return copy_file(from, to) ? true : file_error(vm, "cannot copy", from);
    if (!mkdirs(to))
        return file_error(vm, "cannot create directory", to);
    bool ok;
    ObjList *names = list_dir(vm, from, &ok);
    if (!ok)
        return file_error(vm, "cannot list", from);
    push_root(vm, val_obj(names));
    for (int i = 0; i < names->count; i++) {
        char a[4096], b[4096];
        snprintf(a, sizeof a, "%s/%s", from, AS_CSTR(names->items[i]));
        snprintf(b, sizeof b, "%s/%s", to, AS_CSTR(names->items[i]));
        if (!copy_tree(vm, a, b)) {
            pop_root(vm, 1);
            return false;
        }
    }
    pop_root(vm, 1);
    return true;
}
NATIVE(fs_copy) {
    (void)argc;
    STR(0, "fs.copy");
    STR(1, "fs.copy");
    *out = val_null();
    return copy_tree(vm, AS_CSTR(args[0]), AS_CSTR(args[1]));
}
NATIVE(fs_move) {
    (void)argc;
    STR(0, "fs.move");
    STR(1, "fs.move");
#ifdef _WIN32
    remove(AS_CSTR(args[1]));
#endif
    if (rename(AS_CSTR(args[0]), AS_CSTR(args[1])) != 0)
        return file_error(vm, "cannot move", AS_CSTR(args[0]));
    *out = val_null();
    return true;
}
NATIVE(fs_remove) {
    (void)argc;
    STR(0, "fs.remove");
    const char *p = AS_CSTR(args[0]);
    int r = tb_path_kind(p) == 2 ? remove_dir(p) : remove(p);
    if (r != 0)
        return file_error(vm, "cannot remove", p);
    *out = val_null();
    return true;
}
static int64_t remove_tree(VM *vm, const char *p, bool *ok) {
    int64_t count = 0;
    if (tb_path_kind(p) == 2) {
        bool lok;
        ObjList *names = list_dir(vm, p, &lok);
        push_root(vm, val_obj(names));
        for (int i = 0; i < names->count && *ok; i++) {
            char sub[4096];
            snprintf(sub, sizeof sub, "%s/%s", p, AS_CSTR(names->items[i]));
            count += remove_tree(vm, sub, ok);
        }
        pop_root(vm, 1);
        if (remove_dir(p) != 0)
            *ok = false;
        else
            count++;
    } else if (tb_path_kind(p) == 1) {
        if (remove(p) != 0)
            *ok = false;
        else
            count++;
    }
    return count;
}
NATIVE(fs_remove_all) {
    (void)argc;
    STR(0, "fs.remove_all");
    bool ok = true;
    int64_t n = remove_tree(vm, AS_CSTR(args[0]), &ok);
    if (!ok)
        return file_error(vm, "cannot remove", AS_CSTR(args[0]));
    *out = val_int(n);
    return true;
}
NATIVE(fs_join) {
    Value *items = args;
    int n = argc;
    if (argc == 1 && IS_LIST(args[0])) {
        items = AS_LIST(args[0])->items;
        n = AS_LIST(args[0])->count;
    }
    Buf b;
    buf_init(&b);
    for (int i = 0; i < n; i++) {
        if (!IS_STRING(items[i])) {
            buf_free(&b);
            return vm_throw(vm, "TypeError", "fs.join expects strings");
        }
        const char *s = AS_CSTR(items[i]);
        if (s[0] == '/' && i > 0) {
            b.len = 0;
        } else if (b.len && b.data[b.len - 1] != '/' && s[0])
            buf_addc(&b, '/');
        buf_adds(&b, s);
    }
    *out = sv(vm, b.data ? b.data : "", b.len);
    buf_free(&b);
    return true;
}
static const char *last_sep(const char *s) {
    const char *a = strrchr(s, '/'), *b = strrchr(s, '\\');
    return a > b ? a : b;
}
NATIVE(fs_basename) {
    (void)argc;
    STR(0, "fs.basename");
    const char *s = AS_CSTR(args[0]);
    size_t n = strlen(s);
    while (n > 1 && (s[n - 1] == '/' || s[n - 1] == '\\'))
        n--;
    char *t = xstrndup(s, n);
    const char *sep = last_sep(t);
    *out = svc(vm, sep ? sep + 1 : t);
    free(t);
    return true;
}
NATIVE(fs_dirname) {
    (void)argc;
    STR(0, "fs.dirname");
    const char *s = AS_CSTR(args[0]);
    const char *sep = last_sep(s);
    if (!sep)
        *out = svc(vm, ".");
    else if (sep == s)
        *out = svc(vm, "/");
    else
        *out = sv(vm, s, (size_t)(sep - s));
    return true;
}
NATIVE(fs_extension) {
    (void)argc;
    STR(0, "fs.extension");
    const char *s = AS_CSTR(args[0]);
    const char *sep = last_sep(s);
    const char *base = sep ? sep + 1 : s;
    const char *dot = strrchr(base, '.');
    *out = svc(vm, dot && dot != base ? dot : "");
    return true;
}
NATIVE(fs_stem) {
    (void)argc;
    STR(0, "fs.stem");
    const char *s = AS_CSTR(args[0]);
    const char *sep = last_sep(s);
    const char *base = sep ? sep + 1 : s;
    const char *dot = strrchr(base, '.');
    *out = dot && dot != base ? sv(vm, base, (size_t)(dot - base)) : svc(vm, base);
    return true;
}
static void normalize_path(const char *in, Buf *out) {
    bool abs = in[0] == '/';
    char **parts = xmalloc(sizeof(char *) * (strlen(in) + 2));
    int n = 0;
    char *copy = xstrdup(in);
    for (char *c = copy; *c; c++)
        if (*c == '\\')
            *c = '/';
    for (char *tok = strtok(copy, "/"); tok; tok = strtok(NULL, "/")) {
        if (!strcmp(tok, "."))
            continue;
        if (!strcmp(tok, "..")) {
            if (n > 0 && strcmp(parts[n - 1], ".."))
                n--;
            else if (!abs)
                parts[n++] = tok;
            continue;
        }
        parts[n++] = tok;
    }
    if (abs)
        buf_addc(out, '/');
    for (int i = 0; i < n; i++) {
        if (i)
            buf_addc(out, '/');
        buf_adds(out, parts[i]);
    }
    if (!out->len)
        buf_addc(out, '.');
    free(parts);
    free(copy);
}
NATIVE(fs_normalize) {
    (void)argc;
    STR(0, "fs.normalize");
    Buf b;
    buf_init(&b);
    normalize_path(AS_CSTR(args[0]), &b);
    *out = sv(vm, b.data, b.len);
    buf_free(&b);
    return true;
}
NATIVE(fs_absolute) {
    (void)argc;
    STR(0, "fs.absolute");
    const char *s = AS_CSTR(args[0]);
    Buf joined;
    buf_init(&joined);
    if (s[0] != '/' && !(s[0] && s[1] == ':')) {
        char *cwd = current_dir();
        buf_adds(&joined, cwd);
        buf_addc(&joined, '/');
        free(cwd);
    }
    buf_adds(&joined, s);
    Buf b;
    buf_init(&b);
    normalize_path(joined.data, &b);
    *out = sv(vm, b.data, b.len);
    buf_free(&b);
    buf_free(&joined);
    return true;
}
NATIVE(fs_cwd) {
    (void)argc;
    (void)args;
    char *c = current_dir();
    *out = svc(vm, c);
    free(c);
    return true;
}
NATIVE(fs_temp_dir) {
    (void)argc;
    (void)args;
    const char *t = getenv("TMPDIR");
    if (!t)
        t = getenv("TEMP");
    if (!t)
        t = getenv("TMP");
#ifdef _WIN32
    if (!t)
        t = ".";
#else
    if (!t)
        t = "/tmp";
#endif
    *out = svc(vm, t);
    return true;
}

static void make_fs(VM *vm, ObjModule *m) {
    struct {
        const char *n;
        NativeFn f;
        int a, b;
    } fns[] = {{"exists", fs_exists, 1, 1},       {"is_dir", fs_is_dir, 1, 1},       {"is_file", fs_is_file, 1, 1},
               {"read", fs_read, 1, 1},           {"try_read", fs_try_read, 1, 1},   {"write", fs_write, 2, 2},
               {"append", fs_append, 2, 2},       {"size", fs_size, 1, 1},           {"list", fs_list, 1, 1},
               {"walk", fs_walk, 1, 1},           {"mkdir", fs_mkdir, 1, 1},         {"copy", fs_copy, 2, 2},
               {"move", fs_move, 2, 2},           {"remove", fs_remove, 1, 1},       {"remove_all", fs_remove_all, 1, 1},
               {"join", fs_join, 1, -1},          {"basename", fs_basename, 1, 1},   {"dirname", fs_dirname, 1, 1},
               {"extension", fs_extension, 1, 1}, {"stem", fs_stem, 1, 1},           {"absolute", fs_absolute, 1, 1},
               {"normalize", fs_normalize, 1, 1}, {"cwd", fs_cwd, 0, 0},             {"temp_dir", fs_temp_dir, 0, 0},
               {NULL, NULL, 0, 0}};
    for (int i = 0; fns[i].n; i++)
        module_set_native(vm, m, fns[i].n, fns[i].f, fns[i].a, fns[i].b);
}

/* ---------------------------------------------------------------- native classes */

static ObjClass *native_class(VM *vm, const char *name, const char **fields) {
    ObjClass *k = new_class(vm, str_intern_c(vm, name));
    push_root(vm, val_obj(k));
    int n = 0;
    while (fields[n])
        n++;
    k->nfields = n;
    k->field_names = TB_ALLOC(vm, ObjString *, n ? n : 1);
    k->field_types = TB_ALLOC(vm, Value, n ? n : 1);
    k->field_has_default = TB_ALLOC(vm, bool, n ? n : 1);
    k->field_consts = TB_ALLOC(vm, Value, n ? n : 1);
    for (int i = 0; i < n; i++) {
        k->field_names[i] = str_intern_c(vm, fields[i]);
        k->field_types[i] = val_null();
        k->field_has_default[i] = true;
        k->field_consts[i] = val_null();
        table_set(vm, &k->field_index, k->field_names[i], val_int(i));
    }
    k->finalized = true;
    k->native = true;
    pop_root(vm, 1);
    return k;
}
static Value class_of(VM *vm, ObjModule *m, const char *name) {
    int s = module_find(m, str_intern_c(vm, name));
    return s >= 0 ? m->values.items[s] : val_null();
}

/* ---------------------------------------------------------------- os */

static int decode_status(int st) {
#ifdef _WIN32
    return st;
#else
    if (st == -1)
        return -1;
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    if (WIFSIGNALED(st))
        return -WTERMSIG(st);
    return st;
#endif
}
NATIVE(os_env) {
    (void)argc;
    STR(0, "os.env");
    const char *v = getenv(AS_CSTR(args[0]));
    *out = svc(vm, v ? v : "");
    return true;
}
NATIVE(os_set_env) {
    (void)argc;
    STR(0, "os.set_env");
    STR(1, "os.set_env");
#ifdef _WIN32
    _putenv_s(AS_CSTR(args[0]), AS_CSTR(args[1]));
#else
    setenv(AS_CSTR(args[0]), AS_CSTR(args[1]), 1);
#endif
    *out = val_null();
    return true;
}
NATIVE(os_args) {
    (void)argc;
    (void)args;
    ObjList *l = new_list(vm, OBJ_ARRAY, vm->argc);
    push_root(vm, val_obj(l));
    for (int i = 0; i < vm->argc; i++)
        list_push(vm, l, svc(vm, vm->argv[i]));
    pop_root(vm, 1);
    *out = val_obj(l);
    return true;
}
NATIVE(os_exit) {
    int64_t code = 0;
    if (argc > 0 && !to_int64(vm, args[0], &code, "os.exit"))
        return false;
    fflush(stdout);
    fflush(stderr);
    exit((int)code);
}
NATIVE(os_shell) {
    (void)argc;
    STR(0, "os.shell");
    if ((int)strlen(AS_CSTR(args[0])) != AS_STRING(args[0])->len)
        return vm_throw(vm, "ProcessError", "shell commands cannot contain NUL characters");
    fflush(stdout);
    fflush(stderr);
    int st = system(AS_CSTR(args[0]));
    if (st == -1)
        return vm_throw(vm, "ProcessError", "cannot run the shell: %s", strerror(errno));
    *out = val_int(decode_status(st));
    return true;
}
static bool argv_of(VM *vm, Value v, char ***out, int *n) {
    if (!IS_LIST(v) || AS_LIST(v)->count == 0)
        return vm_throw(vm, "ProcessError", "a command needs a non-empty string[]");
    ObjList *l = AS_LIST(v);
    if (IS_STRING(l->items[0]) && AS_STRING(l->items[0])->len == 0)
        return vm_throw(vm, "ProcessError", "a command needs a program name");
    char **argv = xmalloc(sizeof(char *) * (size_t)(l->count + 1));
    for (int i = 0; i < l->count; i++) {
        if (!IS_STRING(l->items[i])) {
            free(argv);
            return vm_throw(vm, "ProcessError", "command arguments must be strings");
        }
        if ((int)strlen(AS_CSTR(l->items[i])) != AS_STRING(l->items[i])->len) {
            free(argv);
            return vm_throw(vm, "ProcessError", "command arguments cannot contain NUL characters");
        }
        argv[i] = AS_CSTR(l->items[i]);
    }
    argv[l->count] = NULL;
    *out = argv;
    *n = l->count;
    return true;
}
typedef struct RunOutput RunOutput;
static void run_process(char **argv, const char *input, size_t input_len, const char *cwd, double timeout,
                        bool capture, RunOutput *r);
static bool run_simple(VM *vm, char **argv, Value *out);
NATIVE(os_command) {
    (void)argc;
    char **argv = NULL;
    int n = 0;
    if (!argv_of(vm, args[0], &argv, &n))
        return false;
    return run_simple(vm, argv, out);
}

struct RunOutput {
    int status;
    bool timed_out, launched;
    Buf out, err;
    char error[256];
};

/* Runs argv with optional stdin text, capturing stdout/stderr, with an optional timeout. */
static void run_process(char **argv, const char *input, size_t input_len, const char *cwd, double timeout,
                        bool capture, RunOutput *r) {
    memset(r, 0, sizeof *r);
    buf_init(&r->out);
    buf_init(&r->err);
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    (void)cwd;
    (void)timeout;
    Buf cmd;
    buf_init(&cmd);
    for (int i = 0; argv[i]; i++) {
        if (i)
            buf_addc(&cmd, ' ');
        buf_addc(&cmd, '"');
        buf_adds(&cmd, argv[i]);
        buf_addc(&cmd, '"');
    }
    if (input && input_len) {
        /* stdin via a temporary file */
        char tmp[L_tmpnam];
        tmpnam(tmp);
        FILE *t = fopen(tmp, "wb");
        if (t) {
            fwrite(input, 1, input_len, t);
            fclose(t);
            buf_adds(&cmd, " < \"");
            buf_adds(&cmd, tmp);
            buf_addc(&cmd, '"');
        }
    }
    if (!capture) {
        r->status = system(cmd.data);
        r->launched = true;
        buf_free(&cmd);
        return;
    }
    FILE *p = popen(cmd.data, "rb");
    buf_free(&cmd);
    if (!p) {
        snprintf(r->error, sizeof r->error, "cannot run %s", argv[0]);
        return;
    }
    char tmpb[8192];
    size_t n;
    while ((n = fread(tmpb, 1, sizeof tmpb, p)) > 0)
        buf_add(&r->out, tmpb, n);
    r->status = pclose(p);
    r->launched = true;
#else
    int in_p[2] = {-1, -1}, out_p[2] = {-1, -1}, err_p[2] = {-1, -1}, exec_p[2];
    if (pipe(exec_p) != 0 || pipe(in_p) != 0 || (capture && (pipe(out_p) != 0 || pipe(err_p) != 0))) {
        snprintf(r->error, sizeof r->error, "pipe failed: %s", strerror(errno));
        return;
    }
    fcntl(exec_p[1], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(r->error, sizeof r->error, "fork failed: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        dup2(in_p[0], 0);
        if (capture) {
            dup2(out_p[1], 1);
            dup2(err_p[1], 2);
        }
        close(in_p[0]);
        close(in_p[1]);
        if (capture) {
            close(out_p[0]);
            close(out_p[1]);
            close(err_p[0]);
            close(err_p[1]);
        }
        close(exec_p[0]);
        if (cwd && cwd[0] && chdir(cwd) != 0) {
            int e = errno;
            if (write(exec_p[1], &e, sizeof e) < 0) {
            }
            _exit(127);
        }
        execvp(argv[0], argv);
        int e = errno;
        if (write(exec_p[1], &e, sizeof e) < 0) {
        }
        _exit(127);
    }
    close(exec_p[1]);
    close(in_p[0]);
    if (capture) {
        close(out_p[1]);
        close(err_p[1]);
    }
    int child_errno = 0;
    ssize_t got = read(exec_p[0], &child_errno, sizeof child_errno);
    close(exec_p[0]);
    if (got == (ssize_t)sizeof child_errno) {
        snprintf(r->error, sizeof r->error, "cannot run %s: %s", argv[0], strerror(child_errno));
        close(in_p[1]);
        if (capture) {
            close(out_p[0]);
            close(err_p[0]);
        }
        waitpid(pid, NULL, 0);
        return;
    }
    r->launched = true;
    size_t written = 0;
    if (!input || !input_len) {
        close(in_p[1]);
        in_p[1] = -1;
    } else
        fcntl(in_p[1], F_SETFL, O_NONBLOCK);
    signal(SIGPIPE, SIG_IGN);
    double deadline = timeout > 0 ? now_monotonic() + timeout : 0;
    bool out_open = capture, err_open = capture;
    while (out_open || err_open || in_p[1] >= 0) {
        struct pollfd fds[3];
        int nf = 0, io = -1, oo = -1, eo = -1;
        if (in_p[1] >= 0) {
            fds[nf].fd = in_p[1];
            fds[nf].events = POLLOUT;
            io = nf++;
        }
        if (out_open) {
            fds[nf].fd = out_p[0];
            fds[nf].events = POLLIN;
            oo = nf++;
        }
        if (err_open) {
            fds[nf].fd = err_p[0];
            fds[nf].events = POLLIN;
            eo = nf++;
        }
        int wait_ms = -1;
        if (deadline > 0) {
            double left = deadline - now_monotonic();
            if (left <= 0) {
                kill(pid, SIGKILL);
                r->timed_out = true;
                break;
            }
            wait_ms = (int)(left * 1000) + 1;
        }
        int pr = poll(fds, (nfds_t)nf, wait_ms);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            continue;
        char tmpb[16384];
        if (io >= 0 && (fds[io].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t w = write(in_p[1], input + written, input_len - written);
            if (w > 0)
                written += (size_t)w;
            if (w < 0 && errno != EAGAIN) {
                close(in_p[1]);
                in_p[1] = -1;
            } else if (written >= input_len) {
                close(in_p[1]);
                in_p[1] = -1;
            }
        }
        if (oo >= 0 && (fds[oo].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(out_p[0], tmpb, sizeof tmpb);
            if (n > 0)
                buf_add(&r->out, tmpb, (size_t)n);
            else {
                out_open = false;
                close(out_p[0]);
            }
        }
        if (eo >= 0 && (fds[eo].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(err_p[0], tmpb, sizeof tmpb);
            if (n > 0)
                buf_add(&r->err, tmpb, (size_t)n);
            else {
                err_open = false;
                close(err_p[0]);
            }
        }
    }
    if (in_p[1] >= 0)
        close(in_p[1]);
    if (out_open)
        close(out_p[0]);
    if (err_open)
        close(err_p[0]);
    if (!capture && deadline > 0) {
        /* wait with timeout */
        int st;
        for (;;) {
            pid_t w = waitpid(pid, &st, WNOHANG);
            if (w == pid) {
                r->status = decode_status(st);
                return;
            }
            if (now_monotonic() > deadline) {
                kill(pid, SIGKILL);
                r->timed_out = true;
                break;
            }
            sleep_seconds(0.005);
        }
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    r->status = decode_status(st);
#endif
}
static bool run_simple(VM *vm, char **argv, Value *out) {
    RunOutput r;
    run_process(argv, NULL, 0, NULL, 0, false, &r);
    free(argv);
    buf_free(&r.out);
    buf_free(&r.err);
    if (!r.launched)
        return vm_throw(vm, "ProcessError", "%s", r.error);
    *out = val_int(r.status);
    return true;
}
static bool result_instance(VM *vm, RunOutput *r, Value *out) {
    ObjModule *os = lib_native_module(vm, "os");
    Value k = class_of(vm, os, "result");
    ObjInstance *in = new_instance(vm, AS_CLASS(k));
    push_root(vm, val_obj(in));
    in->fields[0] = val_int(r->status);
    in->fields[1] = sv(vm, r->out.data ? r->out.data : "", r->out.len);
    in->fields[2] = sv(vm, r->err.data ? r->err.data : "", r->err.len);
    in->fields[3] = val_bool(r->timed_out);
    pop_root(vm, 1);
    buf_free(&r->out);
    buf_free(&r->err);
    *out = val_obj(in);
    return true;
}
NATIVE(os_run) {
    char **argv = NULL;
    int n = 0;
    if (!argv_of(vm, args[0], &argv, &n))
        return false;
    double timeout = 0;
    const char *input = NULL;
    size_t input_len = 0;
    if (argc > 1 && IS_STRING(args[1])) {
        input = AS_CSTR(args[1]);
        input_len = (size_t)AS_STRING(args[1])->len;
    }
    if (argc > 2 && !to_double(vm, args[2], &timeout, "os.run")) {
        free(argv);
        return false;
    }
    RunOutput r;
    run_process(argv, input, input_len, NULL, timeout, true, &r);
    free(argv);
    if (!r.launched) {
        buf_free(&r.out);
        buf_free(&r.err);
        return vm_throw(vm, "ProcessError", "%s", r.error);
    }
    return result_instance(vm, &r, out);
}
/* os.process: an instance with command/input/cwd/env/inherit_env/timeout/capture and run(). */
NATIVE(os_process) {
    (void)argc;
    if (!IS_LIST(args[0]))
        return vm_throw(vm, "ProcessError", "os.process expects a string[]");
    ObjModule *os = lib_native_module(vm, "os");
    ObjInstance *in = new_instance(vm, AS_CLASS(class_of(vm, os, "process_type")));
    push_root(vm, val_obj(in));
    in->fields[0] = args[0];
    in->fields[1] = svc(vm, "");
    in->fields[2] = svc(vm, "");
    in->fields[3] = val_obj(new_map(vm, OBJ_MAP));
    in->fields[4] = val_bool(true);
    in->fields[5] = val_double(0);
    in->fields[6] = val_bool(true);
    pop_root(vm, 1);
    *out = val_obj(in);
    return true;
}
NATIVE(os_process_run) {
    (void)argc;
    ObjInstance *p = AS_INSTANCE(args[0]);
    char **argv = NULL;
    int n = 0;
    if (!argv_of(vm, p->fields[0], &argv, &n))
        return false;
    double timeout = 0;
    if (!to_double(vm, p->fields[5], &timeout, "process.timeout")) {
        free(argv);
        return false;
    }
    const char *input = IS_STRING(p->fields[1]) ? AS_CSTR(p->fields[1]) : NULL;
    size_t input_len = IS_STRING(p->fields[1]) ? (size_t)AS_STRING(p->fields[1])->len : 0;
    const char *cwd = IS_STRING(p->fields[2]) ? AS_CSTR(p->fields[2]) : NULL;
    /* environment changes apply around the launch */
    ObjMap *env = IS_MAP(p->fields[3]) ? AS_MAP(p->fields[3]) : NULL;
    if (env)
        for (int i = 0; i < env->used; i++)
            if (env->entries[i].live && IS_STRING(env->entries[i].key) && IS_STRING(env->entries[i].value)) {
#ifdef _WIN32
                _putenv_s(AS_CSTR(env->entries[i].key), AS_CSTR(env->entries[i].value));
#else
                setenv(AS_CSTR(env->entries[i].key), AS_CSTR(env->entries[i].value), 1);
#endif
            }
    bool capture = !IS_BOOL(p->fields[6]) || p->fields[6].as.b;
    RunOutput r;
    run_process(argv, input, input_len, cwd, timeout, capture, &r);
    free(argv);
    if (!r.launched) {
        buf_free(&r.out);
        buf_free(&r.err);
        return vm_throw(vm, "ProcessError", "%s", r.error);
    }
    return result_instance(vm, &r, out);
}
NATIVE(os_pipeline) {
    if (!IS_LIST(args[0]))
        return vm_throw(vm, "ProcessError", "os.pipeline expects string[][]");
    ObjList *stages = AS_LIST(args[0]);
    Buf data, errors;
    buf_init(&data);
    buf_init(&errors);
    if (argc > 1 && IS_STRING(args[1]))
        buf_add(&data, AS_CSTR(args[1]), (size_t)AS_STRING(args[1])->len);
    int status = 0;
    for (int i = 0; i < stages->count; i++) {
        char **argv = NULL;
        int n = 0;
        if (!argv_of(vm, stages->items[i], &argv, &n)) {
            buf_free(&data);
            buf_free(&errors);
            return false;
        }
        RunOutput r;
        run_process(argv, data.data, data.len, NULL, 0, true, &r);
        free(argv);
        if (!r.launched) {
            buf_free(&data);
            buf_free(&errors);
            buf_free(&r.out);
            buf_free(&r.err);
            return vm_throw(vm, "ProcessError", "%s", r.error);
        }
        buf_free(&data);
        data = r.out;
        buf_add(&errors, r.err.data ? r.err.data : "", r.err.len);
        buf_free(&r.err);
        status = r.status;
    }
    RunOutput r;
    memset(&r, 0, sizeof r);
    r.status = status;
    r.out = data;
    r.err = errors;
    return result_instance(vm, &r, out);
}
NATIVE(os_cwd) { return fs_cwd(vm, argc, args, out); }
NATIVE(os_chdir) {
    (void)argc;
    STR(0, "os.chdir");
#ifdef _WIN32
    int r = _chdir(AS_CSTR(args[0]));
#else
    int r = chdir(AS_CSTR(args[0]));
#endif
    if (r != 0)
        return file_error(vm, "cannot change directory to", AS_CSTR(args[0]));
    *out = val_null();
    return true;
}
NATIVE(os_platform) {
    (void)argc;
    (void)args;
#if defined(_WIN32)
    *out = svc(vm, "windows");
#elif defined(__APPLE__)
    *out = svc(vm, "macos");
#elif defined(__linux__)
    *out = svc(vm, "linux");
#else
    *out = svc(vm, "unix");
#endif
    return true;
}
NATIVE(os_environment) {
    (void)argc;
    (void)args;
    ObjMap *m = new_map(vm, OBJ_MAP);
    push_root(vm, val_obj(m));
#ifdef _WIN32
    char **env = _environ;
#else
    char **env = environ;
#endif
    for (int i = 0; env && env[i]; i++) {
        const char *eq = strchr(env[i], '=');
        if (!eq || eq == env[i])
            continue;
        Value k = sv(vm, env[i], (size_t)(eq - env[i]));
        push_root(vm, k);
        map_set(vm, m, k, svc(vm, eq + 1));
        pop_root(vm, 1);
    }
    pop_root(vm, 1);
    *out = val_obj(m);
    return true;
}

static void make_os(VM *vm, ObjModule *m) {
    static const char *result_fields[] = {"status", "stdout", "stderr", "timed_out", NULL};
    static const char *process_fields[] = {"command", "input", "cwd", "env", "inherit_env", "timeout", "capture", NULL};
    ObjClass *rk = native_class(vm, "os.result", result_fields);
    module_set_value(vm, m, "result", val_obj(rk));
    ObjClass *pk = native_class(vm, "os.process", process_fields);
    ObjNative *run = new_native(vm, "run", os_process_run, 1, 1);
    run->is_method = true;
    table_set(vm, &pk->methods, str_intern_c(vm, "run"), val_obj(run));
    module_set_value(vm, m, "process_type", val_obj(pk));
    struct {
        const char *n;
        NativeFn f;
        int a, b;
    } fns[] = {{"env", os_env, 1, 1},           {"set_env", os_set_env, 2, 2},     {"args", os_args, 0, 0},
               {"exit", os_exit, 0, 1},         {"shell", os_shell, 1, 1},         {"command", os_command, 1, 1},
               {"run", os_run, 1, 3},           {"process", os_process, 1, 1},     {"pipeline", os_pipeline, 1, 2},
               {"cwd", os_cwd, 0, 0},           {"chdir", os_chdir, 1, 1},         {"platform", os_platform, 0, 0},
               {"environment", os_environment, 0, 0}, {NULL, NULL, 0, 0}};
    for (int i = 0; fns[i].n; i++)
        module_set_native(vm, m, fns[i].n, fns[i].f, fns[i].a, fns[i].b);
}

/* ---------------------------------------------------------------- time */

static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
NATIVE(t_monotonic) {
    (void)argc;
    (void)args;
    (void)vm;
    *out = val_double(now_monotonic());
    return true;
}
NATIVE(t_now) {
    (void)argc;
    (void)args;
    (void)vm;
    *out = val_double(now_wall());
    return true;
}
NATIVE(t_unix) {
    (void)argc;
    (void)args;
    (void)vm;
    *out = val_int((int64_t)floor(now_wall()));
    return true;
}
NATIVE(t_millis) {
    (void)argc;
    (void)args;
    (void)vm;
    *out = val_int((int64_t)(now_monotonic() * 1000.0));
    return true;
}
/* A duration in seconds: a number, or a time quantity (`500ms`, `delay`) converted to seconds. */
static bool duration_seconds(VM *vm, Value v, double *s, const char *what, bool *quantity) {
    char ns[128];
    bool real;
    *quantity = IS_OBJT(v, OBJ_INSTANCE) &&
                quantity_class_name(AS_INSTANCE(v)->klass->name->chars, ns, sizeof ns, &real);
    if (!*quantity)
        return to_double(vm, v, s, what);
    if (strcmp(ns, "time") != 0)
        return vm_throw(vm, "TypeError", "%s expects seconds or a time quantity, not %s[%s]", what,
                        real ? "float" : "int", ns);
    Value converted;
    if (!vm_quantity_in(vm, v, val_obj(str_intern_c(vm, "s")), &converted))
        return false;
    return to_double(vm, AS_INSTANCE(converted)->fields[0], s, what);
}
NATIVE(t_sleep) {
    (void)argc;
    double s;
    bool quantity;
    if (!duration_seconds(vm, args[0], &s, "time.sleep", &quantity))
        return false;
    fflush(stdout);
    sleep_seconds(s);
    *out = val_null();
    return true;
}
NATIVE(t_sleep_ms) {
    (void)argc;
    double s;
    bool quantity;
    if (!duration_seconds(vm, args[0], &s, "time.sleep_ms", &quantity))
        return false;
    fflush(stdout);
    sleep_seconds(quantity ? s : s / 1000.0);
    *out = val_null();
    return true;
}
NATIVE(t_elapsed) {
    (void)argc;
    double s;
    if (!to_double(vm, args[0], &s, "time.elapsed"))
        return false;
    *out = val_double(now_monotonic() - s);
    return true;
}
static bool format_time(VM *vm, double secs, const char *pattern, bool local, Value *out) {
    time_t t = (time_t)floor(secs);
    struct tm tmv;
    if (!(local ? tm_local(t, &tmv) : tm_utc(t, &tmv)))
        return vm_throw(vm, "ValueError", "time out of range");
    char buf[512];
    size_t n = strftime(buf, sizeof buf, pattern, &tmv);
    if (n == 0 && pattern[0])
        return vm_throw(vm, "ValueError", "invalid or too long time format");
    *out = sv(vm, buf, n);
    return true;
}
NATIVE(t_format) {
    double s;
    if (!to_double(vm, args[0], &s, "time.format"))
        return false;
    const char *p = argc > 1 && IS_STRING(args[1]) ? AS_CSTR(args[1]) : "%Y-%m-%dT%H:%M:%SZ";
    return format_time(vm, s, p, false, out);
}
NATIVE(t_format_local) {
    double s;
    if (!to_double(vm, args[0], &s, "time.format_local"))
        return false;
    const char *p = argc > 1 && IS_STRING(args[1]) ? AS_CSTR(args[1]) : "%Y-%m-%d %H:%M:%S";
    return format_time(vm, s, p, true, out);
}
NATIVE(t_today) {
    (void)argc;
    (void)args;
    return format_time(vm, now_wall(), "%Y-%m-%d", true, out);
}
NATIVE(t_clock) {
    (void)argc;
    (void)args;
    return format_time(vm, now_wall(), "%H:%M:%S", true, out);
}
NATIVE(t_timestamp) {
    (void)argc;
    (void)args;
    return format_time(vm, now_wall(), "%Y-%m-%dT%H:%M:%SZ", false, out);
}
NATIVE(t_from_date) {
    int64_t y, mo, d, h = 0, mi = 0;
    double sec = 0;
    if (!to_int64(vm, args[0], &y, "from_date") || !to_int64(vm, args[1], &mo, "from_date") ||
        !to_int64(vm, args[2], &d, "from_date"))
        return false;
    if (argc > 3 && !to_int64(vm, args[3], &h, "from_date"))
        return false;
    if (argc > 4 && !to_int64(vm, args[4], &mi, "from_date"))
        return false;
    if (argc > 5 && !to_double(vm, args[5], &sec, "from_date"))
        return false;
    int64_t days = days_from_civil(y, (int)mo, (int)d);
    *out = val_double((double)(days * 86400 + h * 3600 + mi * 60) + sec);
    return true;
}
/* A portable strptime for %Y %m %d %H %M %S %y %j %%, literal text, and whitespace. */
static bool parse_time(const char *s, const char *p, int64_t *y, int *mo, int *d, int *h, int *mi, double *sec) {
    *y = 1970;
    *mo = 1;
    *d = 1;
    *h = *mi = 0;
    *sec = 0;
    while (*p) {
        if (*p == '%') {
            p++;
            int width = *p == 'Y' ? 4 : 2;
            if (*p == '%') {
                if (*s++ != '%')
                    return false;
                p++;
                continue;
            }
            if (*p == 'S') {
                char *end;
                double v = strtod(s, &end);
                if (end == s)
                    return false;
                *sec = v;
                s = end;
                p++;
                continue;
            }
            int v = 0, n = 0;
            bool neg = false;
            if (*p == 'Y' && *s == '-') {
                neg = true;
                s++;
            }
            while (n < width && isdigit((unsigned char)*s)) {
                v = v * 10 + (*s++ - '0');
                n++;
            }
            if (!n)
                return false;
            switch (*p) {
            case 'Y': *y = neg ? -v : v; break;
            case 'y': *y = v < 69 ? 2000 + v : 1900 + v; break;
            case 'm': *mo = v; break;
            case 'd': *d = v; break;
            case 'H': *h = v; break;
            case 'M': *mi = v; break;
            default: return false;
            }
            p++;
        } else if (isspace((unsigned char)*p)) {
            while (isspace((unsigned char)*s))
                s++;
            p++;
        } else {
            if (*s++ != *p++)
                return false;
        }
    }
    return *s == 0;
}
NATIVE(t_parse) {
    STR(0, "time.parse");
    const char *p = argc > 1 && IS_STRING(args[1]) ? AS_CSTR(args[1]) : "%Y-%m-%dT%H:%M:%SZ";
    int64_t y;
    int mo, d, h, mi;
    double sec;
    if (!parse_time(AS_CSTR(args[0]), p, &y, &mo, &d, &h, &mi, &sec))
        return vm_throw(vm, "ValueError", "\"%s\" does not match the time format \"%s\"", AS_CSTR(args[0]), p);
    int64_t days = days_from_civil(y, mo, d);
    *out = val_double((double)(days * 86400 + h * 3600 + mi * 60) + sec);
    return true;
}
static bool date_of(VM *vm, double secs, bool local, Value *out) {
    time_t t = (time_t)floor(secs);
    struct tm tmv;
    if (!(local ? tm_local(t, &tmv) : tm_utc(t, &tmv)))
        return vm_throw(vm, "ValueError", "time out of range");
    ObjModule *tm = lib_native_module(vm, "time");
    Value k = class_of(vm, tm, "datetime");
    ObjInstance *in = new_instance(vm, AS_CLASS(k));
    in->fields[0] = val_int(tmv.tm_year + 1900);
    in->fields[1] = val_int(tmv.tm_mon + 1);
    in->fields[2] = val_int(tmv.tm_mday);
    in->fields[3] = val_int(tmv.tm_hour);
    in->fields[4] = val_int(tmv.tm_min);
    in->fields[5] = val_double(tmv.tm_sec + (secs - floor(secs)));
    in->fields[6] = val_int(tmv.tm_wday);
    in->fields[7] = val_int(tmv.tm_yday + 1);
    *out = val_obj(in);
    return true;
}
NATIVE(t_date) {
    double s = now_wall();
    if (argc > 0 && !to_double(vm, args[0], &s, "time.date"))
        return false;
    return date_of(vm, s, false, out);
}
NATIVE(t_local_date) {
    double s = now_wall();
    if (argc > 0 && !to_double(vm, args[0], &s, "time.local_date"))
        return false;
    return date_of(vm, s, true, out);
}
static void make_time(VM *vm, ObjModule *m) {
    static const char *fields[] = {"year", "month", "day", "hour", "minute", "second", "weekday", "yearday", NULL};
    module_set_value(vm, m, "datetime", val_obj(native_class(vm, "time.datetime", fields)));
    struct {
        const char *n;
        NativeFn f;
        int a, b;
    } fns[] = {{"monotonic", t_monotonic, 0, 0}, {"now", t_now, 0, 0},           {"unix", t_unix, 0, 0},
               {"millis", t_millis, 0, 0},       {"sleep", t_sleep, 1, 1},       {"delay", t_sleep, 1, 1},
               {"sleep_ms", t_sleep_ms, 1, 1},   {"elapsed", t_elapsed, 1, 1},   {"format", t_format, 1, 2},
               {"format_local", t_format_local, 1, 2}, {"parse", t_parse, 1, 2}, {"date", t_date, 0, 1},
               {"local_date", t_local_date, 0, 1},     {"from_date", t_from_date, 3, 6}, {"today", t_today, 0, 0},
               {"clock", t_clock, 0, 0},         {"timestamp", t_timestamp, 0, 0}, {NULL, NULL, 0, 0}};
    for (int i = 0; fns[i].n; i++)
        module_set_native(vm, m, fns[i].n, fns[i].f, fns[i].a, fns[i].b);
}

/* ---------------------------------------------------------------- io */

NATIVE(io_read_line) {
    (void)argc;
    (void)args;
    fflush(stdout);
    Buf b;
    buf_init(&b);
    int c;
    bool any = false;
    while ((c = fgetc(stdin)) != EOF) {
        any = true;
        if (c == '\n')
            break;
        buf_addc(&b, (char)c);
    }
    if (b.len && b.data[b.len - 1] == '\r')
        b.data[--b.len] = 0;
    *out = any ? sv(vm, b.data ? b.data : "", b.len) : val_null();
    buf_free(&b);
    return true;
}
NATIVE(io_read_all) {
    (void)argc;
    (void)args;
    fflush(stdout);
    Buf b;
    buf_init(&b);
    char tmp[8192];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, stdin)) > 0)
        buf_add(&b, tmp, n);
    *out = sv(vm, b.data ? b.data : "", b.len);
    buf_free(&b);
    return true;
}
NATIVE(io_write) {
    for (int i = 0; i < argc; i++)
        if (!lib_print_value(vm, stdout, args[i]))
            return false;
    *out = val_null();
    return true;
}
NATIVE(io_write_error) {
    fflush(stdout);
    for (int i = 0; i < argc; i++)
        if (!lib_print_value(vm, stderr, args[i]))
            return false;
    *out = val_null();
    return true;
}
NATIVE(io_flush) {
    (void)argc;
    (void)args;
    (void)vm;
    fflush(stdout);
    fflush(stderr);
    *out = val_null();
    return true;
}
NATIVE(io_open) {
    STR(0, "io.open");
    const char *mode = argc > 1 && IS_STRING(args[1]) ? AS_CSTR(args[1]) : "r";
    const char *cmode = !strcmp(mode, "r") ? "rb" : !strcmp(mode, "w") ? "wb" : !strcmp(mode, "a") ? "ab" : !strcmp(mode, "r+") ? "r+b" : NULL;
    if (!cmode)
        return vm_throw(vm, "ValueError", "invalid file mode \"%s\"", mode);
    FILE *f = fopen(AS_CSTR(args[0]), cmode);
    if (!f)
        return file_error(vm, "cannot open", AS_CSTR(args[0]));
    *out = val_obj(new_file(vm, f, AS_STRING(args[0])));
    return true;
}
static void make_io(VM *vm, ObjModule *m) {
    module_set_native(vm, m, "read_line", io_read_line, 0, 0);
    module_set_native(vm, m, "read_all", io_read_all, 0, 0);
    module_set_native(vm, m, "write", io_write, 0, -1);
    module_set_native(vm, m, "write_error", io_write_error, 0, -1);
    module_set_native(vm, m, "flush", io_flush, 0, 0);
    module_set_native(vm, m, "open", io_open, 1, 2);
    ObjFile *in = new_file(vm, stdin, str_intern_c(vm, "<stdin>"));
    in->is_std = true;
    module_set_value(vm, m, "stdin", val_obj(in));
    ObjFile *o = new_file(vm, stdout, str_intern_c(vm, "<stdout>"));
    o->is_std = true;
    module_set_value(vm, m, "stdout", val_obj(o));
    ObjFile *e = new_file(vm, stderr, str_intern_c(vm, "<stderr>"));
    e->is_std = true;
    module_set_value(vm, m, "stderr", val_obj(e));
}

/* ---------------------------------------------------------------- testing */

NATIVE(ts_assert) {
    if (!IS_BOOL(args[0]))
        return vm_throw(vm, "TypeError", "testing.assert expects a bool");
    if (!args[0].as.b) {
        /* The compiler's wording, including the trailing space when there is no message. */
        return vm_throw(vm, "AssertionError", "assertion failed: %s",
                        argc > 1 && IS_STRING(args[1]) ? AS_CSTR(args[1]) : "");
    }
    *out = val_null();
    return true;
}
NATIVE(ts_assert_equal) {
    bool eq;
    if (!values_equal(vm, args[0], args[1], &eq))
        return false;
    if (!eq) {
        Buf b;
        buf_init(&b);
        buf_adds(&b, "assertion failed: ");
        if (argc > 2 && IS_STRING(args[2]) && AS_STRING(args[2])->len) {
            buf_adds(&b, AS_CSTR(args[2]));
            buf_adds(&b, ": ");
        }
        buf_adds(&b, "expected ");
        if (!value_to_buf(vm, args[1], &b, true, 0) || (buf_adds(&b, ", got "), !value_to_buf(vm, args[0], &b, true, 0))) {
            buf_free(&b);
            return false;
        }
        bool r = vm_throw(vm, "AssertionError", "%s", b.data);
        buf_free(&b);
        return r;
    }
    *out = val_null();
    return true;
}
NATIVE(ts_fail) {
    const char *msg = argc > 0 && IS_STRING(args[0]) ? AS_CSTR(args[0]) : "failed";
    (void)out;
    return vm_throw(vm, "AssertionError", "assertion failed: %s", msg);
}
NATIVE(ts_expect_error) {
    (void)argc;
    Value fn = args[1];
    ObjString *want = NULL;
    if (IS_OBJT(args[0], OBJ_NATIVE) && IS_STRING(((ObjNative *)AS_OBJ(args[0]))->data))
        want = AS_STRING(((ObjNative *)AS_OBJ(args[0]))->data);
    else if (IS_OBJT(args[0], OBJ_CLASS))
        want = AS_CLASS(args[0])->name;
    else if (IS_STRING(args[0]))
        want = AS_STRING(args[0]);
    Value r;
    if (vm_call(vm, fn, 0, NULL, &r))
        return vm_throw(vm, "AssertionError", "expected %s, but nothing was raised", want ? want->chars : "Error");
    Value err = vm->error;
    vm->has_error = false;
    if (want && strcmp(want->chars, "Error") != 0) {
        ObjString *t = error_type_name(vm, err);
        bool ok = !strcmp(t->chars, want->chars);
        if (!ok && IS_OBJT(err, OBJ_INSTANCE))
            for (ObjClass *k = AS_INSTANCE(err)->klass; k && !ok; k = k->super)
                ok = !strcmp(k->name->chars, want->chars);
        if (!ok) /* a different error propagates, as with the compiler */
            return vm_throw_value(vm, err);
    }
    *out = err;
    return true;
}
NATIVE(ts_live_objects) {
    (void)argc;
    (void)args;
    gc_collect(vm); /* count what reference counting would still hold */
    int64_t n = 0;
    for (Obj *o = vm->objects; o; o = o->next)
        if (o->type == OBJ_INSTANCE)
            n++;
    *out = val_int(n);
    return true;
}
static void make_testing(VM *vm, ObjModule *m) {
    module_set_native(vm, m, "assert", ts_assert, 1, 2);
    module_set_native(vm, m, "assert_equal", ts_assert_equal, 2, 3);
    module_set_native(vm, m, "fail", ts_fail, 0, 1);
    module_set_native(vm, m, "expect_error", ts_expect_error, 2, 2);
    module_set_native(vm, m, "live_objects", ts_live_objects, 0, 0);
}

/* ---------------------------------------------------------------- collections, gc */

NATIVE(c_range) {
    int64_t a = 0, b;
    if (argc == 1) {
        if (!to_int64(vm, args[0], &b, "range"))
            return false;
    } else if (!to_int64(vm, args[0], &a, "range") || !to_int64(vm, args[1], &b, "range"))
        return false;
    int64_t n = b > a ? b - a : 0;
    if (n > 100000000)
        return vm_throw(vm, "NumericError", "range is too large");
    ObjList *l = new_list(vm, OBJ_ARRAY, (int)n);
    for (int64_t i = 0; i < n; i++)
        l->items[i] = val_int(a + i);
    l->count = (int)n;
    *out = val_obj(l);
    return true;
}
NATIVE(gc_collect_native) {
    (void)argc;
    (void)args;
    int64_t before = 0, after = 0;
    for (Obj *o = vm->objects; o; o = o->next)
        before++;
    gc_collect(vm);
    for (Obj *o = vm->objects; o; o = o->next)
        after++;
    *out = val_int(before - after);
    return true;
}
NATIVE(gc_pending) {
    (void)argc;
    (void)args;
    (void)vm;
    *out = val_int(0);
    return true;
}

/* ---------------------------------------------------------------- encoding, uuid */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static void b64_encode(Buf *b, const unsigned char *s, size_t n, const char *alpha, bool pad) {
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)s[i] << 16 | (i + 1 < n ? (uint32_t)s[i + 1] << 8 : 0) | (i + 2 < n ? s[i + 2] : 0);
        buf_addc(b, alpha[(v >> 18) & 63]);
        buf_addc(b, alpha[(v >> 12) & 63]);
        if (i + 1 < n)
            buf_addc(b, alpha[(v >> 6) & 63]);
        else if (pad)
            buf_addc(b, '=');
        if (i + 2 < n)
            buf_addc(b, alpha[v & 63]);
        else if (pad)
            buf_addc(b, '=');
    }
}
static bool b64_decode(Buf *b, const char *s, size_t n, bool url) {
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int v;
        if (c >= 'A' && c <= 'Z')
            v = c - 'A';
        else if (c >= 'a' && c <= 'z')
            v = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            v = c - '0' + 52;
        else if (c == (url ? '-' : '+'))
            v = 62;
        else if (c == (url ? '_' : '/'))
            v = 63;
        else if (c == '=')
            break;
        else if (c == '\n' || c == '\r' || c == ' ')
            continue;
        else
            return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            buf_addc(b, (char)((acc >> bits) & 0xff));
        }
    }
    return true;
}
#define ENC(name, ...)                                                                            \
    NATIVE(name) {                                                                                \
        (void)argc;                                                                               \
        STR(0, #name);                                                                            \
        const char *s = AS_CSTR(args[0]);                                                         \
        size_t n = (size_t)AS_STRING(args[0])->len;                                               \
        Buf b;                                                                                    \
        buf_init(&b);                                                                             \
        __VA_ARGS__;                                                                              \
        *out = sv(vm, b.data ? b.data : "", b.len);                                               \
        buf_free(&b);                                                                             \
        return true;                                                                              \
    }
ENC(e_base64_encode, b64_encode(&b, (const unsigned char *)s, n, B64, true))
ENC(e_base64url_encode, b64_encode(&b, (const unsigned char *)s, n, B64URL, false))
ENC(e_base64_decode, if (!b64_decode(&b, s, n, false)) {
    buf_free(&b);
    return vm_throw(vm, "ValueError", "invalid base64");
})
ENC(e_base64url_decode, if (!b64_decode(&b, s, n, true)) {
    buf_free(&b);
    return vm_throw(vm, "ValueError", "invalid base64url");
})
ENC(e_hex_encode, for (size_t i = 0; i < n; i++) buf_printf(&b, "%02x", (unsigned char)s[i]))
static int hexd(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
ENC(e_hex_decode, if (n % 2) {
    buf_free(&b);
    return vm_throw(vm, "ValueError", "hex text needs an even number of digits");
} for (size_t i = 0; i < n; i += 2) {
    int h = hexd(s[i]), l = hexd(s[i + 1]);
    if (h < 0 || l < 0) {
        buf_free(&b);
        return vm_throw(vm, "ValueError", "invalid hex digit");
    }
    buf_addc(&b, (char)(h * 16 + l));
})
static void url_enc(Buf *b, const char *s, size_t n, bool form) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            buf_addc(b, (char)c);
        else if (form && c == ' ')
            buf_addc(b, '+');
        else
            buf_printf(b, "%%%02X", c);
    }
}
static bool url_dec(Buf *b, const char *s, size_t n, bool form) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%') {
            if (i + 2 >= n)
                return false;
            int h = hexd(s[i + 1]), l = hexd(s[i + 2]);
            if (h < 0 || l < 0)
                return false;
            buf_addc(b, (char)(h * 16 + l));
            i += 2;
        } else if (form && s[i] == '+')
            buf_addc(b, ' ');
        else
            buf_addc(b, s[i]);
    }
    return true;
}
ENC(e_url_encode, url_enc(&b, s, n, false))
ENC(e_form_encode, url_enc(&b, s, n, true))
ENC(e_url_decode, if (!url_dec(&b, s, n, false)) {
    buf_free(&b);
    return vm_throw(vm, "ValueError", "invalid percent-encoding");
})
ENC(e_form_decode, if (!url_dec(&b, s, n, true)) {
    buf_free(&b);
    return vm_throw(vm, "ValueError", "invalid percent-encoding");
})
static void make_encoding(VM *vm, ObjModule *m) {
    module_set_native(vm, m, "base64_encode", e_base64_encode, 1, 1);
    module_set_native(vm, m, "base64_decode", e_base64_decode, 1, 1);
    module_set_native(vm, m, "base64url_encode", e_base64url_encode, 1, 1);
    module_set_native(vm, m, "base64url_decode", e_base64url_decode, 1, 1);
    module_set_native(vm, m, "hex_encode", e_hex_encode, 1, 1);
    module_set_native(vm, m, "hex_decode", e_hex_decode, 1, 1);
    module_set_native(vm, m, "url_encode", e_url_encode, 1, 1);
    module_set_native(vm, m, "url_decode", e_url_decode, 1, 1);
    module_set_native(vm, m, "form_encode", e_form_encode, 1, 1);
    module_set_native(vm, m, "form_decode", e_form_decode, 1, 1);
}
static void uuid_text(Buf *b, const unsigned char *u) {
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            buf_addc(b, '-');
        buf_printf(b, "%02x", u[i]);
    }
}
NATIVE(u_v4) {
    (void)argc;
    (void)args;
    unsigned char u[16];
    secure_random(u, 16);
    u[6] = (unsigned char)((u[6] & 0x0f) | 0x40);
    u[8] = (unsigned char)((u[8] & 0x3f) | 0x80);
    Buf b;
    buf_init(&b);
    uuid_text(&b, u);
    *out = sv(vm, b.data, b.len);
    buf_free(&b);
    return true;
}
NATIVE(u_v7) {
    (void)argc;
    (void)args;
    unsigned char u[16];
    secure_random(u, 16);
    uint64_t ms = (uint64_t)(now_wall() * 1000.0);
    for (int i = 0; i < 6; i++)
        u[i] = (unsigned char)(ms >> (8 * (5 - i)));
    u[6] = (unsigned char)((u[6] & 0x0f) | 0x70);
    u[8] = (unsigned char)((u[8] & 0x3f) | 0x80);
    Buf b;
    buf_init(&b);
    uuid_text(&b, u);
    *out = sv(vm, b.data, b.len);
    buf_free(&b);
    return true;
}
NATIVE(u_is_valid) {
    (void)argc;
    STR(0, "uuid.is_valid");
    const char *s = AS_CSTR(args[0]);
    bool ok = strlen(s) == 36;
    for (int i = 0; ok && i < 36; i++)
        ok = (i == 8 || i == 13 || i == 18 || i == 23) ? s[i] == '-' : hexd(s[i]) >= 0;
    *out = val_bool(ok);
    return true;
}

/* ---------------------------------------------------------------- html */

NATIVE(h_render) {
    (void)argc;
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
NATIVE(h_escape) {
    (void)argc;
    ObjString *s = value_to_string(vm, args[0]);
    if (!s)
        return false;
    Buf b;
    buf_init(&b);
    html_escape(&b, s->chars, (size_t)s->len, false);
    *out = sv(vm, b.data ? b.data : "", b.len);
    buf_free(&b);
    return true;
}

/* ---------------------------------------------------------------- registry */

void lib_json_module(VM *vm, ObjModule *m);

static const char *unavailable[] = {"web", "net", "thread", "sqlite", "regex", "desktop", "crypto", "compress",
                                    "toml", "yaml", "flywheel", NULL};
bool lib_is_native_module(const char *name) {
    for (int i = 0; unavailable[i]; i++)
        if (!strcmp(unavailable[i], name))
            return true;
    return false;
}

ObjModule *lib_native_module(VM *vm, const char *name) {
    static const char *known[] = {"math", "fs", "os", "time", "io", "json", "testing", "collections",
                                  "encoding", "uuid", "html", "svg", "gc", "core", NULL};
    bool ok = false;
    for (int i = 0; known[i]; i++)
        if (!strcmp(known[i], name))
            ok = true;
    if (!ok)
        return NULL;
    if (!strcmp(name, "svg"))
        name = "html";
    char key[64];
    snprintf(key, sizeof key, "<native %s>", name);
    ObjString *k = str_intern_c(vm, key);
    Value existing;
    if (table_get(&vm->modules, k, &existing))
        return AS_MODULE(existing);
    vm->gc_off++;
    ObjModule *m = new_module(vm, str_intern_c(vm, name));
    m->native = true;
    m->initialized = true;
    table_set(vm, &vm->modules, k, val_obj(m));
    if (!strcmp(name, "math"))
        make_math(vm, m);
    else if (!strcmp(name, "fs"))
        make_fs(vm, m);
    else if (!strcmp(name, "os"))
        make_os(vm, m);
    else if (!strcmp(name, "time"))
        make_time(vm, m);
    else if (!strcmp(name, "io"))
        make_io(vm, m);
    else if (!strcmp(name, "json"))
        lib_json_module(vm, m);
    else if (!strcmp(name, "testing"))
        make_testing(vm, m);
    else if (!strcmp(name, "collections"))
        module_set_native(vm, m, "range", c_range, 1, 2);
    else if (!strcmp(name, "encoding"))
        make_encoding(vm, m);
    else if (!strcmp(name, "uuid")) {
        module_set_native(vm, m, "v4", u_v4, 0, 0);
        module_set_native(vm, m, "v7", u_v7, 0, 0);
        module_set_native(vm, m, "is_valid", u_is_valid, 1, 1);
    } else if (!strcmp(name, "html") || !strcmp(name, "svg")) {
        lib_html_module(vm, m);
        module_set_native(vm, m, "render", h_render, 1, 1);
        module_set_native(vm, m, "escape", h_escape, 1, 1);
    } else if (!strcmp(name, "gc")) {
        module_set_native(vm, m, "collect", gc_collect_native, 0, 0);
        module_set_native(vm, m, "pending", gc_pending, 0, 0);
    } else if (!strcmp(name, "core")) {
        static const char *names[] = {"print", "input", "len", "typeof", "string", "int", "double", "bool", "char", NULL};
        for (int i = 0; names[i]; i++) {
            Value v;
            if (table_get(&vm->builtins, str_intern_c(vm, names[i]), &v))
                module_set_value(vm, m, names[i], v);
        }
    }
    vm->gc_off--;
    return m;
}
