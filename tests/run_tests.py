"""Flywheel Lite test runner.

1. Conformance: runs the positive, input, and trap programs from the Flywheel compiler's
   regression suite ($FLYWHEEL_DIR/tests/test_compiler.py, default ../) and compares output byte for byte.
   Programs that import modules Flywheel Lite does not provide are reported as skipped.
2. Flywheel Lite cases: tests/cases/NAME.fw with NAME.out (expected stdout) and optionally
   NAME.err (a substring of stderr) and NAME.code (expected exit status).
3. Command line: --source, options, export -o, and `flywheel-lite test`.

usage: python3 tests/run_tests.py path/to/flywheel-lite [-v] [-k substring] [--only cases|conformance|cli]
"""

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = Path(os.environ.get("FLYWHEEL_DIR") or HERE.parent.parent).resolve()
UNSUPPORTED = {"web", "net", "thread", "sqlite", "regex", "desktop", "crypto", "compress",
               "toml", "yaml", "flywheel"}


def load_suite():
    sys.path.insert(0, str(REPO / "tests"))
    saved = sys.argv
    sys.argv = [saved[0]]
    try:
        import test_compiler  # noqa: E402
    finally:
        sys.argv = saved
    return test_compiler


def unsupported(source):
    if re.search(r"^\s*import\s+(c|cxx|rust|zig)\s+\w", source, re.M) or "import++" in source:
        return "native import"
    for m in re.finditer(r"^\s*import\s+\"?([\w.]+)", source, re.M):
        name = m.group(1).split(".")[0]
        if name in UNSUPPORTED:
            return name
    return None


def platform_skip(source):
    if os.name == "nt" and re.search(r"os\.(run|process|pipeline|shell|command)\(", source):
        return "POSIX shell commands"
    return None


def load_known():
    known = {}
    path = HERE / "known_failures.txt"
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.split("#", 1)
            name = line[0].strip()
            if name:
                known[name] = line[1].strip() if len(line) > 1 else ""
    return known


def run(lite, source, cwd, stdin=None, args=()):
    path = Path(cwd) / "program.fw"
    path.write_text(source, encoding="utf-8")
    try:
        return subprocess.run([lite, str(path), *args], cwd=cwd, input=stdin, capture_output=True,
                              text=True, encoding="utf-8", errors="replace", timeout=20)
    except subprocess.TimeoutExpired:
        return None


