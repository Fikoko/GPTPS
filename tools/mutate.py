#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Fikoko. See LICENSE for the full text.
"""
mutate.py - does a test notice when the code it covers is wrong?

    python3 tools/mutate.py --file src/settings.c --lines 600-780 --test config_strict
    python3 tools/mutate.py --since HEAD~3 --test 'config_strict|toml|fuzz'
    python3 tools/mutate.py --file src/engine.c --sample 150 --test . --exclude '_perf$|bench'

A test that passes against a broken build is a test that checks nothing. This
breaks the code on purpose, one small change at a time, and runs the tests that
are meant to cover it:

    ==  <->  !=      <  <->  <=      >  <->  >=      &&  <->  ||
    if (X)  ->  if (!(X))      return 0;  <->  return 1;
    + 1  <->  - 1               true  <->  false

Each change - a mutant - should make a test fail: KILLED. One that passes
SURVIVED, and is either a weak test or a change that does not matter (an
"equivalent" mutant: `<` and `<=` on a value that is never equal). Read each
survivor and decide. One that does not compile is INVALID, and one that hangs
past --timeout is counted killed (the test noticed, the hard way).

A whole file can hold thousands of mutants. --sample N tests N of them chosen
at random - the same N for the same --seed - and the score then estimates the
file's: 150 mutants put it within about eight points, nineteen times in twenty.

The work happens in a copy of the tree - tracked files and uncommitted changes,
build directories left out - under --work, so the tree you are editing is never
touched. The copy is configured once; each mutant then rebuilds what changed and
runs the tests through ctest. Comments, string literals and preprocessor lines
are never mutated.

Needs python3, cmake and the project's normal toolchain. Linux and macOS; the
copy uses POSIX paths.
"""

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import time

OPS = [
    # (name, pattern, replacement) - one occurrence at a time
    ("== to !=", r"==", "!="),
    ("!= to ==", r"!=", "=="),
    ("<= to <", r"(?<![<>])<=", "<"),
    (">= to >", r"(?<![<>])>=", ">"),
    ("< to <=", r"(?<![<\-])<(?![<=])", "<="),
    ("> to >=", r"(?<![>\-])>(?![>=])", ">="),
    ("&& to ||", r"&&", "||"),
    ("|| to &&", r"\|\|", "&&"),
    ("+ 1 to - 1", r"\+ 1\b", "- 1"),
    ("- 1 to + 1", r"(?<![-])- 1\b", "+ 1"),
    ("true to false", r"\btrue\b", "false"),
    ("false to true", r"\bfalse\b", "true"),
]
RETURN_FLIP = [("return 0; to return 1;", r"\breturn 0;", "return 1;"),
               ("return 1; to return 0;", r"\breturn 1;", "return 0;")]
IF_NEGATE = re.compile(r"\bif \((.*)\)\s*(\{|[^;{]*;)?\s*$")


def code_mask(text):
    """Per character: True where it is code, False inside comments, strings,
    character constants and preprocessor lines - where a mutation would either
    not compile or not mean anything."""
    mask = [True] * len(text)
    i, n = 0, len(text)
    line_start = True
    while i < n:
        c = text[i]
        if line_start and c in " \t":
            i += 1
            continue
        if line_start and c == "#":                         # preprocessor line, with continuations
            j = i
            while j < n and not (text[j] == "\n" and text[j - 1] != "\\"):
                j += 1
            for k in range(i, j):
                mask[k] = False
            i = j
            continue
        line_start = (c == "\n")
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                mask[k] = False
            i = j
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                mask[k] = False
            i = j
            continue
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            for k in range(i, min(j + 1, n)):
                mask[k] = False
            i = j + 1
            continue
        i += 1
    return mask


