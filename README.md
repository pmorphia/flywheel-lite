# Turbine

Turbine runs Flywheel programs (`.fw` / `.fly`) directly on a bytecode virtual machine. It is
not a new language: it reads the same source as the Flywheel compiler and aims to print the same
output. Where the compiler builds a native executable through LLVM, Turbine compiles a file to
bytecode in memory and runs it straight away.

Why it exists:

1. **macOS support.** Turbine is portable C11 with no platform toolchain requirements, so it runs
   on macOS as well as Linux and Windows.
2. **Fewer dependencies, faster install.** Building needs only a C compiler: no LLVM, no C++20
   toolchain, no linker at run time, nothing downloaded. A clean build takes a few seconds.
3. **Lightweight.** One ~0.5 MB executable that depends only on the C library; about 1 ms to start.

```sh
turbine hello.fw
turbine run app.fw -- one two      # program arguments after --
turbine -s 'print 6 * 7'
```

## Build

Any C11 compiler works (GCC, Clang, Apple Clang, MSVC). The build embeds library modules from
a [Flywheel](https://github.com/fwlang/flywheel) checkout, and the tests run its conformance
suite. By default that checkout is the parent directory, so clone Turbine inside it:

```sh
git clone https://github.com/fwlang/flywheel && cd flywheel
git clone https://github.com/fwlang/turbine && cd turbine
make                      # build/turbine
make test                 # needs Python 3; runs the conformance and Turbine suites
make install PREFIX=~/.local
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

The library modules written in Flywheel (`csv`, `url`, `log`, `args`, `bigint`, `decimal`) are
embedded into the executable from `$FLYWHEEL_DIR/stdlib/fw` at build time by a small helper (`tools/embed.c`),
so the installed binary is self-contained. `make debug` builds with AddressSanitizer and UBSan.

## Usage

| Command | Effect |
|---|---|
| `turbine FILE [ARGS...]` / `turbine run FILE [-- ARGS...]` | run a program; a top-level `return n` exits with status `n` |
| `turbine run -s CODE` | run code from the command line (`-` as FILE reads standard input) |
| `turbine check FILE` | parse and compile without running |
| `turbine debug FILE` | run, then print the top-level return value and the CPU time |
| `turbine export FILE OUTPUT` | run, then write the top-level return value to `OUTPUT` |
| `turbine disasm FILE` | print the compiled bytecode |
| `turbine repl` | interactive session; a trailing expression's value is printed |

Errors use the compiler's formats: `file:line:col: error: ...` for compile errors and
`Flywheel runtime error: Type: message` (plus the failing location) for uncaught errors, with exit
status 1. Set `TURBINE_TRACE=1` for a full stack trace.

## What runs

The whole core language: declarations and `const`, numeric rules (checked 64-bit overflow, `/`
and `//`, `%`), chained comparisons, `??`, ranges and slices, parentheses-free calls, string
interpolation with format specs, functions with defaults, closures with shared captures and
fresh per-iteration loop bindings, classes with generated constructors and `init`, inheritance
and `super`, interfaces, operator methods, `to_string`, `hash`/`equals` keys, the iterator
protocol, enums and enums with data, `match` with every pattern form (values, ranges, enum
members, types, `ok`/`error`, destructuring, guards), destructuring declarations, generics,
nullable values, `try`/`catch`/`finally`/`then`, named errors, `assert`, `defer`, `Result`,
arrays/tuples/maps/sets with their methods, `freeze`, casts and conversions, markup literals,
components, element families, and local, path, package (`.flywheel-modules`) and library-module
imports with aliases, wildcards and `private`.

Native modules: `math`, `fs`, `os` (including `run`, `process`, `pipeline` with captured output
and timeouts), `time`, `io`, `json` (parse, encode, typed `decode`, `Class.from_json`,
`<json>` literals), `testing`, `collections`, `encoding`, `uuid`, `gc`, and `html`/`svg`
(rendering, `html.parse`, CSS selector queries, mutation, parent and sibling links).

Not available in Turbine (importing one reports it clearly; use the Flywheel compiler for these):
`web`, `net`, `thread`, `sqlite`, `regex`, `desktop`, `crypto`, `compress`, `toml`, `yaml`,
`flywheel`, native `import c/cxx/rust/zig`, and URL imports. On Windows, `os.run`/`os.process`
capture standard output only and ignore timeouts.

## Differences from the compiler

Turbine is dynamically typed at run time. Declared types still matter where they change values
(an `int` assigned to a `double`, an array literal assigned to a tuple type, `{}` as a set, a
`json.value` decoded into a class, a `weak<T>` field) and are checked at typed declarations, but
there is no static type checker. So:

- Programs the compiler rejects can run under Turbine until they reach the problem: type
  mismatches surface as `TypeError` when executed, a non-exhaustive `match` raises `MatchError`
  only if no case applies, and missing returns yield `null`. Use `flywheel check` to validate.
- `private` and `readonly` class members are not enforced (module-level `private` and `const` are).
- `float` values are stored as `double`; `u64` values above the signed 64-bit range are not
  supported; `typeof` cannot name generic instances (`Box`, not `Box<int>`) or function types.
- Memory is managed by a tracing collector instead of reference counting. Cycles are collected
  automatically, but a `weak<T>` clears when the collector runs rather than the moment the last
  strong reference goes away. `testing.live_objects()` collects first, so it reports what
  reference counting would still hold.

`tests/known_failures.txt` lists the conformance programs affected by these differences.

## Tests

`python3 tests/run_tests.py build/turbine` runs:

- every positive, input, and runtime-trap program from the compiler's regression suite
  (`$FLYWHEEL_DIR/tests/test_compiler.py`), comparing output byte for byte (programs using unavailable modules
  are skipped and counted);
- Turbine's own cases in `tests/cases`, whose expected output was produced by the Flywheel compiler.

Current result on Linux: 174 of 183 pass; the 9 others are the known differences above.
`TURBINE_GC_STRESS=1` collects at every safepoint, which together with `make debug` checks that
the runtime roots every live value.

## Performance

A computed-goto dispatch loop (a portable `switch` loop on MSVC, or with
`-DTURBINE_SWITCH_DISPATCH`), inline caches for fields and methods, and a fast path for integer
`for` ranges. Measured on Linux x86-64: recursive `fib(32)` 0.18 s (CPython 3.14: 0.17 s), and a
3-million-iteration arithmetic loop plus string building and 200k map writes 0.16 s (CPython:
0.30 s). Natively compiled Flywheel is much faster for compute-bound code; Turbine is for
starting quickly and running anywhere.

## Layout

| File | Contents |
|---|---|
| `src/lexer.c`, `src/parser.c`, `src/front.h` | tokens and AST, ported from the compiler's front end |
| `src/compiler.c`, `src/opcodes.h` | scope resolution and bytecode generation |
| `src/vm.c` | the interpreter loop, calls, errors, classes, coercion, module loading |
| `src/value.c`, `src/memory.c` | values, maps, formatting, the garbage collector |
| `src/lib_*.c` | builtins and library modules |
| `src/main.c` | the command-line tool |
