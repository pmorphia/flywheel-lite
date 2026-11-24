# Flywheel Lite

Flywheel Lite runs Flywheel programs (`.fw` / `.fly`) directly on a bytecode virtual machine. It is
not a new language: it reads the same source as the Flywheel compiler and aims to print the same
output. Where the compiler builds a native executable through LLVM, Flywheel Lite compiles a file to
bytecode in memory and runs it straight away.

Flywheel Lite is currently in its **beta** phase: version **2.13 (beta-13)**, following Flywheel
2.28 (`flywheel-lite --version`).

Why it exists:

1. **No C++ compiler required.** You can skip installing Visual Studio Build Tools and don't need administrator privileges.
2. **Fewer dependencies, faster install.** Building needs only a C compiler: no LLVM, no C++20
   toolchain, no linker at run time, nothing downloaded. A clean build takes a few seconds.
3. **Lightweight.** One ~0.5 MB executable that depends only on the C library; about 1 ms to start.

```sh
flywheel-lite hello.fw
flywheel-lite run app.fw -- one two      # program arguments after --
flywheel-lite -x 'print 6 * 7'
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

The library modules written in Flywheel (`csv`, `url`, `log`, `args`, `bigint`, `decimal`, `random`, `metric`, `imperial`, `cogwheel`, `vecmath`, and `_time`, the time units behind `import time`) are
embedded into the executable from `$FLYWHEEL_DIR/stdlib/fw` at build time by a small helper (`tools/embed.c`),
so the installed binary is self-contained. `make debug` builds with AddressSanitizer and UBSan.

## Usage

The command line follows the Flywheel compiler's: every command is a flag, and without a mode flag
SOURCE is run.

| Flag | Effect |
|---|---|
| `-r`, `--run` | run a program (the default); a top-level `return n` exits with status `n` |
| `-c`, `--check` | parse and compile without running |
| `-e`, `--export` | run, then write the top-level return value to `-o OUTPUT` or the next argument |
| `-D`, `--debug` | run, then print the top-level return value and the CPU time |
| `-t`, `--test` | alone, run `test_*.fw` and `*_test.fw` files like `flywheel --test`, with the same `setup`/`teardown`/`setup_all`/`teardown_all` fixtures and output; with another mode or `-x`, run the program's `test_*` functions before its first top-level `return` |
| `-n NAME`, `--new NAME` | create a project with Pulley (`--game`: a game) |
| `--disasm` | print the compiled bytecode |
| `--repl` | interactive session; a trailing expression's value is printed |
| `--selftest` | run this checkout's test suite (needs Python 3) |
| `-E SOURCE`, `--error SOURCE` | print the last error SOURCE stopped with: its message and locations, without the source lines |
| `-u`, `--update` | reinstall from flywheel.polymorphia.dev, then carry on with the rest of the command line |
| `-v`, `--version` / `-h`, `--help` | print the version / the usage |
| `-x CODE`, `--source CODE` | run CODE instead of SOURCE (diagnostics name it `<source>`) |
| `-o FILE`, `--output FILE` | output file (export) |

Short flags combine and may come before or after SOURCE (`fwl main.fw -vt`); `-u`, `-v`, and `-h` run
first, in that order. Program arguments that start with `-` go after `--`. The old command words
(`flywheel-lite run app.fw`) are reported with the flag to use instead. `fwl` is a short alias for
`flywheel-lite`; `make install` and `cmake --install` create it next to the executable.

SOURCE is a `.fw`/`.fly` path, a directory (which runs its `source.fw`), or `-` for standard
input. `-O0`..`-O3`, `-g`, and `--hot` are accepted without effect. `--build`, `--desktop`, `--serve`,
`--format`, `--doc`, `--lsp`, `--coverage`, `--race`, and `--emit-ir` need the Flywheel compiler and
say so.

Errors use the compiler's formats, with exit status 1: `file:line:col: error: ...` for compile
errors, and `Flywheel runtime error: Type: message` followed by a traceback for uncaught errors.
Each location shows its source line with the failing range marked: `^` under the operator, call,
or member that failed and `~` under the rest of its expression. Recursion is folded ("the same
call 197 more times"). Output is colored when standard error is a terminal; `NO_COLOR=1` turns
color off and `FORCE_COLOR=1` turns it on.

```
Flywheel runtime error: NumericError: integer division by zero
  at main.fw:2:14 in divide
    2 | return a // b
      |        ~~^^~~
  at main.fw:8:18 in compute
    8 | total += divide(100, x)
      |          ^^^^^^^^^^^^^^
  at main.fw:13:7 in <module>
   13 | print compute([1, 2, 0])
      |       ^^^^^^^^^^^^^^^^^^
