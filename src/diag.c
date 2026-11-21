/* Diagnostics: runtime tracebacks and compile errors with source excerpts and color, and the
 * last-error record behind `flywheel-lite --error FILE`.
 *
 *   Flywheel runtime error: NumericError: integer division by zero
 *     at main.fw:2:14 in divide
 *        2 | return a // b
 *          |        ~~^^~~
 *     at main.fw:13:7 in <module>
 *       13 | print divide(1, 0)
 *          |       ^^^^^^^^^^^^
 *
 * The output matches the Flywheel compiler's (runtime/diagnostic.inc there) and flywheel.js's. */
#include "lite.h"
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <io.h>
#include <windows.h>
#define isatty _isatty
#define fileno _fileno
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

/* ---------------------------------------------------------------- color */

static bool env_set(const char *name) {
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}
bool diag_color(void) {
    static int cached = -1;
    if (cached >= 0)
        return cached;
    if (getenv("NO_COLOR") && *getenv("NO_COLOR"))
        cached = 0;
    else if (env_set("FORCE_COLOR") || env_set("CLICOLOR_FORCE"))
        cached = 1;
    else {
        const char *term = getenv("TERM");
        cached = isatty(fileno(stderr)) && !(term && !strcmp(term, "dumb"));
#ifdef _WIN32
        if (cached) {
            HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
            DWORD mode = 0;
            cached = GetConsoleMode(h, &mode) &&
                     SetConsoleMode(h, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */);
        }
#endif
    }
    return cached;
}

#define RESET "\033[0m"
#define BOLD "\033[1m"
#define DIM "\033[2m"
#define RED "\033[31m"
#define BRED "\033[1;31m"
#define BMAGENTA "\033[1;35m"
#define CYAN "\033[36m"
#define BLUE "\033[34m"

static void paint(Buf *b, bool color, const char *code, const char *text) {
    if (color)
        buf_adds(b, code);
    buf_adds(b, text);
    if (color)
        buf_adds(b, RESET);
}

/* ---------------------------------------------------------------- reports */

DiagReport *diag_new(void) {
    DiagReport *r = xmalloc(sizeof *r);
    memset(r, 0, sizeof *r);
    return r;
}
void diag_reset(DiagReport *r) {
    free(r->message);
    memset(r, 0, sizeof *r);
}
void diag_set_message(DiagReport *r, const char *message) {
    free(r->message);
    r->message = xstrdup(message ? message : "");
}

/* ---------------------------------------------------------------- paths */

static bool is_sep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}
static bool is_absolute(const char *p) {
#ifdef _WIN32
    return (p[0] && p[1] == ':') || is_sep(p[0]);
#else
    return p[0] == '/';
#endif
}
/* A path as shown: relative to the working directory when it lies inside it. */
static const char *shown_path(const char *path) {
    static char cwd[1024];
    static bool have = false;
    if (!have) {
        have = true;
        if (!getcwd(cwd, sizeof cwd))
            cwd[0] = 0;
    }
    size_t n = strlen(cwd);
    if (n && is_absolute(path) && !strncmp(path, cwd, n) && is_sep(path[n]) && path[n + 1])
        return path + n + 1;
    return path;
}
/* The absolute form of a path to an existing file, in out (or path itself). */
const char *diag_absolute(const char *path, char *out, size_t size) {
#ifdef _WIN32
    if (_fullpath(out, path, size))
        return out;
#else
    char *real = realpath(path, NULL);
    if (real && strlen(real) < size) {
        strcpy(out, real);
        free(real);
        return out;
    }
    free(real);
#endif
    snprintf(out, size, "%s", path);
    return out;
}

/* ---------------------------------------------------------------- source excerpts */

typedef struct {
    char *path;
    char *text;
    size_t len;
} Cached;
static Cached cache[8];
static int ncache;

