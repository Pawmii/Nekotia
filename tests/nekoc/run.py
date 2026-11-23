#!/usr/bin/env python3
"""nekoc tests: tests/nekoc/*.cs|*.neko are compiled as small user programs.

  - `// error NEKOxxxx` on a line: nekoc must report exactly that code on exactly that line
    (and nothing else in the file);
  - a file without such markers must transpile cleanly, and the generated C++ must compile
    (clang -fsyntax-only with the user-program flags).

    tests/nekoc/run.py [NAME…]      (make test-nekoc)
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NEKOC = ["dotnet", os.path.join(ROOT, "build/tools/nekoc/nekoc.dll")]
SDK = ["sdk/Nekotia.Sdk/Attributes.cs", "sdk/Nekotia.Sdk/Intrinsics.cs", "sdk/Nekotia.Sdk/Debug.cs",
       "sdk/Nekotia.Sdk/Maths.cs", "sdk/Nekotia.Sdk/Text.cs", "sdk/Nekotia.User"]
CXX = ["clang++", "-std=c++20", "-fsyntax-only", "--target=x86_64-unknown-none-elf", "-ffreestanding",
       "-fno-exceptions", "-fno-rtti", "-Wall", "-Wno-unused-parameter", "-Wno-unused-variable",
       "-Wno-unused-but-set-variable", "-Wno-parentheses-equality", "-Werror",
       "-Iruntime/user/include", "-Iruntime/include", "-DNEKO_HOSTED=0"]
DIAG = re.compile(r"^(?P<file>.*)\((?P<line>\d+),\d+\): error (?P<code>NEKO\d+): (?P<msg>.*)$")


def run(name):
    path = os.path.join(ROOT, "tests/nekoc", name)
    work = os.path.join(ROOT, "build/nekoc-tests", os.path.splitext(name)[0])
    os.makedirs(work, exist_ok=True)
    config = {"name": "t", "target": "user", "output": os.path.join(work, "gen"),
              "sources": [os.path.join(ROOT, s) for s in SDK] + [path], "defines": ["NEKOTIA", "NEKOTIA_USER"]}
    with open(os.path.join(work, "nekotia.json"), "w") as f:
        json.dump(config, f)
    expected = set()
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            marker = re.search(r"//\s*error ((?:NEKO\d+\s*)+)", line)
            for code in (marker.group(1).split() if marker else []):
                expected.add((number, code))
    command = "check" if expected else "build"
    out = subprocess.run(NEKOC + [command, "--config", os.path.join(work, "nekotia.json")],
                         cwd=ROOT, capture_output=True, text=True)
    got = set()
    messages = {}
    for line in (out.stdout + out.stderr).splitlines():
        m = DIAG.match(line.strip())
        if m and os.path.basename(m["file"]) == name:
            got.add((int(m["line"]), m["code"]))
            messages[(int(m["line"]), m["code"])] = m["msg"]
        elif m:
            got.add((0, m["code"]))  # a diagnostic in the SDK is always a failure
            messages[(0, m["code"])] = f'{m["file"]}: {m["msg"]}'
    problems = []
    for item in sorted(expected - got):
        problems.append(f"line {item[0]}: expected {item[1]}, not reported")
    for item in sorted(got - expected):
        problems.append(f"line {item[0]}: unexpected {item[1]} — {messages[item]}")
    if not expected and not problems:
        if out.returncode != 0:
            problems.append("nekoc build failed:\n" + out.stdout + out.stderr)
        else:
            cxx = subprocess.run(CXX + [os.path.join(work, "gen/nekotia.gen.cpp")], cwd=ROOT, capture_output=True, text=True)
            if cxx.returncode != 0:
                problems.append("generated C++ does not compile:\n" + cxx.stderr[:4000])
    return problems


def main():
    names = sys.argv[1:] or sorted(n for n in os.listdir(os.path.join(ROOT, "tests/nekoc")) if n.endswith((".cs", ".neko")))
    failed = 0
    for name in names:
        problems = run(name)
        print(f"  {'ok  ' if not problems else 'FAIL'}  {name}")
        for p in problems:
            print("        " + p.replace("\n", "\n        "))
        failed += bool(problems)
    print(f"nekoc tests: {len(names) - failed} passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