def mutants_for(path, text, wanted_lines):
    """Every single-site mutation on the wanted lines: (line, column, description,
    start, end, replacement) - text[start:end] becomes the replacement. The column
    says which of several operators on a line was changed."""
    mask = code_mask(text)
    starts = [0]
    for m in re.finditer("\n", text):
        starts.append(m.end())
    out = []
    for ln in sorted(wanted_lines):
        if ln < 1 or ln > len(starts):
            continue
        a = starts[ln - 1]
        b = starts[ln] - 1 if ln < len(starts) else len(text)
        line = text[a:b]
        for name, pat, rep in OPS + RETURN_FLIP:
            for m in re.finditer(pat, line):
                s, e = a + m.start(), a + m.end()
                if not all(mask[s:e]):
                    continue
                out.append((ln, m.start() + 1, name, s, e, rep))
        m = IF_NEGATE.search(line)
        if m and mask[a + m.start()]:
            cond = m.group(1)
            if cond.count("(") == cond.count(")"):
                s = a + m.start(1)
                e = a + m.end(1)
                out.append((ln, m.start(1) + 1, "negate the if", s, e, "!(" + cond + ")"))
    return out


def run(cmd, cwd, timeout=None):
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, timeout=timeout)


def changed_lines(repo, rev):
    """{file: set(lines)} added or changed since `rev`, for .c files."""
    out = run(["git", "diff", "-U0", rev, "--", "*.c"], repo).stdout
    files, cur = {}, None
    for line in out.splitlines():
        if line.startswith("+++ "):
            cur = line[6:] if line.startswith("+++ b/") else None
        elif line.startswith("@@") and cur:
            m = re.search(r"\+(\d+)(?:,(\d+))?", line)
            start, count = int(m.group(1)), int(m.group(2) or "1")
            files.setdefault(cur, set()).update(range(start, start + count))
    return files