static const char *source_of(const char *path, size_t *len, const char *main_name, const char *main_src,
                             size_t main_len) {
    if (main_name && main_src && !strcmp(path, main_name)) {
        *len = main_len;
        return main_src;
    }
    if (path[0] == '<')
        return NULL; /* <source>, <stdin>, <library csv>: no file to read */
    for (int i = 0; i < ncache; i++)
        if (!strcmp(cache[i].path, path)) {
            *len = cache[i].len;
            return cache[i].text;
        }
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
    if (ncache == (int)(sizeof cache / sizeof *cache)) {
        free(cache[0].path);
        free(cache[0].text);
        memmove(cache, cache + 1, sizeof cache - sizeof *cache);
        ncache--;
    }
    cache[ncache].path = xstrdup(path);
    cache[ncache].text = b.data;
    cache[ncache].len = b.len;
    *len = b.len;
    return cache[ncache++].text;
}

static bool word_byte(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }
/* The end (exclusive, 0-based) of the token starting at byte `at` of a line: a name or number, a
 * quoted literal, or a run of operator characters. */
static size_t token_end(const char *s, size_t n, size_t at) {
    if (at >= n)
        return at + 1;
    unsigned char c = (unsigned char)s[at];
    size_t e = at + 1;
    if (word_byte(c)) {
        while (e < n && (word_byte((unsigned char)s[e]) || (isdigit(c) && s[e] == '.')))
            e++;
    } else if (c == '"' || c == '\'') {
        while (e < n && s[e] != (char)c)
            e += s[e] == '\\' ? 2 : 1;
        e = e < n ? e + 1 : n;
    } else if (strchr("+-*/%=<>!&|^~?:.", c)) {
        while (e < n && e < at + 3 && strchr("+-*/%=<>!&|^~?:.", s[e]) && s[e])
            e++;
    }
    return e;
}

/* Display width of bytes [from, to) of a line: one column per character, four per tab. */
static int width(const char *s, size_t from, size_t to) {
    int w = 0;
    for (size_t i = from; i < to; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t')
            w += 4;
        else if ((c & 0xc0) != 0x80)
            w++;
    }
    return w;
}

static int digits(int n) {
    int d = 1;
    while (n >= 10)
        n /= 10, d++;
    return d;
}

/* Two lines under a location: the source line (indentation removed), and markers under the
 * failing range: `^` under the node itself, `~` under the rest of its expression. */
