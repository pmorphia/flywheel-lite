/* Lowering after parsing, ported from the Flywheel compiler (src/units.cpp and src/lower.cpp).
 *
 * Units: each `unitns` becomes hidden classes __fwu_<ns>__int and __fwu_<ns>__double (the types
 * int[ns] and float[ns]) holding a number and the index of its unit, plus top-level tables of unit
 * names and exact conversion ratios. Their methods do the arithmetic; the VM routes operators on
 * quantities to them (vm.c, quantity_arith) and the compiler maps literals, `in`, and
 * type()/unit()/unitns() onto them (compiler.c).
 *
 * Reactive variables: a `reactive` variable gets a hidden watcher list (__fwr_w_<name>) and every
 * assignment to it runs the watchers; `when` becomes a check closure registered with the reactive
 * and computed variables its condition names; `computed` is a reactive variable that a watcher
 * recomputes. Negated aliases (`bool on/off`) read as `not on` and assign `on = not value`.
 * Names resolve lexically, as the compiler resolves them. */
#include "front.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ text buffers */

static void relocate_list(NodeList *l, int line, int col);
static void relocate(Node *n, int line, int col) {
    if (!n)
        return;
    n->line = line;
    n->col = col;
    for (int i = 0; i < n->args.count; i++)
        relocate(n->args.items[i], line, col);
    relocate(n->expr, line, col);
    relocate(n->target, line, col);
    relocate_list(&n->body, line, col);
    relocate_list(&n->other, line, col);
    relocate_list(&n->finalizer, line, col);
    for (int i = 0; i < n->ncatches; i++)
        relocate_list(&n->catches[i], line, col);
    if (n->fn) {
        n->fn->line = line;
        relocate_list(&n->fn->body, line, col);
    }
}
static void relocate_list(NodeList *l, int line, int col) {
    for (int i = 0; i < l->count; i++)
        relocate(l->items[i], line, col);
}

/* Parses generated Flywheel source into `out`; failures are internal errors at line:col. */
static void generated(Parser *P, const char *text, size_t len, Program *out, int line, int col) {
    char *copy = arena_strndup(P->arena, text, len);
    CompileError saved = *P->err;
    if (!parse_program(P->arena, copy, len, out, P->err)) {
        char message[sizeof P->err->message];
        snprintf(message, sizeof message, "%s", P->err->message);
        int at = P->err->line;
        *P->err = saved;
        parser_fail(P, line, col, "internal error in generated code: %s (line %d)", message, at);
    }
    relocate_list(&out->body, line, col);
}
static NodeList generated_body(Parser *P, const char *text, int line, int col) {
    Program prog;
    generated(P, text, strlen(text), &prog, line, col);
    return prog.body;
}

/* ------------------------------------------------------------------ units */

bool quantity_class_name(const char *name, char *ns, size_t nscap, bool *real) {
    const char *dot = strrchr(name, '.');
    if (dot)
        name = dot + 1;
    if (strncmp(name, "__fwu_", 6))
        return false;
    const char *inner = name + 6;
    const char *split = NULL;
    for (const char *p = inner; (p = strstr(p, "__")); p++)
        split = p;
    if (!split || split == inner)
        return false;
    if (!strcmp(split + 2, "int"))
        *real = false;
    else if (!strcmp(split + 2, "double"))
        *real = true;
    else
        return false;
    snprintf(ns, nscap, "%.*s", (int)(split - inner), inner);
    return true;
}

/* A positive rational, exact when both parts fit in 63 bits; `value` always holds the double. */
typedef struct {
    uint64_t num, den;
    bool exact;
    double value;
} Ratio;
static const uint64_t ratio_limit = (uint64_t)1 << 62;
static bool umul(uint64_t a, uint64_t b, uint64_t *r) {
    if (a && b > ratio_limit / a)
        return false;
    *r = a * b;
    return true;
}
static uint64_t ugcd(uint64_t a, uint64_t b) {
    while (b) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}
static bool decimal(const char *text, size_t len, Ratio *r) {
    char tmp[128];
    if (len >= sizeof tmp)
        return false;
    memcpy(tmp, text, len);
    tmp[len] = 0;
    r->value = strtod(tmp, NULL);
    r->exact = true;
    uint64_t mantissa = 0;
    int exponent = 0;
    size_t i = 0;
    bool point = false;
    for (; i < len && (isdigit((unsigned char)tmp[i]) || tmp[i] == '.'); i++) {
        if (tmp[i] == '.') {
            point = true;
            continue;
        }
        if (!umul(mantissa, 10, &mantissa)) {
            r->exact = false;
            return true;
        }
        mantissa += (uint64_t)(tmp[i] - '0');
        if (point)
            exponent--;
    }
    if (i < len && (tmp[i] == 'e' || tmp[i] == 'E'))
        exponent += atoi(tmp + i + 1);
    uint64_t scale = 1;
    for (int k = 0; k < abs(exponent); k++)
        if (!umul(scale, 10, &scale)) {
            r->exact = false;
            return true;
        }
    if (exponent >= 0) {
        if (!umul(mantissa, scale, &r->num)) {
            r->exact = false;
            return true;
        }
        r->den = 1;
    } else {
        r->num = mantissa;
        r->den = scale;
    }
    uint64_t g = ugcd(r->num, r->den);
    if (g > 1) {
        r->num /= g;
        r->den /= g;
    }
    return true;
}
static Ratio ratio_divide(Ratio a, Ratio b) {
    Ratio r = {1, 1, false, a.value / b.value};
    if (!a.exact || !b.exact)
        return r;
    uint64_t g1 = ugcd(a.num, b.num), g2 = ugcd(a.den, b.den);
    if (!g1 || !g2)
        return r;
    if (!umul(a.num / g1, b.den / g2, &r.num) || !umul(a.den / g2, b.num / g1, &r.den))
        return r;
    r.exact = true;
    return r;
}
static Ratio unit_factor(Parser *P, const char *text, int line) {
    const char *slash = strchr(text, '/');
    size_t top_len = slash ? (size_t)(slash - text) : strlen(text);
    Ratio top, bottom = {1, 1, true, 1};
    bool ok = decimal(text, top_len, &top) && (!slash || decimal(slash + 1, strlen(slash + 1), &bottom));
    if (!ok || !(top.value > 0) || !(bottom.value > 0) || !isfinite(top.value) || !isfinite(bottom.value))
        parser_fail(P, line, 1, "a unit's factor must be a positive number%s", "");
    return ratio_divide(top, bottom);
}
/* A double as a Flywheel floating-point literal. */
static const char *real_text(double v, char *buffer, size_t n) {
    snprintf(buffer, n, "%.17g", v);
    if (!strpbrk(buffer, ".en"))
        strncat(buffer, ".0", n - strlen(buffer) - 1);
    return buffer;
}

typedef struct {
    const char *left, *op, *right, *result;
    int result_unit;
    double k; /* result base = left base (op) right base * k */
    bool declared;
    int line;
} Relation;

/* The module's own namespaces, then the imported ones it does not hide. */
typedef struct {
    UnitNs *items;
    int count, own;
} UnitSet;
/* By bare name (own namespaces first, then imported ones reached bare), or qualified as
 * metric.length (imported, by the module's alias). */