```

The last error of each source file is kept in `$XDG_STATE_HOME/flywheel-lite/errors` (by default
`~/.local/state`; `%LOCALAPPDATA%` on Windows), so `fwl -E main.fw` prints it again later, with
the time it happened, for example after a program cleared the terminal. The record holds the
message and locations only, never the source. `-x` code and standard input are not recorded.

## What runs

The whole core language: declarations and `const`, numeric rules (checked 64-bit overflow, `/`
and `//`, `%`), chained comparisons, `otherwise` (the null fallback, formerly `??`), ranges and slices, parentheses-free calls, string
interpolation with format specs, functions with defaults, closures with shared captures and
fresh per-iteration loop bindings, classes with generated constructors and `init`, inheritance
and `super`, interfaces, operator methods, `to_string`, `hash`/`equals` keys, the iterator
protocol, enums and enums with data, `match` with every pattern form (values, ranges, enum
members, types, `ok`/`error`, destructuring, guards), destructuring declarations, generics,
nullable values, `try`/`catch`/`finally`, named errors, declared error types (`error Name(...)`,
`throw Name with "message"`), `assert`, `defer`, `Result`, reactive variables (`reactive`, `when`,
`computed`), `struct` value types, one-call `run` bodies (`if x run f()`, `else run g()`, `for i in xs run f(i)`),
negated bool aliases (`bool on/off`), units (`unitns name`, `unit`, `unitconv`, quantity
literals such as `100m`, quantity types of any numeric type such as `u8[time]` and
`float[metric.length]`, `x in cm`, `type`/`unit`/`unitns`, units shared across modules, `unitconv A = B`
between namespaces (`1m + 1ft`), `%` and `math.min`/`max`/`clamp`/`abs` on quantities, and the `metric`, `imperial`, and `time` unit libraries),
arrays/tuples/maps/sets with their methods, `freeze`, casts and conversions, markup literals,
components (also qualified by module, `<cogwheel.heading>`; including `void component`s, used like
`<br>` with no closing tag), element families, void tags and components written without a closing
tag (`<cogwheel.conf key="a" value="1">`), and local, path, package (every `.flywheel-modules` at
or above the importing file, nearest first) and library-module
imports with aliases, wildcards, `private`, and `public import name.*`.