static void excerpt(Buf *b, const SrcSpan *at, int gutter, bool color, const char *text, size_t tlen) {
    if (!text || at->line < 1)
        return;
    size_t ls = 0;
    for (int l = 1; l < at->line; l++) {
        const char *nl = memchr(text + ls, '\n', tlen - ls);
        if (!nl)
            return;
        ls = (size_t)(nl - text) + 1;
    }
    const char *nl = memchr(text + ls, '\n', tlen - ls);
    size_t le = nl ? (size_t)(nl - text) : tlen;
    const char *s = text + ls;
    size_t n = le - ls;
    while (n && (s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        n--;
    size_t ind = 0;
    while (ind < n && (s[ind] == ' ' || s[ind] == '\t'))
        ind++;
    if (ind == n)
        return;
    size_t focus = at->col > 0 ? (size_t)at->col - 1 : ind;
    if (focus < ind)
        focus = ind;
    if (focus > n)
        focus = n;
    size_t start, end;
    if (at->eline > 0) {
        start = at->sline < at->line ? ind : at->sline == at->line && at->scol > 0 ? (size_t)at->scol - 1 : focus;
        end = at->eline > at->line ? n : at->ecol > 0 ? (size_t)at->ecol - 1 : n;
    } else {
        start = focus;
        end = token_end(s, n, focus);
    }
    if (start < ind)
        start = ind;
    if (focus < start || focus >= end)
        start = focus < start ? focus : start;
    if (end > n)
        end = n;
    if (end <= start)
        end = start + 1;
    size_t fend = token_end(s, n, focus);
    if (fend > end)
        fend = end;
    bool whole = focus == start; /* a call or a name: carets under all of it */

    /* the line, its failing range in bold */
    buf_printf(b, "    %s%*d |%s ", color ? BLUE : "", gutter, at->line, color ? RESET : "");
    for (size_t i = ind; i < n; i++) {
        if (color && i == start)
            buf_adds(b, BOLD);
        if (s[i] == '\t')
            buf_adds(b, "    ");
        else
            buf_addc(b, s[i]);
        if (color && i + 1 == end)
            buf_adds(b, RESET);
    }
    if (color && end > n)
        buf_adds(b, RESET);
    buf_addc(b, '\n');
    buf_printf(b, "    %s%*s |%s ", color ? BLUE : "", gutter, "", color ? RESET : "");
    buf_printf(b, "%*s", width(s, ind, start), "");
    if (color)
        buf_adds(b, BRED);
    int before = width(s, start, focus), mid = width(s, focus, fend), after = width(s, fend, end);
    if (end > n)
        after += (int)(end - n);
    if (mid < 1)
        mid = 1;
    for (int i = 0; i < before; i++)
        buf_addc(b, whole ? '^' : '~');
    for (int i = 0; i < mid; i++)
        buf_addc(b, '^');
    for (int i = 0; i < after; i++)
        buf_addc(b, whole ? '^' : '~');
    if (color)
        buf_adds(b, RESET);
    buf_addc(b, '\n');
}

static void header(Buf *b, const DiagReport *r, bool color) {
    if (r->compile) {
        const DiagFrame *f = r->nframes ? &r->frames[0] : NULL;
        if (f) {
            char loc[700];
            snprintf(loc, sizeof loc, "%s:%d:%d:", shown_path(f->file), f->at.line, f->at.col ? f->at.col : 1);
            paint(b, color, BOLD, loc);
            buf_addc(b, ' ');
        }
        paint(b, color, BRED, "error:");
        buf_addc(b, ' ');
        paint(b, color, BOLD, r->message ? r->message : "");
        buf_addc(b, '\n');
        return;
    }
    paint(b, color, BRED, "Flywheel runtime error:");
    buf_addc(b, ' ');
    paint(b, color, BMAGENTA, r->type);
    if (!r->bare) {
        buf_adds(b, ": ");
        paint(b, color, BOLD, r->message && r->message[0] ? r->message : "exception");
    }
    buf_addc(b, '\n');
}

static bool same_frame(const DiagFrame *a, const DiagFrame *b) {
    return !strcmp(a->file, b->file) && !strcmp(a->fn, b->fn) && a->at.line == b->at.line && a->at.col == b->at.col;
}

void diag_print(const DiagReport *r, bool source, const char *main_name, const char *main_src, size_t main_len) {
    bool color = diag_color();
    Buf b;
    buf_init(&b);
    header(&b, r, color);
    int gutter = 1;
    for (int i = 0; i < r->nframes; i++)
        if (digits(r->frames[i].at.line) > gutter)
            gutter = digits(r->frames[i].at.line);
    bool covered = false; /* the left-out frames were counted as repeats */
    for (int i = 0; i < r->nframes; i++) {
        const DiagFrame *f = &r->frames[i];
        if (r->omitted && i == r->omitted_at && !covered) {
            char note[96];
            snprintf(note, sizeof note, "  ... %d more frame%s ...", r->omitted, r->omitted == 1 ? "" : "s");
            paint(&b, color, DIM, note);
            buf_addc(&b, '\n');
        }
        /* recursion: the same call three times, then a count (frames left out of a deep stack
         * between two copies of the call count as copies too) */
        int repeats = 0, hidden = 0;
        while (i + repeats + 1 < r->nframes && same_frame(f, &r->frames[i + repeats + 1])) {
            if (r->omitted && i + repeats + 1 == r->omitted_at)
                hidden = r->omitted;
            repeats++;
        }
        if (!r->compile) {
            char loc[700];
            snprintf(loc, sizeof loc, "%s:%d:%d", shown_path(f->file), f->at.line, f->at.col ? f->at.col : 1);
            buf_adds(&b, "  ");
            paint(&b, color, DIM, "at");
            buf_addc(&b, ' ');
            paint(&b, color, CYAN, loc);
            buf_addc(&b, ' ');
            paint(&b, color, DIM, "in");
            buf_addc(&b, ' ');
            paint(&b, color, BOLD, f->fn);
            buf_addc(&b, '\n');
        }
        if (source) {
            size_t len = 0;
            const char *text = source_of(f->file, &len, main_name, main_src, main_len);
            excerpt(&b, &f->at, gutter, color, text, len);
        }
        if (repeats >= 3) {
            for (int k = 1; k < 3; k++) {
                /* two more as written, so the cycle is visible */
                const DiagFrame *g = &r->frames[i + k];
                char loc[700];
                snprintf(loc, sizeof loc, "%s:%d:%d", shown_path(g->file), g->at.line, g->at.col ? g->at.col : 1);
                buf_adds(&b, "  ");
                paint(&b, color, DIM, "at");
                buf_addc(&b, ' ');
                paint(&b, color, CYAN, loc);
                buf_addc(&b, ' ');
                paint(&b, color, DIM, "in");
                buf_addc(&b, ' ');
                paint(&b, color, BOLD, g->fn);
                buf_addc(&b, '\n');
            }
            char note[96];
            int more = repeats - 2 + hidden;
            snprintf(note, sizeof note, "  ... the same call %d more time%s ...", more, more == 1 ? "" : "s");
            paint(&b, color, DIM, note);
            buf_addc(&b, '\n');
            covered = covered || hidden;
            i += repeats;
        }
    }
    fflush(stdout);
    fwrite(b.data ? b.data : "", 1, b.len, stderr);
    fflush(stderr);
    buf_free(&b);
}

void diag_compile_error(DiagReport *r, const char *path, int line, int col, const char *message,
                        const char *src, size_t len) {
    diag_reset(r);
    r->compile = true;
    diag_set_message(r, message);
    snprintf(r->frames[0].file, sizeof r->frames[0].file, "%s", path);
    r->frames[0].at = (SrcSpan){line, col ? col : 1, 0, 0, 0, 0};
    r->nframes = 1;
    diag_print(r, true, path, src, len);
}

/* ---------------------------------------------------------------- the last-error record */

static void make_dirs(char *path) {
    for (char *p = path + 1; *p; p++)
        if (is_sep(*p)) {
            char c = *p;
            *p = 0;
#ifdef _WIN32
            _mkdir(path);
#else
            mkdir(path, 0755);
#endif
            *p = c;
        }
}
/* <state dir>/flywheel-lite/errors/<hash of the absolute source path>.txt */
static bool record_path(const char *source_path, char *out, size_t size, char *abs, size_t abs_size) {
    diag_absolute(source_path, abs, abs_size);
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)abs; *p; p++)
        h = (h ^ *p) * 1099511628211ULL;
    const char *base = NULL, *sub = "";
    char buf[1024];
#ifdef _WIN32
    base = getenv("LOCALAPPDATA");
#else
    base = getenv("XDG_STATE_HOME");
    if (!base || !*base) {
        const char *home = getenv("HOME");
        if (!home || !*home)
            return false;
        snprintf(buf, sizeof buf, "%s/.local/state", home);
        base = buf;
    }
#endif
    if (!base || !*base)
        return false;
    snprintf(out, size, "%s%s/flywheel-lite/errors/%016llx.txt", base, sub, (unsigned long long)h);
    return true;
}

