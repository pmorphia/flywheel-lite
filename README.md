# Flywheel Lite

Flywheel Lite runs Flywheel programs (`.fw` / `.fly`) directly on a bytecode virtual machine. It is
not a new language: it reads the same source as the Flywheel compiler and aims to print the same
output. Where the compiler builds a native executable through LLVM, Flywheel Lite compiles a file to
bytecode in memory and runs it straight away.

Why it exists:

1. **macOS support.** Flywheel Lite is portable C11 with no platform toolchain requirements, so it runs
   on macOS as well as Linux and Windows.
2. **Fewer dependencies, faster install.** Building needs only a C compiler: no LLVM, no C++20
   toolchain, no linker at run time, nothing downloaded. A clean build takes a few seconds.
3. **Lightweight.** One ~0.5 MB executable that depends only on the C library; about 1 ms to start.

```sh
flywheel-lite hello.fw
flywheel-lite run app.fw -- one two      # program arguments after --
flywheel-lite -s 'print 6 * 7'
fwl hello.fw                       # fwl is a short alias for flywheel-lite
```

## Build

Any C11 compiler works (GCC, Clang, Apple Clang, MSVC). The build embeds library modules from
a [Flywheel](https://github.com/fwlang/flywheel) checkout, and the tests run its conformance
suite. By default that checkout is the parent directory, so clone Flywheel Lite inside it:

```sh
git clone https://github.com/fwlang/flywheel && cd flywheel
git clone https://github.com/fwlang/flywheel-lite && cd flywheel-lite
make                      # build/flywheel-lite
make test                 # needs Python 3; runs the conformance and Flywheel Lite suites
make install PREFIX=~/.local     # installs flywheel-lite and the fwl alias
```

With CMake (also the way to build with MSVC on Windows):

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Elsewhere, point at the Flywheel checkout with `make FLYWHEEL_DIR=/path/to/flywheel` (also passed
through to `make test`), `cmake -DFLYWHEEL_DIR=/path/to/flywheel`, or the `FLYWHEEL_DIR`
environment variable for `tests/run_tests.py`.

The library modules written in Flywheel (`csv`, `url`, `log`, `args`, `bigint`, `decimal`, `random`) are
embedded into the executable from `$FLYWHEEL_DIR/stdlib/fw` at build time by a small helper (`tools/embed.c`),
so the installed binary is self-contained. `make debug` builds with AddressSanitizer and UBSan.

## Usage

The command line follows the Flywheel compiler's:

| Command | Effect |
|---|---|
| `flywheel-lite run SOURCE [options] [-- ARGS...]` / `flywheel-lite SOURCE [ARGS...]` | run a program; a top-level `return n` exits with status `n` |
| `flywheel-lite check SOURCE` | parse and compile without running |
| `flywheel-lite export SOURCE OUTPUT` / `-o OUTPUT` | run, then write the top-level return value to `OUTPUT` |
| `flywheel-lite debug SOURCE [-- ARGS...]` | run, then print the top-level return value and the CPU time |
| `flywheel-lite test [files or directories]` | run `test_*.fw` and `*_test.fw` files like `flywheel test`, with the same `setup`/`teardown`/`setup_all`/`teardown_all` fixtures and output |
| `flywheel-lite disasm SOURCE` | print the compiled bytecode |
| `flywheel-lite repl` | interactive session; a trailing expression's value is printed |
| `flywheel-lite selftest` | run this checkout's test suite (needs Python 3) |

`fwl` is a short alias for `flywheel-lite` (`fwl hello.fw`, `fwl repl`); `make install` and
`cmake --install` create it next to the executable.

SOURCE is a `.fw`/`.fly` path, or `-` for standard input. `-s CODE`, `--source CODE`, or
`--source=CODE` in place of SOURCE runs code from the command line (diagnostics name it
`<source>`); like every option, it can appear anywhere before `--`. `-O0`..`-O3` and `-g` are
accepted and ignored, so compiler command lines work unchanged. `build`, `desktop`, `serve`,
`fmt`, `doc`, `lsp`, and `test --coverage`/`--race` need the Flywheel compiler and say so.

Errors use the compiler's formats: `file:line:col: error: ...` for compile errors and
`Flywheel runtime error: Type: message` (plus the failing location) for uncaught errors, with exit
status 1. Set `LITE_TRACE=1` for a full stack trace.

## What runs

The whole core language: declarations and `const`, numeric rules (checked 64-bit overflow, `/`
and `//`, `%`), chained comparisons, `??`, ranges and slices, parentheses-free calls, string
interpolation with format specs, functions with defaults, closures with shared captures and
fresh per-iteration loop bindings, classes with generated constructors and `init`, inheritance
and `super`, interfaces, operator methods, `to_string`, `hash`/`equals` keys, the iterator
protocol, enums and enums with data, `match` with every pattern form (values, ranges, enum
members, types, `ok`/`error`, destructuring, guards), destructuring declarations, generics,
nullable values, `try`/`catch`/`finally`/`then`, named errors, declared error types (`error Name(...)`,
`throw Name with "message"`), `assert`, `defer`, `Result`, reactive variables (`reactive`, `when`,
`computed`), negated bool aliases (`bool on/off`), units (`unitns`, `unit`, `unitconv`, quantity
literals such as `100m`, `x in cm`, `type`/`unit`/`unitns`),
arrays/tuples/maps/sets with their methods, `freeze`, casts and conversions, markup literals,
components, element families, and local, path, package (`.flywheel-modules`) and library-module
imports with aliases, wildcards and `private`.

Native modules: `math`, `fs`, `os` (including `run`, `process`, `pipeline` with captured output
and timeouts), `time`, `io`, `json` (parse, encode, typed `decode`, `Class.from_json`,
`<json>` literals), `testing`, `collections`, `encoding`, `uuid`, `gc`, and `html`/`svg`
(rendering, `html.parse`, CSS selector queries, mutation, parent and sibling links).

Not available in Flywheel Lite (importing one reports it clearly; use the Flywheel compiler for these):
`web`, `net`, `thread`, `sqlite`, `regex`, `desktop`, `crypto`, `compress`, `toml`, `yaml`,
`flywheel`, native `import c/cxx/rust/zig`, and URL imports. On Windows, `os.run`/`os.process`
capture standard output only and ignore timeouts.

## Differences from the compiler

Flywheel Lite is dynamically typed at run time. Declared types still matter where they change values
(an `int` assigned to a `double`, an array literal assigned to a tuple type, `{}` as a set, a
`json.value` decoded into a class, a `weak<T>` field) and are checked at typed declarations, but
there is no static type checker. So:

- Programs the compiler rejects can run under Flywheel Lite until they reach the problem: type
  mismatches surface as `TypeError` when executed, a non-exhaustive `match` raises `MatchError`
  only if no case applies, and missing returns yield `null`. Use `flywheel check` to validate.
- Unit mistakes the compiler rejects (`100m + 10s`, a plain number where a quantity is
  declared, a product with no `unitconv`) raise `TypeError` when executed, with the compiler's
  wording. Quantity operators dispatch at run time to the hidden unit classes.
- The compiler's warning for a `catch "Name"` that nothing throws is not reported.
- `private` and `readonly` class members are not enforced (module-level `private` and `const` are).
- `float` values are stored as `double`; `u64` values above the signed 64-bit range are not
  supported; `typeof` cannot name generic instances (`Box`, not `Box<int>`) or function types.
- Memory is managed by a tracing collector instead of reference counting. Cycles are collected
  automatically, but a `weak<T>` clears when the collector runs rather than the moment the last
  strong reference goes away. `testing.live_objects()` collects first, so it reports what
  reference counting would still hold.

`tests/known_failures.txt` lists the conformance programs affected by these differences.

## Tests

`python3 tests/run_tests.py build/flywheel-lite` runs:

- every positive, input, and runtime-trap program from the compiler's regression suite
  (`$FLYWHEEL_DIR/tests/test_compiler.py`), comparing output byte for byte (programs using unavailable modules
  are skipped and counted);
- Flywheel Lite's own cases in `tests/cases`, whose expected output was produced by the Flywheel compiler.

Current result on Linux: 197 of 206 pass; the 9 others are the known differences above.
`LITE_GC_STRESS=1` collects at every safepoint, which together with `make debug` checks that
the runtime roots every live value.

## Performance

A computed-goto dispatch loop (a portable `switch` loop on MSVC, or with
`-DLITE_SWITCH_DISPATCH`), inline caches for fields and methods, and a fast path for integer
`for` ranges. Measured on Linux x86-64: recursive `fib(32)` 0.18 s (CPython 3.14: 0.17 s), and a
3-million-iteration arithmetic loop plus string building and 200k map writes 0.16 s (CPython:
0.30 s). Natively compiled Flywheel is much faster for compute-bound code; Flywheel Lite is for
starting quickly and running anywhere.

## Layout

| File | Contents |
|---|---|
| `src/lexer.c`, `src/parser.c`, `src/front.h` | tokens and AST, ported from the compiler's front end |
| `src/lower.c` | unit namespaces and reactive variables lowered to ordinary code (as in the compiler) |
| `src/compiler.c`, `src/opcodes.h` | scope resolution and bytecode generation |
| `src/vm.c` | the interpreter loop, calls, errors, classes, coercion, module loading |
| `src/value.c`, `src/memory.c` | values, maps, formatting, the garbage collector |
| `src/lib_*.c` | builtins and library modules |
| `src/main.c` | the command-line tool |