static int find_ns(UnitSet *s, const char *name) {
    const char *dot = strchr(name, '.');
    for (int i = 0; i < s->count; i++) {
        UnitNs *ns = &s->items[i];
        if (dot) {
            size_t q = ns->qual ? strlen(ns->qual) : 0;
            if (i >= s->own && q && !strncmp(name, ns->qual, q) && !strcmp(name + q, ns->name))
                return i;
        } else if (!strcmp(ns->name, name) && (i < s->own || (!ns->shadowed && (!ns->ref || !ns->ref[0]))))
            return i;
    }
    return -1;
}
void unit_key(char *out, size_t cap, const char *module, const char *ns) {
    snprintf(out, cap, "%s__%s", module ? module : "", ns);
    for (char *p = out; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_')
            *p = '_';
}
typedef struct {
    int own, other;
    double k; /* other's base = own's base * k */
} Equivalence;
static int unit_index(UnitNs *ns, const char *unit) {
    for (int i = 0; i < ns->nunits; i++)
        if (!strcmp(ns->units[i], unit))
            return i;
    return -1;
}

static void add_relation(Parser *P, Relation *rel, int *n, Relation r) {
    for (int i = 0; i < *n; i++)
        if (!strcmp(rel[i].left, r.left) && !strcmp(rel[i].op, r.op) && !strcmp(rel[i].right, r.right)) {
            if (rel[i].declared && r.declared)
                parser_fail(P, r.line, 1, "unitconv already relates %s %s %s", r.left, r.op, r.right);
            if (r.declared)
                rel[i] = r;
            return;
        }
    rel[(*n)++] = r;
}

static void generate_units(Parser *P, Program *p, const UnitNs *imported, int nimported, bool strict, Buf *out) {
    UnitSet set;
    set.items = arena_alloc(P->arena, sizeof(UnitNs) * (size_t)(p->nunitns + nimported + 1));
    set.count = 0;
    for (int n = 0; n < p->nunitns; n++)
        set.items[set.count++] = p->unitns[n];
    set.own = set.count;
    for (int n = 0; n < nimported; n++)
        set.items[set.count++] = imported[n];
    Ratio **factors = arena_alloc(P->arena, sizeof(Ratio *) * (size_t)set.count);
    for (int n = 0; n < set.count; n++) {
        UnitNs *ns = &set.items[n];
        if (!ns->nunits)
            parser_fail(P, ns->line, ns->col, "unit namespace '%s' has no units; add one, such as unit %s.base = 1",
                        ns->name, ns->name);
        factors[n] = arena_alloc(P->arena, sizeof(Ratio) * (size_t)ns->nunits);
        for (int i = 0; i < ns->nunits; i++)
            factors[n][i] = unit_factor(P, ns->factors[i], ns->unit_lines[i]);
    }
    Equivalence *eq = arena_alloc(P->arena, sizeof(Equivalence) * (size_t)(p->nunitconvs + 1));
    int neq = 0;
    for (int ci = 0; ci < p->nunitconvs; ci++) {
        UnitConv *c = &p->unitconvs[ci];
        if (strcmp(c->op, "="))
            continue;
        int index[2];
        bool known = true;
        for (int k = 0; k < 2; k++) {
            index[k] = find_ns(&set, c->namespaces[k]);
            known = known && index[k] >= 0;
        }
        if (!known && !strict)
            continue;
        for (int k = 0; k < 2; k++)
            if (index[k] < 0)
                parser_fail(P, c->line, c->col,
                            "unknown unit namespace '%s'; declare it with unitns or import a module that declares it",
                            c->namespaces[k]);
        if (index[0] >= set.own && index[1] >= set.own)
            parser_fail(P, c->line, c->col,
                        "unitconv relates namespaces of other modules; relate them in a module that declares one of "
                        "them%s", "");
        if (index[0] == index[1])
            parser_fail(P, c->line, c->col, "unitconv = relates two different namespaces%s", "");
        double base[2];
        for (int k = 0; k < 2; k++) {
            UnitNs *ns = &set.items[index[k]];
            int u = unit_index(ns, c->units[k]);
            if (u < 0)
                parser_fail(P, c->line, c->col, "unit namespace '%s' has no unit '%s'", ns->name, c->units[k]);
            double amount = strtod(c->amounts[k], NULL);
            if (!(amount > 0))
                parser_fail(P, c->line, c->col, "unitconv amounts must be positive%s", "");
            base[k] = amount * factors[index[k]][u].value;
        }
        int a = index[0] < set.own ? 0 : 1, b = 1 - a;
        for (int e = 0; e < neq; e++)
            if ((eq[e].own == index[a] && eq[e].other == index[b]) || (eq[e].own == index[b] && eq[e].other == index[a]))
                parser_fail(P, c->line, c->col, "unitconv already relates %s and %s", c->namespaces[0],
                            c->namespaces[1]);
        eq[neq++] = (Equivalence){index[a], index[b], base[b] / base[a]};
    }
    Relation *rel = arena_alloc(P->arena, sizeof(Relation) * (size_t)(p->nunitconvs * 4 + 1));
    int nrel = 0;
    for (int ci = 0; ci < p->nunitconvs; ci++) {
        UnitConv *c = &p->unitconvs[ci];
        if (!strcmp(c->op, "="))
            continue;
        double base[3];
        int units[3];
        int index[3];
        bool known = true, any_own = false;
        for (int k = 0; k < 3; k++) {
            index[k] = find_ns(&set, c->namespaces[k]);
            known = known && index[k] >= 0;
            any_own = any_own || (index[k] >= 0 && index[k] < set.own);
        }
        if (!known && !strict)
            continue;
        if (known && !any_own)
            parser_fail(P, c->line, c->col,
                        "unitconv relates namespaces of other modules; relate them in a module that declares one of "
                        "them%s", "");
        if (known && !strcmp(c->op, "/") && index[0] >= set.own)
            parser_fail(P, c->line, c->col,
                        "unitconv divides %s, which another module declares; write the relation as a product, such "
                        "as unitconv 1%s.%s * 1%s.%s = 1%s.%s",
                        c->namespaces[0], c->namespaces[2], c->units[2], c->namespaces[1], c->units[1],
                        c->namespaces[0], c->units[0]);
        for (int k = 0; k < 3; k++) {
            if (index[k] < 0)
                parser_fail(P, c->line, c->col,
                            "unknown unit namespace '%s'; declare it with unitns or import a module that declares it",
                            c->namespaces[k]);
            UnitNs *ns = &set.items[index[k]];
            int u = unit_index(ns, c->units[k]);
            if (u < 0)
                parser_fail(P, c->line, c->col, "unit namespace '%s' has no unit '%s'", ns->name, c->units[k]);
            double amount = strtod(c->amounts[k], NULL);
            if (!(amount > 0))
                parser_fail(P, c->line, c->col, "unitconv amounts must be positive%s", "");
            base[k] = amount * factors[index[k]][u].value;
            units[k] = u;
        }
        bool divide = !strcmp(c->op, "/");
        double k = divide ? base[2] / (base[0] / base[1]) : base[2] / (base[0] * base[1]);
        const char *a = c->namespaces[0], *b = c->namespaces[1], *r = c->namespaces[2];
        add_relation(P, rel, &nrel, (Relation){a, c->op, b, r, units[2], k, true, c->line});
        if (divide) {
            add_relation(P, rel, &nrel, (Relation){r, "*", b, a, units[0], 1 / k, false, c->line});
            add_relation(P, rel, &nrel, (Relation){b, "*", r, a, units[0], 1 / k, false, c->line});
            add_relation(P, rel, &nrel, (Relation){a, "/", r, b, units[1], k, false, c->line});
        } else {
            add_relation(P, rel, &nrel, (Relation){b, "*", a, r, units[2], k, false, c->line});
            add_relation(P, rel, &nrel, (Relation){r, "/", b, a, units[0], 1 / k, false, c->line});
            add_relation(P, rel, &nrel, (Relation){r, "/", a, b, units[1], 1 / k, false, c->line});
        }
    }
    char t1[64];
    for (int n = 0; n < p->nunitns; n++) {
        UnitNs *ns = &p->unitns[n];
        Ratio *f = factors[n];
        int count = ns->nunits;
        const char *N = ns->name;
        buf_printf(out, "const __fwu_%s__names = [", N);
        for (int i = 0; i < count; i++)
            buf_printf(out, "%s\"%s\"", i ? ", " : "", ns->units[i]);
        buf_printf(out, "]\nconst __fwu_%s__factor = [", N);
        for (int i = 0; i < count; i++)
            buf_printf(out, "%s%s", i ? ", " : "", real_text(f[i].value, t1, sizeof t1));
        const char *tables[] = {"finer", "num", "den", "mul"};
        for (int t = 0; t < 4; t++) {
            buf_printf(out, "]\nconst __fwu_%s__%s = [", N, tables[t]);
            for (int i = 0; i < count; i++)
                for (int j = 0; j < count; j++) {
                    Ratio r = ratio_divide(f[i], f[j]);
                    const char *sep = i || j ? ", " : "";
                    if (t == 0)
                        buf_printf(out, "%s%d", sep, f[i].value <= f[j].value ? i : j);
                    else if (t == 1)
                        buf_printf(out, "%s%s", sep, real_text(r.exact ? (double)r.num : r.value, t1, sizeof t1));
                    else if (t == 2)
                        buf_printf(out, "%s%s", sep, real_text(r.exact ? (double)r.den : 1.0, t1, sizeof t1));
                    else
                        buf_printf(out, "%s%llu", sep, r.exact && r.den == 1 ? (unsigned long long)r.num : 0ULL);
                }
        }
        buf_printf(out, "]\n");
        char at_self[128], at_other[128];
        snprintf(at_self, sizeof at_self, "[this.__fwu_unit * %d + j]", count);
        snprintf(at_other, sizeof at_other, "[this.__fwu_unit * %d + o.__fwu_unit]", count);
        for (int kind = 0; kind < 2; kind++) {
            bool is_int = kind == 0;
            char C[160], D[160];
            snprintf(C, sizeof C, "__fwu_%s__%s", N, is_int ? "int" : "double");
            snprintf(D, sizeof D, "__fwu_%s__double", N);
            const char *T = is_int ? "int" : "double";
            const char *number = is_int ? "double(this.value)" : "this.value";
            buf_printf(out,
                       "class %s\n"
                       "    readonly %s value\n"
                       "    readonly int __fwu_unit\n"
                       "    function to_string returns string\n"
                       "        return \"{this.value}\" + __fwu_%s__names[this.__fwu_unit]\n"
                       "    end\n"
                       "    function operator -() returns %s\n"
                       "        return %s(-this.value, this.__fwu_unit)\n"
                       "    end\n"
                       "    function __fwu_unit_name() returns string\n"
                       "        return __fwu_%s__names[this.__fwu_unit]\n"
                       "    end\n"
                       "    function __fwu_type() returns string\n"
                       "        return \"%s\"\n"
                       "    end\n"
                       "    function __fwu_ns() returns string\n"
                       "        return \"%s\"\n"
                       "    end\n"
                       "    function __fwu_real(int j) returns double\n"
                       "        return %s * __fwu_%s__num%s / __fwu_%s__den%s\n"
                       "    end\n"
                       "    function __fwu_base() returns double\n"
                       "        return %s * __fwu_%s__factor[this.__fwu_unit]\n"
                       "    end\n"
                       /* `x in cm`: the unit by name */
                       "    function __fwu_to_named(string u) returns %s\n"
                       "        var j = __fwu_%s__names.index_of(u)\n"
                       "        if j < 0 do\n"
                       "            throw ValueError(\"unit namespace '%s' has no unit '\" + u + \"'\")\n"
                       "        end\n"
                       "        return this.__fwu_to(j)\n"
                       "    end\n",
                       C, T, N, C, C, N, is_int ? "int" : "float", N, number, N, at_self, N, at_self,
                       number, N, D, N, N);
            if (is_int) {
                buf_printf(out,
                           "    function __fwu_exact(int j) returns int\n"
                           "        if this.__fwu_unit == j do\n"
                           "            return this.value\n"
                           "        end\n"
                           "        var m = __fwu_%s__mul%s\n"
                           "        if m == 0 do\n"
                           "            throw ValueError(\"{this.value}\" + __fwu_%s__names[this.__fwu_unit] + \" is not "
                           "a whole number of \" + __fwu_%s__names[j] + \"; use %s[%s]\")\n"
                           "        end\n"
                           "        return this.value * m\n"
                           "    end\n",
                           N, at_self, N, N, "float", N);
                const char *ops[3][2] = {{"add", "+"}, {"sub", "-"}, {"mod", "%"}};
                for (int o = 0; o < 3; o++)
                    buf_printf(out,
                               "    function __fwu_%s(%s o) returns %s\n"
                               "        var j = __fwu_%s__finer%s\n"
                               "        return %s(this.__fwu_exact(j) %s o.__fwu_exact(j), j)\n"
                               "    end\n",
                               ops[o][0], C, C, N, at_other, C, ops[o][1]);
                char at_o_j[128];
                snprintf(at_o_j, sizeof at_o_j, "[o.__fwu_unit * %d + j]", count);
                buf_printf(out,
                           "    function __fwu_cmp(%s o) returns int\n"
                           "        var j = __fwu_%s__finer%s\n"
                           "        if __fwu_%s__mul%s != 0 and __fwu_%s__mul%s != 0 do\n"
                           "            var a = this.__fwu_exact(j)\n"
                           "            var b = o.__fwu_exact(j)\n"
                           "            if a < b do\n                return -1\n            end\n"
                           "            if a > b do\n                return 1\n            end\n"
                           "            return 0\n"
                           "        end\n"
                           "        var x = this.__fwu_real(j)\n"
                           "        var y = o.__fwu_real(j)\n"
                           "        if x < y do\n            return -1\n        end\n"
                           "        if x > y do\n            return 1\n        end\n"
                           "        return 0\n"
                           "    end\n"
                           "    function __fwu_scale(int k) returns %s\n"
                           "        return %s(this.value * k, this.__fwu_unit)\n"
                           "    end\n"
                           "    function __fwu_floordiv(int k) returns %s\n"
                           "        return %s(this.value // k, this.__fwu_unit)\n"
                           "    end\n",
                           C, N, at_other, N, at_self, N, at_o_j, C, C, C, C);
                buf_printf(out,
                           "    function __fwu_float() returns %s\n"
                           "        return %s(double(this.value), this.__fwu_unit)\n"
                           "    end\n"
                           "    function __fwu_to(int j) returns %s\n"
                           "        return %s(this.__fwu_real(j), j)\n"
                           "    end\n",
                           D, D, D, D);
            } else {
                const char *ops[3][2] = {{"add", "+"}, {"sub", "-"}, {"mod", "%"}};
                for (int o = 0; o < 3; o++)
                    buf_printf(out,
                               "    function __fwu_%s(%s o) returns %s\n"
                               "        var j = __fwu_%s__finer%s\n"
                               "        return %s(this.__fwu_real(j) %s o.__fwu_real(j), j)\n"
                               "    end\n",
                               ops[o][0], C, C, N, at_other, C, ops[o][1]);
                buf_printf(out,
                           "    function __fwu_cmp(%s o) returns int\n"
                           "        var j = __fwu_%s__finer%s\n"
                           "        var x = this.__fwu_real(j)\n"
                           "        var y = o.__fwu_real(j)\n"
                           "        if x < y do\n            return -1\n        end\n"
                           "        if x > y do\n            return 1\n        end\n"
                           "        return 0\n"
                           "    end\n"
                           "    function __fwu_scale(double k) returns %s\n"
                           "        return %s(this.value * k, this.__fwu_unit)\n"
                           "    end\n"
                           "    function __fwu_divide(double k) returns %s\n"
                           "        return %s(this.value / k, this.__fwu_unit)\n"
                           "    end\n"
                           "    function __fwu_to(int j) returns %s\n"
                           "        return %s(this.__fwu_real(j), j)\n"
                           "    end\n",
                           C, N, at_other, C, C, C, C, C, C);
                /* equivalences: this value in another namespace's unit (o's), and o in this one's */
                for (int e = 0; e < neq; e++) {
                    if (eq[e].own != n)
                        continue;
                    UnitNs *o = &set.items[eq[e].other];
                    const char *ref = eq[e].other < set.own || !o->ref || !o->ref[0] ? "" : o->qual;
                    char k1[64];
                    real_text(eq[e].k, k1, sizeof k1);
                    buf_printf(out,
                               "    function __fwu_as__%s(%s__fwu_%s__double o) returns %s__fwu_%s__double\n"
                               "        return %s__fwu_%s__double(this.__fwu_base() * %s / %s__fwu_%s__factor[o.__fwu_unit], "
                               "o.__fwu_unit)\n"
                               "    end\n"
                               "    function __fwu_from__%s(%s__fwu_%s__double o) returns %s\n"
                               "        return %s(o.__fwu_base() / %s / __fwu_%s__factor[this.__fwu_unit], this.__fwu_unit)\n"
                               "    end\n",
                               o->key, ref, o->name, ref, o->name, ref, o->name, k1, ref, o->name, o->key, ref, o->name,
                               C, C, k1, N);
                }
                for (int r = 0; r < nrel; r++) {
                    if (strcmp(rel[r].left, N))
                        continue;
                    buf_printf(out,
                               "    function __fwu_%s__%s(__fwu_%s__double o) returns __fwu_%s__double\n"
                               "        return __fwu_%s__double(this.__fwu_base() %s o.__fwu_base() * %s / "
                               "__fwu_%s__factor[%d], %d)\n"
                               "    end\n",
                               !strcmp(rel[r].op, "*") ? "mul" : "div", rel[r].right, rel[r].right, rel[r].result,
                               rel[r].result, rel[r].op, real_text(rel[r].k, t1, sizeof t1), rel[r].result,
                               rel[r].result_unit, rel[r].result_unit);
                }
            }
            buf_printf(out,
                       "    function __fwu_abs() returns %s\n"
                       "        return %s(-this.value if this.value < %s else this.value, this.__fwu_unit)\n"
                       "    end\n",
                       C, C, is_int ? "0" : "0.0");
            buf_printf(out,
                       "    function __fwu_ratio(%s o) returns double\n"
                       "        var j = __fwu_%s__finer%s\n"
                       "        return this.__fwu_real(j) / o.__fwu_real(j)\n"
                       "    end\n"
                       "    function equals(%s o) returns bool\n"
                       "        return this.__fwu_cmp(o) == 0\n"
                       "    end\n"
                       /* Equal quantities hash alike: the base value, normalized to 12 digits. */
                       "    function hash() returns int\n"
                       "        var h = this.__fwu_base()\n"
                       "        if h < 0.0 do\n            h = -h\n        end\n"
                       "        if h == 0.0 or h != h or h > 1e300 do\n            return 0\n        end\n"
                       "        while h >= 1000000000000.0 do\n            h = h / 10.0\n        end\n"
                       "        while h < 100000000000.0 do\n            h = h * 10.0\n        end\n"
                       "        return int(h + 0.5)\n"
                       "    end\n"
                       "end\n",
                       C, N, at_other, C);
        }
    }
}

void lower_units(Parser *P, Program *p, const UnitNs *imported, int nimported, bool strict) {
    if (!p->nunitns) {
        if (p->nunitconvs && (strict || !nimported))
            parser_fail(P, p->unitconvs[0].line, p->unitconvs[0].col, "%s",
                        nimported ? "unitconv relates namespaces of other modules; relate them in a module that "
                                    "declares one of them"
                                  : "unitconv needs unit namespaces; declare them with unitns");
        p->nunitconvs = 0;
        return;
    }
    Buf text = {0};
    generate_units(P, p, imported, nimported, strict, &text);
    p->nunitconvs = 0;
    Program gen;
    int line = p->unitns[0].line, col = p->unitns[0].col;
    generated(P, text.data, text.len, &gen, line, col);
    free(text.data);
    ClassDecl *classes = arena_alloc(P->arena, sizeof(ClassDecl) * (size_t)(p->nclasses + gen.nclasses));
    if (p->nclasses)
        memcpy(classes, p->classes, sizeof(ClassDecl) * (size_t)p->nclasses);
    for (int i = 0; i < gen.nclasses; i++) {
        /* errors raised inside the generated methods point at the unitns declaration */
        gen.classes[i].line = line;
        for (int f = 0; f < gen.classes[i].nfields; f++)
            gen.classes[i].fields[f].line = line;
        for (int m = 0; m < gen.classes[i].nmethods; m++) {
            gen.classes[i].methods[m].line = line;
            relocate_list(&gen.classes[i].methods[m].body, line, col);
        }
        classes[p->nclasses + i] = gen.classes[i];
    }
    p->classes = classes;
    p->nclasses += gen.nclasses;
    NodeList body = gen.body;
    for (int i = 0; i < p->body.count; i++)
        nl_push(P->arena, &body, p->body.items[i]);
    p->body = body;
}

/* ------------------------------------------------------------------ reactive variables */

enum { B_PLAIN, B_REACTIVE, B_COMPUTED, B_ALIAS };
typedef struct {
    const char *name, *target;
    int kind, depth;
} Binding;
typedef struct {
    const char **items;
    int count, cap;
} Names;
static bool names_has(const Names *n, const char *name) {
    for (int i = 0; name && i < n->count; i++)
        if (!strcmp(n->items[i], name))
            return true;
    return false;
}
static void names_add(Names *n, const char *name) {
    if (!name || names_has(n, name))
        return;
    if (n->count == n->cap) {
        n->cap = GROW_CAP(n->cap);
        n->items = realloc(n->items, sizeof(const char *) * (size_t)n->cap);
    }
    n->items[n->count++] = name;
}
/* What an expression or a function body reads: plain names, fields (member accesses and
 * `this.field` reads that are not called), and the functions and methods it calls. */
typedef struct {
    Names names, fields;
    FuncDecl **calls;
    int ncalls, capcalls;
} Reads;
static void reads_call(Reads *r, FuncDecl *f) {
    for (int i = 0; i < r->ncalls; i++)
        if (r->calls[i] == f)
            return;
    if (r->ncalls == r->capcalls) {
        r->capcalls = GROW_CAP(r->capcalls);
        r->calls = realloc(r->calls, sizeof(FuncDecl *) * (size_t)r->capcalls);
    }
    r->calls[r->ncalls++] = f;
}
static void reads_free(Reads *r) {
    free(r->names.items);
    free(r->fields.items);
    free(r->calls);
}
typedef struct {
    Parser *P;
    Program *prog;
    Binding *items;
    int count, cap, depth, counter;
    /* Names that a `when` condition or `computed` expression reads: a plain variable with one of
     * these names is made reactive, so `when delay == 10s do` works on `int[time] delay`. */
    Names watched;
    /* Top-level variables read by the functions a condition calls: made reactive at the top level. */
    Names watched_top;
    /* Fields that conditions read, directly or through the functions they call. */
    Names fields;
    /* What each function's own body reads, computed once */
    FuncDecl **direct_fn;
    Reads *direct;
    int ndirect, capdirect;
    int top_count; /* bindings of the top-level scope: items[0 .. top_count) */
} Lowerer;

static void scan(Lowerer *L, Node *e, Reads *r, const Names *locals);
static void scan_list(Lowerer *L, NodeList *l, Reads *r, const Names *locals);
static void calls_named(Lowerer *L, const char *name, bool method, Reads *r) {
    Program *p = L->prog;
    if (!method) {
        for (int i = 0; i < p->nfunctions; i++)
            if (p->functions[i].name && !strcmp(p->functions[i].name, name))
                reads_call(r, &p->functions[i]);
        return;
    }
    for (int c = 0; c < p->nclasses; c++) {
        for (int m = 0; m < p->classes[c].nmethods; m++)
            if (p->classes[c].methods[m].name && !strcmp(p->classes[c].methods[m].name, name))
                reads_call(r, &p->classes[c].methods[m]);
        for (int m = 0; m < p->classes[c].nstatic_methods; m++)
            if (p->classes[c].static_methods[m].name && !strcmp(p->classes[c].static_methods[m].name, name))
                reads_call(r, &p->classes[c].static_methods[m]);
    }
}
static bool is_function(Lowerer *L, const char *name) {
    for (int i = 0; i < L->prog->nfunctions; i++)
        if (L->prog->functions[i].name && !strcmp(L->prog->functions[i].name, name))
            return true;
    return false;
}
static void scan(Lowerer *L, Node *e, Reads *r, const Names *locals) {
    if (!e)
        return;
    if (e->kind == N_NAME) {
        if (!(locals && names_has(locals, e->text)))
            names_add(&r->names, e->text);
        return;
    }
    if (e->kind == N_INST) { /* this.field */
        names_add(&r->fields, e->text);
        return;
    }
    if (e->kind == N_MEMBER && e->args.count > 0 && e->args.items[0]) {
        names_add(&r->fields, e->text);
        scan(L, e->args.items[0], r, locals);
        return;
    }
    int from = 0;
    if (e->kind == N_CALL && e->args.count > 0 && e->args.items[0]) {
        Node *callee = e->args.items[0];
        if (callee->kind == N_NAME && !(locals && names_has(locals, callee->text))) {
            if (is_function(L, callee->text))
                calls_named(L, callee->text, false, r);
            else
                names_add(&r->names, callee->text); /* a function-valued variable */
        } else if (callee->kind == N_MEMBER && callee->args.count > 0 && callee->args.items[0]) {
            calls_named(L, callee->text, true, r);
            scan(L, callee->args.items[0], r, locals);
        } else if (callee->kind == N_INST) {
            calls_named(L, callee->text, true, r);
        } else
            scan(L, callee, r, locals);
        from = 1;
    }
    for (int i = from; i < e->args.count; i++)
        scan(L, e->args.items[i], r, locals);
    if (e->fn)
        scan_list(L, &e->fn->body, r, locals);
}
static void scan_list(Lowerer *L, NodeList *l, Reads *r, const Names *locals) {
    for (int i = 0; i < l->count; i++) {
        Node *s = l->items[i];
        scan(L, s->expr, r, locals);
        if (s->target) {
            /* an assignment writes its target: only the object and index parts are read */
            if (s->kind == S_ASSIGN && s->target->kind != N_NAME) {
                for (int k = 0; k < s->target->args.count; k++)
                    scan(L, s->target->args.items[k], r, locals);
            } else if (s->kind != S_ASSIGN)
                scan(L, s->target, r, locals);
        }
        for (int c = 0; s->guards && c < s->ncatches; c++)
            scan(L, s->guards[c], r, locals);
        scan_list(L, &s->body, r, locals);
        scan_list(L, &s->other, r, locals);
        scan_list(L, &s->finalizer, r, locals);
        for (int c = 0; c < s->ncatches; c++)
            scan_list(L, &s->catches[c], r, locals);
    }
}
static void declared_list(NodeList *l, Names *out);
static void declared_expr(Node *e, Names *out) {
    if (!e)
        return;
    for (int i = 0; i < e->args.count; i++)
        declared_expr(e->args.items[i], out);
    if (e->fn) {
        for (int i = 0; i < e->fn->nparams; i++)
            names_add(out, e->fn->params[i].name);
        declared_list(&e->fn->body, out);
    }
}
static void pattern_names(Node *p, Names *out) {
    if (!p)
        return;
    if (p->kind == N_NAME)
        names_add(out, p->text);
    for (int i = 0; i < p->args.count; i++)
        pattern_names(p->args.items[i], out);
}
static void declared_list(NodeList *l, Names *out) {
    for (int i = 0; i < l->count; i++) {
        Node *s = l->items[i];
        if (s->kind == S_DECLARE || s->kind == S_FOR || s->kind == S_DESTRUCTURE)
            names_add(out, s->name);
        if (s->kind == S_FOR)
            names_add(out, s->op);
        names_add(out, s->alias);
        for (int c = 0; s->catch_names && c < s->ncatches; c++)
            names_add(out, s->catch_names[c]);
        for (int c = 0; s->patterns && c < s->ncatches; c++)
            for (int k = 0; k < s->patterns[c].count; k++)
                pattern_names(s->patterns[c].items[k], out);
        declared_expr(s->expr, out);
        declared_expr(s->target, out);
        declared_list(&s->body, out);
        declared_list(&s->other, out);
        declared_list(&s->finalizer, out);
        for (int c = 0; c < s->ncatches; c++)
            declared_list(&s->catches[c], out);
    }
}
static Reads *own(Lowerer *L, FuncDecl *f) {
    for (int i = 0; i < L->ndirect; i++)
        if (L->direct_fn[i] == f)
            return &L->direct[i];
    Names locals = {0};
    for (int i = 0; i < f->nparams; i++)
        names_add(&locals, f->params[i].name);
    declared_list(&f->body, &locals);
    Reads r = {0};
    scan_list(L, &f->body, &r, &locals);
    free(locals.items);
    if (L->ndirect == L->capdirect) {
        L->capdirect = GROW_CAP(L->capdirect);
        L->direct_fn = realloc(L->direct_fn, sizeof(FuncDecl *) * (size_t)L->capdirect);
        L->direct = realloc(L->direct, sizeof(Reads) * (size_t)L->capdirect);
    }
    L->direct_fn[L->ndirect] = f;
    L->direct[L->ndirect] = r;
    return &L->direct[L->ndirect++];
}
/* Everything the called functions read, following their calls in turn. */
static Reads called(Lowerer *L, Reads *start) {
    Reads out = {0};
    Reads work = {0}; /* calls: the functions seen, in order; walked as a queue */
    for (int i = 0; i < start->ncalls; i++)
        reads_call(&work, start->calls[i]);
    for (int i = 0; i < work.ncalls; i++) {
        Reads *r = own(L, work.calls[i]);
        for (int k = 0; k < r->names.count; k++)
            names_add(&out.names, r->names.items[k]);
        for (int k = 0; k < r->fields.count; k++)
            names_add(&out.fields, r->fields.items[k]);
        for (int k = 0; k < r->ncalls; k++)
            reads_call(&work, r->calls[k]);
    }
    reads_free(&work);
    return out;
}

static void watch_reads(Lowerer *L, Node *e) {
    if (!e)
        return;
    Reads r = {0};
    scan(L, e, &r, NULL);
    for (int i = 0; i < r.names.count; i++)
        names_add(&L->watched, r.names.items[i]);
    for (int i = 0; i < r.fields.count; i++)
        names_add(&L->fields, r.fields.items[i]);
    Reads deep = called(L, &r);
    for (int i = 0; i < deep.names.count; i++)
        names_add(&L->watched_top, deep.names.items[i]);
    for (int i = 0; i < deep.fields.count; i++)
        names_add(&L->fields, deep.fields.items[i]);
    reads_free(&deep);
    reads_free(&r);
}
static void watch_list(Lowerer *L, NodeList *l);
static void watch_lambdas(Lowerer *L, Node *e) {
    if (!e)
        return;
    for (int i = 0; i < e->args.count; i++)
        watch_lambdas(L, e->args.items[i]);
    if (e->fn)
        watch_list(L, &e->fn->body);
}
static void watch_list(Lowerer *L, NodeList *l) {
    for (int i = 0; i < l->count; i++) {
        Node *s = l->items[i];
        if (s->kind == S_WHEN || (s->kind == S_DECLARE && s->op && !strcmp(s->op, "computed")))
            watch_reads(L, s->expr);
        watch_lambdas(L, s->expr);
        watch_lambdas(L, s->target);
        watch_list(L, &s->body);
        watch_list(L, &s->other);
        watch_list(L, &s->finalizer);
        for (int c = 0; c < s->ncatches; c++)
            watch_list(L, &s->catches[c]);
    }
}
/* A plain declaration that a `when` or `computed` reads becomes reactive; so does a top-level one
 * that a function called by a condition reads. */
static bool promoted(Lowerer *L, Node *s, bool top) {
    if (s->op && !strcmp(s->op, "const"))
        return false;
    if (names_has(&L->watched, s->name) || names_has(&L->watched, s->alias))
        return true;
    return top && (names_has(&L->watched_top, s->name) || names_has(&L->watched_top, s->alias));
}

static void bind(Lowerer *L, const char *name, int kind, const char *target) {
    if (!name)
        return;
    if (L->count == L->cap) {
        L->cap = GROW_CAP(L->cap);
        L->items = realloc(L->items, sizeof(Binding) * (size_t)L->cap);
    }
    L->items[L->count++] = (Binding){name, target, kind, L->depth};
}
static Binding *find(Lowerer *L, const char *name) {
    for (int i = L->count - 1; i >= 0; i--)
        if (!strcmp(L->items[i].name, name))
            return &L->items[i];
    return NULL;
}
static Binding *find_top(Lowerer *L, const char *name) {
    for (int i = L->top_count - 1; i >= 0; i--)
        if (!strcmp(L->items[i].name, name))
            return &L->items[i];
    return NULL;
}
static void push_scope(Lowerer *L) { L->depth++; }
static void pop_scope(Lowerer *L) {
    while (L->count && L->items[L->count - 1].depth == L->depth)
        L->count--;
    L->depth--;
}
static char *fmt(Lowerer *L, const char *f, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(tmp, sizeof tmp, f, ap);
    va_end(ap);
    return arena_strndup(L->P->arena, tmp, strlen(tmp));
}
static NodeList gen(Lowerer *L, const char *text, Node *at) {
    return generated_body(L->P, text, at->line, at->col);
}
/* Runs every watcher in a list; a copy, so watchers may unregister while it runs. */
static Node *notify(Lowerer *L, const char *list, Node *at) {
    char *text = fmt(L, "if %s.length > 0 do\n    for __fwr_f in %s.copy() do\n        __fwr_f()\n    end\nend\n",
                     list, list);
    return gen(L, text, at).items[0];
}
static bool watchable(Binding *b) { return b && (b->kind == B_REACTIVE || b->kind == B_COMPUTED); }
static void add_dep(const char **out, int *n, int cap, const char *list) {
    for (int i = 0; i < *n; i++)
        if (!strcmp(out[i], list))
            return;
    if (*n < cap)
        out[(*n)++] = list;
}
/* The watcher lists an expression depends on, in order of first use: those of the reactive and
 * computed variables it reads, of the fields it reads, and of what the functions it calls read
 * (top-level variables and fields). */
static void dependencies(Lowerer *L, Node *e, const char **out, int *n, int cap) {
    if (!e)
        return;
    Reads r = {0};
    scan(L, e, &r, NULL);
    for (int i = 0; i < r.names.count; i++)
        if (watchable(find(L, r.names.items[i])))
            add_dep(out, n, cap, fmt(L, "__fwr_w_%s", r.names.items[i]));
    for (int i = 0; i < r.fields.count; i++)
        if (names_has(&L->fields, r.fields.items[i]))
            add_dep(out, n, cap, fmt(L, "__fwr_field_%s", r.fields.items[i]));
    Reads deep = called(L, &r);
    for (int i = 0; i < deep.names.count; i++)
        if (watchable(find_top(L, deep.names.items[i])))
            add_dep(out, n, cap, fmt(L, "__fwr_w_%s", deep.names.items[i]));
    for (int i = 0; i < deep.fields.count; i++)
        if (names_has(&L->fields, deep.fields.items[i]))
            add_dep(out, n, cap, fmt(L, "__fwr_field_%s", deep.fields.items[i]));
    reads_free(&deep);
    reads_free(&r);
}
static int require_dependencies(Lowerer *L, Node *e, const char *what, Node *at, const char **out, int cap) {
    int n = 0;
    dependencies(L, e, out, &n, cap);
    if (!n)
        parser_fail(L->P, at->line, at->col,
                    "%s must read a variable or a field, such as `alive` in `when alive do`; parameters "
                    "and loop variables are not watched", what);
    return n;
}
/* After a deferred `when`, every later statement of the block (and of the blocks inside it, but
 * not of functions or lambdas) is followed by a check of the pending flag. */
static void settle_after(Lowerer *L, NodeList *list, int from, int n, Node *at) {
    for (int i = from; i < list->count; i++) {
        Node *s = list->items[i];
        if (s->kind == S_BREAK || s->kind == S_CONTINUE || s->kind == S_RETURN || s->kind == S_THROW)
            continue;
        if (s->kind != S_WHEN && s->kind != S_DEFER) {
            settle_after(L, &s->body, 0, n, at);
            settle_after(L, &s->other, 0, n, at);
            settle_after(L, &s->finalizer, 0, n, at);
            for (int c = 0; c < s->ncatches; c++)
                settle_after(L, &s->catches[c], 0, n, at);
        }
        char *text = fmt(L, "if __fwr_pending%d do\n    __fwr_pending%d = false\n    __fwr_check%d()\nend\n", n, n, n);
        nl_insert(L->P->arena, list, i + 1, gen(L, text, at).items[0]);
        i++;
    }
}
static void block(Lowerer *L, NodeList *list, bool top);
static void function(Lowerer *L, FuncDecl *f);
static void expression(Lowerer *L, Node *e) {
    if (!e)
        return;
    if (e->kind == N_NAME) {
        Binding *b = find(L, e->text);
        if (b && b->kind == B_ALIAS) {
            /* the alias reads as `not original` */
            Node *original = arena_alloc(L->P->arena, sizeof(Node));
            *original = *e;
            original->text = b->target;
            original->grouped = true;
            memset(&e->args, 0, sizeof e->args);
            e->kind = N_UNARY;
            e->text = "not";
            e->grouped = true;
            nl_push(L->P->arena, &e->args, original);
            return;
        }
    }
    for (int i = 0; i < e->args.count; i++)
        expression(L, e->args.items[i]);
    if (e->fn)
        function(L, e->fn);
}
static void function(Lowerer *L, FuncDecl *f) {
    push_scope(L);
    for (int i = 0; i < f->nparams; i++) {
        expression(L, f->params[i].value);
        bind(L, f->params[i].name, B_PLAIN, NULL);
    }
    block(L, &f->body, false);
    pop_scope(L);
}
/* After a `when` or `computed` inside a block, the rest of the block runs in a try whose
 * finally removes the watchers again. */
static void unregister_at_end(Lowerer *L, NodeList *list, int after, const char *closure, const char **deps,
                              int ndeps, Node *at) {
    Buf text = {0};
    for (int i = 0; i < ndeps; i++)
        buf_printf(&text, "%s.remove(%s)\n", deps[i], closure);
    Node *t = arena_alloc(L->P->arena, sizeof(Node));
    t->kind = S_TRY;
    t->line = at->line;
    t->col = at->col;
    t->len = -1;
    t->finalizer = gen(L, text.data, at);
    free(text.data);
    for (int j = after; j < list->count; j++)
        nl_push(L->P->arena, &t->body, list->items[j]);
    list->count = after;
    nl_push(L->P->arena, list, t);
}
static void insert_all(Lowerer *L, NodeList *list, int at, NodeList *items) {
    for (int i = 0; i < items->count; i++)
        nl_insert(L->P->arena, list, at + i, items->items[i]);
}
#define MAX_DEPS 64
static void block(Lowerer *L, NodeList *list, bool top) {
    push_scope(L);
    for (int i = 0; i < list->count; i++) {
        Node *s = list->items[i];
        switch (s->kind) {
        case S_DECLARE: {
            expression(L, s->expr);
            if (s->op && !strcmp(s->op, "computed")) {
                /* computed T y = expr: a compute closure; y is recomputed (and its watchers run)
                 * whenever a variable expr reads changes. Assignments to y are rejected. */
                const char *deps[MAX_DEPS];
                int ndeps = require_dependencies(L, s->expr, "a computed value", s, deps, MAX_DEPS);
                int n = ++L->counter;
                Buf text = {0};
                buf_printf(&text, "var __fwr_compute%d = function() => __fwr_value\n", n);
                buf_printf(&text, "__fwr_compute%d()\n", n);
                buf_printf(&text, "array<function() returns void> __fwr_w_%s = []\n", s->name);
                buf_printf(&text, "var __fwr_update%d = function() returns void\n    %s = __fwr_compute%d()\nend\n",
                           n, s->name, n);
                for (int d = 0; d < ndeps; d++)
                    buf_printf(&text, "%s.push(__fwr_update%d)\n", deps[d], n);
                NodeList extra = gen(L, text.data, s);
                free(text.data);
                extra.items[0]->expr->fn->body.items[0]->expr = s->expr;
                s->expr = extra.items[1]->target; /* y = __fwr_computeN() */
                nl_push(L->P->arena, &extra.items[3]->expr->fn->body, notify(L, fmt(L, "__fwr_w_%s", s->name), s));
                s->op = "var";
                bind(L, s->name, B_COMPUTED, NULL);
                if (s->alias)
                    bind(L, s->alias, B_ALIAS, s->name);
                s->alias = NULL;
                nl_insert(L->P->arena, list, i, extra.items[0]);
                NodeList rest = {0};
                for (int k = 2; k < extra.count; k++)
                    nl_push(L->P->arena, &rest, extra.items[k]);
                insert_all(L, list, i + 2, &rest);
                int after = i + 2 + rest.count;
                if (!top)
                    unregister_at_end(L, list, after, fmt(L, "__fwr_update%d", n), deps, ndeps, s);
                i = after - 1;
                continue;
            }
            bool reactive = (s->op && !strcmp(s->op, "reactive")) || promoted(L, s, top);
            if (s->op && !strcmp(s->op, "reactive"))
                s->op = "var";
            bind(L, s->name, reactive ? B_REACTIVE : B_PLAIN, NULL);
            if (s->alias)
                bind(L, s->alias, B_ALIAS, s->name);
            s->alias = NULL;
            if (reactive) {
                NodeList extra = gen(L, fmt(L, "array<function() returns void> __fwr_w_%s = []\n", s->name), s);
                nl_insert(L->P->arena, list, i + 1, extra.items[0]);
                i++;
            }
            continue;
        }
        case S_ASSIGN: {
            expression(L, s->expr);
            if (s->target->kind != N_NAME) {
                expression(L, s->target);
                /* `x.health = 1` notifies the conditions that read a `.health`; `xs[0] = 1` and
                 * `p.x = 1` notify the watchers of a reactive `xs` or `p`. */
                int at = i + 1;
                if ((s->target->kind == N_MEMBER || s->target->kind == N_INST) &&
                    names_has(&L->fields, s->target->text))
                    nl_insert(L->P->arena, list, at++, notify(L, fmt(L, "__fwr_field_%s", s->target->text), s));
                Node *root = s->target;
                while ((root->kind == N_MEMBER || root->kind == N_INDEX) && root->args.count > 0 && root->args.items[0])
                    root = root->args.items[0];
                if (root->kind == N_NAME) {
                    Binding *b = find(L, root->text);
                    if (b && b->kind == B_COMPUTED)
                        parser_fail(L->P, s->line, s->col,
                                    "cannot assign into computed value '%s'; it follows its expression", root->text);
                    if (b && b->kind == B_REACTIVE)
                        nl_insert(L->P->arena, list, at++, notify(L, fmt(L, "__fwr_w_%s", root->text), s));
                }
                i = at - 1;
                continue;
            }
            Binding *b = find(L, s->target->text);
            if (b && b->kind == B_ALIAS) {
                if (strcmp(s->op, "="))
                    parser_fail(L->P, s->line, s->col, "'%s' is a negated alias of '%s'; assign it with =",
                                s->target->text, b->target);
                Node *neg = arena_alloc(L->P->arena, sizeof(Node));
                neg->kind = N_UNARY;
                neg->text = "not";
                neg->line = s->expr->line;
                neg->col = s->expr->col;
                neg->len = -1;
                s->expr->grouped = true;
                nl_push(L->P->arena, &neg->args, s->expr);
                s->expr = neg;
                s->target->text = b->target;
                b = find(L, b->target);
            }
            if (b && b->kind == B_COMPUTED)
                parser_fail(L->P, s->line, s->col, "cannot assign to computed value '%s'; it follows its expression",
                            s->target->text);
            if (b && b->kind == B_REACTIVE) {
                nl_insert(L->P->arena, list, i + 1, notify(L, fmt(L, "__fwr_w_%s", s->target->text), s));
                i++;
            }
            continue;
        }
        case S_WHEN: {
            /* when cond do body else do other end: a check closure that runs body on each false ->
             * true change of cond and other on each true -> false change, registered with every
             * watcher list cond depends on, and run once now. A deferred when registers a closure
             * that only marks it pending; the check runs between the statements that follow. */
            expression(L, s->expr);
            const char *deps[MAX_DEPS];
            int ndeps = require_dependencies(L, s->expr, "a 'when' condition", s, deps, MAX_DEPS);
            bool deferred = s->op && !strcmp(s->op, "defer");
            push_scope(L);
            block(L, &s->body, false);
            pop_scope(L);
            push_scope(L);
            block(L, &s->other, false);
            pop_scope(L);
            int n = ++L->counter;
            Buf text = {0};
            buf_printf(&text,
                       "var __fwr_armed%d = true\n"
                       "var __fwr_check%d = function() returns void\n"
                       "    if __fwr_condition do\n"
                       "        if __fwr_armed%d do\n"
                       "            __fwr_armed%d = false\n"
                       "        end\n"
                       "    else do\n"
                       "        if not __fwr_armed%d do\n"
                       "            __fwr_armed%d = true\n"
                       "        end\n"
                       "    end\n"
                       "end\n",
                       n, n, n, n, n, n);
            if (deferred)
                buf_printf(&text,
                           "var __fwr_pending%d = false\n"
                           "var __fwr_mark%d = function() returns void\n    __fwr_pending%d = true\nend\n",
                           n, n, n);
            const char *watcher = fmt(L, deferred ? "__fwr_mark%d" : "__fwr_check%d", n);
            for (int d = 0; d < ndeps; d++)
                buf_printf(&text, "%s.push(%s)\n", deps[d], watcher);
            buf_printf(&text, "__fwr_check%d()\n", n);
            NodeList extra = gen(L, text.data, s);
            free(text.data);
            Node *test = extra.items[1]->expr->fn->body.items[0];
            test->expr = s->expr;
            Node *armed = test->body.items[0];
            for (int k = 0; k < s->body.count; k++)
                nl_push(L->P->arena, &armed->body, s->body.items[k]);
            Node *disarmed = test->other.items[0];
            for (int k = 0; k < s->other.count; k++)
                nl_push(L->P->arena, &disarmed->body, s->other.items[k]);
            memmove(&list->items[i], &list->items[i + 1], sizeof(Node *) * (size_t)(list->count - i - 1));
            list->count--;
            insert_all(L, list, i, &extra);
            int after = i + extra.count;
            bind(L, fmt(L, "__fwr_armed%d", n), B_PLAIN, NULL);
            bind(L, fmt(L, "__fwr_check%d", n), B_PLAIN, NULL);
            if (deferred) {
                bind(L, fmt(L, "__fwr_pending%d", n), B_PLAIN, NULL);
                bind(L, fmt(L, "__fwr_mark%d", n), B_PLAIN, NULL);
                settle_after(L, list, after, n, s);
            }
            if (!top)
                unregister_at_end(L, list, after, watcher, deps, ndeps, s);
            i = after - 1;
            continue;
        }
        case S_FOR:
            expression(L, s->expr);
            push_scope(L);
            bind(L, s->name, B_PLAIN, NULL);
            bind(L, s->op, B_PLAIN, NULL);
            block(L, &s->body, false);
            pop_scope(L);
            continue;
        case S_TRY:
            block(L, &s->body, false);
            for (int c = 0; c < s->ncatches; c++) {
                push_scope(L);
                if (s->catch_names)
                    bind(L, s->catch_names[c], B_PLAIN, NULL);
                block(L, &s->catches[c], false);
                pop_scope(L);
            }
            block(L, &s->finalizer, false);
            continue;
        case S_MATCH:
            expression(L, s->expr);
            for (int c = 0; c < s->ncatches; c++) {
                push_scope(L);
                if (s->catch_names && s->catch_names[c])
                    bind(L, s->catch_names[c], B_PLAIN, NULL);
                /* bare names in patterns bind values; patterns themselves are left alone */
                if (s->patterns) {
                    Node *stack[256];
                    int top_ = 0;
                    for (int k = 0; k < s->patterns[c].count && top_ < 256; k++)
                        stack[top_++] = s->patterns[c].items[k];
                    while (top_) {
                        Node *pn = stack[--top_];
                        if (pn->kind == N_NAME)
                            bind(L, pn->text, B_PLAIN, NULL);
                        for (int k = 0; k < pn->args.count && top_ < 256; k++)
                            if (pn->args.items[k])
                                stack[top_++] = pn->args.items[k];
                    }
                }
                if (s->guards)
                    expression(L, s->guards[c]);
                block(L, &s->catches[c], false);
                pop_scope(L);
            }
            block(L, &s->other, false);
            continue;
        default:
            expression(L, s->expr);
            expression(L, s->target);
            block(L, &s->body, false);
            block(L, &s->other, false);
            block(L, &s->finalizer, false);
            continue;
        }
    }
    pop_scope(L);
}

static bool uses_list(NodeList *l);
static bool uses_expr(Node *e) {
    if (!e)
        return false;
    for (int i = 0; i < e->args.count; i++)
        if (uses_expr(e->args.items[i]))
            return true;
    return e->fn && uses_list(&e->fn->body);
}
static bool uses_list(NodeList *l) {
    for (int i = 0; i < l->count; i++) {
        Node *s = l->items[i];
        if (s->kind == S_WHEN || (s->kind == S_DECLARE && (s->alias || (s->op && (!strcmp(s->op, "reactive") ||
                                                                                   !strcmp(s->op, "computed"))))))
            return true;
        if (uses_expr(s->expr) || uses_expr(s->target) || uses_list(&s->body) || uses_list(&s->other) ||
            uses_list(&s->finalizer))
            return true;
        for (int c = 0; c < s->ncatches; c++)
            if (uses_list(&s->catches[c]))
                return true;
    }
    return false;
}

void lower_reactive(Parser *P, Program *p) {
    bool any = uses_list(&p->body);
    for (int i = 0; i < p->nfunctions && !any; i++)
        any = uses_list(&p->functions[i].body);
    for (int c = 0; c < p->nclasses && !any; c++)
        for (int m = 0; m < p->classes[c].nmethods && !any; m++)
            any = uses_list(&p->classes[c].methods[m].body);
    if (!any)
        return;
    Lowerer L = {0};
    L.P = P;
    L.prog = p;
    watch_list(&L, &p->body);
    for (int i = 0; i < p->nfunctions; i++)
        watch_list(&L, &p->functions[i].body);
    for (int c = 0; c < p->nclasses; c++)
        for (int m = 0; m < p->classes[c].nmethods; m++)
            watch_list(&L, &p->classes[c].methods[m].body);
    /* Top-level declarations are visible in every function, whatever their position. */
    push_scope(&L);
    for (int i = 0; i < p->body.count; i++) {
        Node *s = p->body.items[i];
        if (s->kind != S_DECLARE)
            continue;
        int kind = s->op && !strcmp(s->op, "computed") ? B_COMPUTED
                   : (s->op && !strcmp(s->op, "reactive")) || promoted(&L, s, true) ? B_REACTIVE
                                                                               : B_PLAIN;
        bind(&L, s->name, kind, NULL);
        if (s->alias)
            bind(&L, s->alias, B_ALIAS, s->name);
    }
    L.top_count = L.count;
    /* A watcher list for each field a condition reads, before anything can assign one. */
    if (L.fields.count && p->body.count) {
        Buf text = {0};
        for (int i = 0; i < L.fields.count; i++)
            buf_printf(&text, "array<function() returns void> __fwr_field_%s = []\n", L.fields.items[i]);
        NodeList extra = gen(&L, text.data, p->body.items[0]);
        free(text.data);
        insert_all(&L, &p->body, 0, &extra);
    }
    for (int i = 0; i < p->nfunctions; i++)
        function(&L, &p->functions[i]);
    for (int c = 0; c < p->nclasses; c++)
        for (int m = 0; m < p->classes[c].nmethods; m++)
            function(&L, &p->classes[c].methods[m]);
    block(&L, &p->body, true);
    pop_scope(&L);
    free(L.items);
    free(L.watched.items);
    free(L.watched_top.items);
    free(L.fields.items);
    for (int i = 0; i < L.ndirect; i++)
        reads_free(&L.direct[i]);
    free(L.direct);
    free(L.direct_fn);
}