static void put_field(FILE *f, const char *key, const char *value) {
    fprintf(f, "%s ", key);
    for (const char *p = value; *p; p++) {
        if (*p == '\\')
            fputs("\\\\", f);
        else if (*p == '\n')
            fputs("\\n", f);
        else if (*p == '\t')
            fputs("\\t", f);
        else if (*p != '\r')
            fputc(*p, f);
    }
    fputc('\n', f);
}
static void unescape(char *s) {
    char *w = s;
    for (char *p = s; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            *w++ = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
        } else
            *w++ = *p;
    }
    *w = 0;
}

void diag_record(const char *source_path, const DiagReport *r) {
    if (!source_path || source_path[0] == '<')
        return;
    char path[1200], abs[1024];
    if (!record_path(source_path, path, sizeof path, abs, sizeof abs))
        return;
    make_dirs(path);
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return;
    fputs("flywheel-error 1\n", f);
    put_field(f, "source", abs);
    fprintf(f, "time %lld\n", (long long)time(NULL));
    fprintf(f, "kind %s\n", r->compile ? "compile" : r->bare ? "bare" : "runtime");
    put_field(f, "type", r->type);
    put_field(f, "message", r->message ? r->message : "");
    if (r->omitted)
        fprintf(f, "omitted %d %d\n", r->omitted_at, r->omitted);
    for (int i = 0; i < r->nframes; i++) {
        const DiagFrame *fr = &r->frames[i];
        char file[1024];
        const char *shown = fr->file[0] == '<' ? fr->file : diag_absolute(fr->file, file, sizeof file);
        fprintf(f, "frame %d %d %d %d %d %d\t", fr->at.line, fr->at.col, fr->at.sline, fr->at.scol, fr->at.eline,
                fr->at.ecol);
        put_field(f, "", shown);
        put_field(f, "fn", fr->fn);
    }
    fclose(f);
#ifdef _WIN32
    remove(path);
#endif
    if (rename(tmp, path) != 0)
        remove(tmp);
}