def main():
    argv = sys.argv[1:]
    if not argv:
        print(__doc__)
        return 2
    lite = str(Path(argv[0]).resolve())
    verbose = "-v" in argv
    pattern = argv[argv.index("-k") + 1] if "-k" in argv else None
    only = argv[argv.index("--only") + 1] if "--only" in argv else None
    passed = failed = skipped = 0
    failures = []
    skips = {}

    def check(name, ok, detail):
        nonlocal passed, failed
        if ok:
            passed += 1
        else:
            failed += 1
            failures.append((name, detail))

    if only in (None, "conformance"):
        suite = load_suite()
        cases = [(f"positive/{n}", s, None, e, 0) for n, (s, e) in suite.POSITIVE.items()]
        cases += [(f"input/{n}", s, i, e, 0) for n, (s, i, e) in suite.INPUT_CASES.items()]
        for name, source, stdin, expected, _ in cases:
            if pattern and pattern not in name:
                continue
            why = unsupported(source) or platform_skip(source)
            if why:
                skipped += 1
                skips[why] = skips.get(why, 0) + 1
                continue
            with tempfile.TemporaryDirectory() as tmp:
                r = run(lite, source, tmp, stdin)
            if r is None:
                check(name, False, "timed out")
                continue
            ok = r.returncode == 0 and r.stdout == expected
            check(name, ok, f"exit {r.returncode}\n--- expected\n{expected}--- got\n{r.stdout}--- stderr\n{r.stderr}")
        traps = getattr(suite, "TRAPS", {})
        for n, value in traps.items():
            name = f"trap/{n}"
            if pattern and pattern not in name:
                continue
            source, needle = (value[0], value[1]) if isinstance(value, tuple) else (value, "")
            why = unsupported(source)
            if why:
                skipped += 1
                skips[why] = skips.get(why, 0) + 1
                continue
            with tempfile.TemporaryDirectory() as tmp:
                r = run(lite, source, tmp)
            if r is None:
                check(name, False, "timed out")
                continue
            ok = r.returncode != 0 and (not isinstance(needle, str) or needle in r.stderr)
            check(name, ok, f"exit {r.returncode}\n--- wanted stderr containing\n{needle}\n--- stderr\n{r.stderr}")

    if only in (None, "cases"):
        for src in sorted((HERE / "cases").glob("*.fw")):
            name = f"case/{src.stem}"
            if pattern and pattern not in name:
                continue
            expected = src.with_suffix(".out").read_text(encoding="utf-8") if src.with_suffix(".out").exists() else ""
            err = src.with_suffix(".err").read_text(encoding="utf-8").strip() if src.with_suffix(".err").exists() else None
            code = int(src.with_suffix(".code").read_text()) if src.with_suffix(".code").exists() else (1 if err else 0)
            stdin = src.with_suffix(".in").read_text(encoding="utf-8") if src.with_suffix(".in").exists() else None
            r = subprocess.run([lite, str(src)], cwd=src.parent, input=stdin, capture_output=True, text=True,
                               encoding="utf-8", errors="replace", timeout=60)
            ok = r.returncode == code and r.stdout == expected and (err is None or err in r.stderr)
            check(name, ok, f"exit {r.returncode} (want {code})\n--- expected\n{expected}--- got\n{r.stdout}--- stderr\n{r.stderr}")

    if only in (None, "cli"):
        # Command-line behaviour shared with the Flywheel compiler: --source anywhere, -o, test.
        def cli(*args, cwd=None):
            return subprocess.run([lite, *args], cwd=cwd, capture_output=True, text=True,
                                  encoding="utf-8", errors="replace", timeout=60)

        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            argv_code = "import os\nprint os.args()"
            out = tmp / "out.txt"
            suite_dir = tmp / "suite"
            (suite_dir / "nested").mkdir(parents=True)
            (suite_dir / "build").mkdir()
            (suite_dir / "test_ok.fw").write_text(
                "import testing\nfunction setup()\n    print \"setup\"\nend\n"
                "function test_add() returns void\n    testing.assert_equal(1 + 1, 2)\nend\n"
                "function helper() returns void\n    testing.fail(\"not a test\")\nend\n", encoding="utf-8")
            (suite_dir / "nested" / "math_test.fw").write_text(
                "import testing\nfunction test_bad() returns void\n    testing.assert_equal(2 * 2, 5, \"product\")\nend\n"
                "function test_good() returns void\n    testing.assert(true)\nend\n", encoding="utf-8")
            (suite_dir / "build" / "test_skipped.fw").write_text("throw ValueError(\"ran\")\n", encoding="utf-8")
            cases = [
                ("short", ["-s", "print 6 * 7"], 0, "42\n"),
                ("long", ["--source", "print 6 * 7"], 0, "42\n"),
                ("equals", ["--source=print 6 * 7"], 0, "42\n"),
                ("run_after_options", ["run", "-O2", "-g", "--source", "print 1"], 0, "1\n"),
                ("run_arguments", ["run", "-s", argv_code, "--", "a", "-b"], 0, "[\"a\", \"-b\"]\n"),
                ("implicit_arguments", ["--source", argv_code, "x"], 0, "[\"x\"]\n"),
                ("export_o", ["export", "-s", "return [1, 2]", "-o", str(out)], 0, ""),
                ("check_error", ["check", "-s", "print 1 +"], 1, ""),
                ("unknown_option", ["run", "--bogus", "x.fw"], 2, ""),
                ("compiler_only", ["build", "x.fw"], 2, ""),
                ("alias_run", ["r", "-s", "print 6 * 7"], 0, "42\n"),
                ("alias_check", ["c", "-s", "print 1"], 0, ""),
                ("alias_compiler_only", ["f", "x.fw"], 2, ""),
                ("alias_version", ["v"], 0, None),
                ("alias_help", ["h"], 0, None),
                ("test", ["test", str(suite_dir)], 1,
                 f"{suite_dir / 'nested' / 'math_test.fw'}\n"
                 "  FAIL test_bad: assertion failed: product: expected 5, got 4\n  ok   test_good\n"
                 f"{suite_dir / 'test_ok.fw'}\nsetup\n  ok   test_add\n2 test file(s), 1 failed\n"),
                ("test_file", ["test", str(suite_dir / "test_ok.fw")], 0, None),
            ]
            for n, args, code, expected in cases:
                name = f"cli/{n}"
                if pattern and pattern not in name:
                    continue
                r = cli(*args, cwd=tmp)
                ok = r.returncode == code and (expected is None or r.stdout.replace("\\", "/") ==
                                               expected.replace("\\", "/"))
                if n == "export_o":
                    ok = ok and out.exists() and out.read_text(encoding="utf-8") == "[1, 2]"
                if n == "check_error":
                    ok = ok and r.stderr.startswith("<source>:1:")
                check(name, ok, f"exit {r.returncode} (want {code})\n--- expected\n{expected}--- got\n"
                                f"{r.stdout}--- stderr\n{r.stderr}")

    known = load_known()
    regressions = 0
    for name, detail in failures:
        tag = f"  (known: {known[name]})" if name in known else ""
        if name not in known:
            regressions += 1
        print(f"FAIL {name}{tag}")
        if verbose:
            print("    " + detail.replace("\n", "\n    "))
    total = passed + failed
    print(f"\n{passed}/{total} passed, {failed} failed ({failed - regressions} known), {skipped} skipped ("
          + ", ".join(f"{k} {v}" for k, v in sorted(skips.items())) + ")")
    return 1 if regressions else 0


if __name__ == "__main__":
    sys.exit(main())