def make_copy(repo, work):
    """The tree as it is now - tracked files plus uncommitted and new ones - in `work`."""
    src = os.path.join(work, "src-tree")
    if os.path.isdir(src):
        shutil.rmtree(src)
    listed = run(["git", "ls-files", "-co", "--exclude-standard"], repo).stdout.split("\n")
    for rel in listed:
        if not rel or not os.path.isfile(os.path.join(repo, rel)):
            continue
        dst = os.path.join(src, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(os.path.join(repo, rel), dst)
    return src


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--file", action="append", default=[], help="a .c file to mutate (repeatable)")
    ap.add_argument("--lines", help="line range for --file, e.g. 600-780 (default: the whole file)")
    ap.add_argument("--since", help="mutate the .c lines changed since this git revision")
    ap.add_argument("--test", required=True, help="ctest -R regex: the tests that should notice")
    ap.add_argument("--exclude", help="ctest -E regex: tests to leave out, such as timing tests that a loaded machine fails")
    ap.add_argument("--work", default=os.path.join(os.environ.get("TMPDIR", "/tmp"), "gptps-mutate"),
                    help="scratch directory for the copy and its build")
    ap.add_argument("--cmake-args", default="-DCMAKE_BUILD_TYPE=Debug", help="extra cmake configure arguments")
    ap.add_argument("--timeout", type=int, default=120, help="seconds a test run may take")
    ap.add_argument("--max", type=int, default=0, help="stop after this many mutants (0 = all)")
    ap.add_argument("--sample", type=int, default=0,
                    help="test this many mutants chosen at random (0 = all); the score estimates the whole")
    ap.add_argument("--seed", type=int, default=1, help="random seed for --sample")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()

    repo = run(["git", "rev-parse", "--show-toplevel"], os.getcwd()).stdout.strip()
    if not repo:
        sys.exit("run it inside the GPTPS git tree")
    targets = {}
    if a.since:
        targets = changed_lines(repo, a.since)
    for f in a.file:
        rel = os.path.relpath(os.path.abspath(f), repo)
        with open(os.path.join(repo, rel)) as fh:
            total = fh.read().count("\n") + 1
        if a.lines:
            lo, hi = (int(x) for x in a.lines.split("-"))
        else:
            lo, hi = 1, total
        targets.setdefault(rel, set()).update(range(lo, hi + 1))
    if not targets:
        sys.exit("nothing to mutate: give --file or --since")

    os.makedirs(a.work, exist_ok=True)
    tree = make_copy(repo, a.work)
    build = os.path.join(a.work, "build")
    r = run(["cmake", "-S", tree, "-B", build] + a.cmake_args.split(), a.work)
    if r.returncode:
        sys.exit("configure failed:\n" + r.stdout[-2000:])
    r = run(["cmake", "--build", build, "-j", str(a.jobs)], a.work)
    if r.returncode:
        sys.exit("the unmutated tree does not build:\n" + r.stdout[-2000:])
    tests = ["-R", a.test] + (["-E", a.exclude] if a.exclude else [])
    r = run(["ctest", "--test-dir", build, "-j", str(a.jobs), "--no-tests=error"] + tests, a.work, a.timeout * 4)
    if r.returncode:
        sys.exit("the tests fail on the unmutated tree - fix that first:\n" + r.stdout[-2000:])

    originals, plan = {}, []
    for rel, lines in sorted(targets.items()):
        with open(os.path.join(tree, rel)) as fh:
            originals[rel] = fh.read()
        plan += [(rel,) + m for m in mutants_for(rel, originals[rel], lines)]
    if a.sample and a.sample < len(plan):
        print("testing %d of the %d mutants, chosen at random (--seed %d)" % (a.sample, len(plan), a.seed), flush=True)
        plan = sorted(random.Random(a.seed).sample(plan, a.sample))

    tally = {"KILLED": 0, "SURVIVED": 0, "INVALID": 0, "TIMEOUT": 0}
    survivors = []
    count = 0
    t0 = time.time()
    for rel, ln, col, name, s, e, rep in plan:
        if a.max and count >= a.max:
            break
        count += 1
        path, original = os.path.join(tree, rel), originals[rel]
        with open(path, "w") as fh:
            fh.write(original[:s] + rep + original[e:])
        try:
            b = run(["cmake", "--build", build, "-j", str(a.jobs)], a.work)
            if b.returncode:
                verdict = "INVALID"
            else:
                try:
                    t = run(["ctest", "--test-dir", build, "-j", str(a.jobs), "--timeout", str(a.timeout),
                             "--no-tests=error"] + tests, a.work, a.timeout * 4)
                    verdict = "KILLED" if t.returncode else "SURVIVED"
                except subprocess.TimeoutExpired:
                    verdict = "TIMEOUT"
        finally:
            with open(path, "w") as fh:
                fh.write(original)
        tally[verdict] += 1
        raw = original.split("\n")[ln - 1]
        shown = raw.strip()
        mark = " " * (col - 1 - (len(raw) - len(raw.lstrip()))) + "^"
        print("%-8s %s:%d:%d  %s   | %s" % (verdict, rel, ln, col, name, shown[:90]), flush=True)
        if verdict == "SURVIVED":
            survivors.append("%s:%d:%d  %s\n      %s\n      %s" % (rel, ln, col, name, shown[:110], mark[:110]))
    run(["cmake", "--build", build, "-j", str(a.jobs)], a.work)     # leave the copy built as it is

    tested = tally["KILLED"] + tally["SURVIVED"] + tally["TIMEOUT"]
    print("\n%d mutants in %.0f s: %d killed, %d timed out, %d survived, %d did not compile"
          % (count, time.time() - t0, tally["KILLED"], tally["TIMEOUT"], tally["SURVIVED"], tally["INVALID"]))
    if tested:
        print("score: %.0f%% of the mutants that built were noticed" % (100.0 * (tested - tally["SURVIVED"]) / tested))
    if survivors:
        print("\nsurvivors - a weak test, or a change that does not matter:")
        for s in survivors:
            print("  " + s)
    return 1 if survivors else 0


if __name__ == "__main__":
    sys.exit(main())