int diag_show_last(const char *source_path) {
    char path[1200], abs[1024];
    FILE *f = record_path(source_path, path, sizeof path, abs, sizeof abs) ? fopen(path, "rb") : NULL;
    if (!f) {
        fprintf(stderr, "flywheel-lite: no error recorded for %s\n", source_path);
        return 1;
    }
    DiagReport *r = diag_new();
    char line[8192];
    long long when = 0;
    if (!fgets(line, sizeof line, f) || strncmp(line, "flywheel-error ", 15) != 0) {
        fclose(f);
        fprintf(stderr, "flywheel-lite: the error record for %s is unreadable: %s\n", source_path, path);
        return 1;
    }
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *sp = strchr(line, ' ');
        char *value = sp ? sp + 1 : line + strlen(line);
        if (!strncmp(line, "time ", 5))
            when = atoll(value);
        else if (!strncmp(line, "kind ", 5)) {
            r->compile = !strcmp(value, "compile");
            r->bare = !strcmp(value, "bare");
        } else if (!strncmp(line, "type ", 5)) {
            unescape(value);
            snprintf(r->type, sizeof r->type, "%s", value);
        } else if (!strncmp(line, "message ", 8)) {
            unescape(value);
            diag_set_message(r, value);
        } else if (!strncmp(line, "omitted ", 8))
            sscanf(value, "%d %d", &r->omitted_at, &r->omitted);
        else if (!strncmp(line, "frame ", 6) && r->nframes < DIAG_FRAMES) {
            DiagFrame *fr = &r->frames[r->nframes];
            SrcSpan *a = &fr->at;
            char *tab = strchr(value, '\t');
            if (!tab || sscanf(value, "%d %d %d %d %d %d", &a->line, &a->col, &a->sline, &a->scol, &a->eline,
                               &a->ecol) != 6)
                continue;
            char *file = tab + 2; /* "\t " before the path */
            unescape(file);
            snprintf(fr->file, sizeof fr->file, "%s", file);
            if (fgets(line, sizeof line, f) && !strncmp(line, "fn ", 3)) {
                line[strcspn(line, "\n")] = 0;
                unescape(line + 3);
                snprintf(fr->fn, sizeof fr->fn, "%.127s", line + 3);
            }
            r->nframes++;
        }
    }
    fclose(f);
    bool color = diag_color();
    char stamp[64] = "";
    time_t t = (time_t)when;
    struct tm *tm = localtime(&t);
    if (tm)
        strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", tm);
    Buf b;
    buf_init(&b);
    char intro[1400];
    snprintf(intro, sizeof intro, "last error of %s, %s:", shown_path(abs), stamp);
    paint(&b, color, DIM, intro);
    buf_addc(&b, '\n');
    fwrite(b.data, 1, b.len, stderr);
    buf_free(&b);
    diag_print(r, false, NULL, NULL, 0);
    diag_reset(r);
    free(r);
    return 0;
}