Native modules: `math`, `fs`, `os` (including `run`, `process`, `pipeline` with captured output
and timeouts, and background processes with `start`, `alive`, `wait`, and `kill`), `time`, `io`, `json` (parse, encode, typed `decode`, `Class.from_json`,
`<json>` literals), `testing`, `collections`, `encoding`, `uuid`, `toml`, `gc`, `hot` (`hot.reload()` and
`hot.enabled()` are always `false`: hot reload is the compiler's `--hot`), and `html`/`svg`
(rendering, `html.parse`, CSS selector queries, mutation, parent and sibling links).
`deep_copy(value)` copies a whole object graph as in the compiler, and `random.Generator(seed)`
produces the same numbers as the compiler's for the same seed.
The built-in names (`__file__`, `__dir__`, `__line__`, `__args__`, `__version__`, `__flywheel__`,
`__impl__`, `__os__`, `__arch__`) work as in the compiler; here `__impl__` is `"lite"`, `__version__`
is Flywheel Lite's own version, and `__flywheel__` the Flywheel version it follows.

Not available in Flywheel Lite (importing one reports it clearly; use the Flywheel compiler for these):
`web`, `net`, `thread`, `sqlite`, `regex`, `desktop`, `crypto`, `compress`, `yaml`,
`flywheel`, native `import rust/zig`, and URL imports. On Windows, `os.run`/`os.process`
capture standard output only and ignore timeouts, and `os.start` and native imports are unavailable.

## Native C and C++ imports

`import c name` and `import cxx name` (`import++ name`) work on Linux and macOS, so packages with a C
backend, such as Aperture, run. The first run builds the source with the system compiler (`cc`/`c++`,
or `$CC`/`$CXX`) into `$XDG_CACHE_HOME/flywheel-lite/native` (by default `~/.cache`); it is rebuilt when
the source, or a file it includes with quotes, changes. Programs without native imports still need no
compiler. Numbers, `bool`, strings, pointers (as `int` handles), and arrays for pointer parameters
cross as with the compiler, and the `// flywheel:` directives are followed. `--check` only reads the
source. Differences:

- Statically linked builds (the prebuilt Linux executables) cannot load native code; build from source.
- Functions that take or return C structs by value or take function pointers are not exposed.
- Arrays are copied into a C block for the call (and back, unless the pointer is to `const`). An
  array passed for a `void *` is laid out by its values: `double` for numbers with fractions, 64-bit
  integers for `int`, and structs by their field types.

## Differences from the compiler

Flywheel Lite is dynamically typed at run time. Declared types still matter where they change values
(an `int` assigned to a `double`, an array literal assigned to a tuple type, `{}` as a set, a
`json.value` decoded into a class, a `weak<T>` field) and are checked at typed declarations, but
there is no static type checker. So:

- Programs the compiler rejects can run under Flywheel Lite until they reach the problem: type
  mismatches surface as `TypeError` when executed, a non-exhaustive `match` raises `MatchError`
  only if no case applies, and missing returns yield `null`. Use `flywheel --check` to validate.
- Unit mistakes the compiler rejects (`100m + 10s`, a plain number where a quantity is
  declared, a product with no `unitconv`) raise `TypeError` when executed, with the compiler's
  wording. Quantity operators dispatch at run time to the hidden unit classes.
- The compiler's warning for a `catch "Name"` that nothing throws is not reported.
- `private`, `protected`, and private or protected `static` members are enforced when the access
  runs (a `TypeError`), not at compile time; `readonly` fields are not enforced (module-level
  `private` and `const` are).
- `float` values are stored as `double`; `u64` values above the signed 64-bit range are not
  supported; `typeof` cannot name generic instances (`Box`, not `Box<int>`) or function types.
- Memory is managed by a tracing collector instead of reference counting. Cycles are collected
  automatically, but a `weak<T>` clears when the collector runs rather than the moment the last
  strong reference goes away. `testing.live_objects()` collects first, so it reports what
  reference counting would still hold. Likewise a `destructor` runs after the collector finds the
  instance unreachable (at the next safepoint), or when the program ends, not the moment the last
  reference drops.
- There are no threads: `spawn call` runs the call at once and its `thread.task` holds the result
  (or error) for `join()`.

`tests/known_failures.txt` lists the conformance programs affected by these differences.

## Tests

`python3 tests/run_tests.py build/flywheel-lite` runs:

- every positive, input, and runtime-trap program from the compiler's regression suite
  (`$FLYWHEEL_DIR/tests/test_compiler.py`), comparing output byte for byte (programs using unavailable modules
  are skipped and counted);
- Flywheel Lite's own cases in `tests/cases`, whose expected output was produced by the Flywheel compiler.

Every failure on Linux is one of the known differences above.
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
| `src/diag.c` | tracebacks, compile error excerpts, color, the last-error record (`--error`) |
| `src/main.c` | the command-line tool |
