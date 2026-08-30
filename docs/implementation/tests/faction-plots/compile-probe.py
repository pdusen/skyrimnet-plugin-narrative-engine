"""Compile a throwaway TU against the real build flags and report the result.

Usage: probe.py <source.cpp> pass
       probe.py <source.cpp> fail <expected-error-code>

Pulls the compile command for src/PlotDispatch.cpp out of the build's
compile_commands.json and substitutes the probe source, so the probe sees
exactly the include paths, defines, standard level, warning flags and forced
PCH include the real build uses.

A negative probe must fail with the SPECIFIC diagnostic it is asserting, not
merely fail. A typo in an include name also "fails to compile", and counting
that as a pass is how a negative probe silently stops testing anything.
"""
import json
import os
import re
import subprocess
import sys

REPO = r"C:\Projects\NarrativeEngine"
DB = os.path.join(REPO, "build", "local-release", "compile_commands.json")
REF = "src/PlotDispatch.cpp"


def main():
    src = os.path.abspath(sys.argv[1])
    expect = sys.argv[2]
    want_code = sys.argv[3] if len(sys.argv) > 3 else None

    with open(DB, encoding="utf-8") as f:
        db = json.load(f)

    entry = next((e for e in db if e["file"].replace("\\", "/").endswith(REF)), None)
    if entry is None:
        print("FATAL: no compile_commands entry for " + REF)
        return 2

    obj = src + ".obj"
    cmd = entry["command"].replace(entry["file"], src)
    cmd = re.sub(r"/Fo\S+", lambda m: "/Fo" + obj, cmd)
    cmd = re.sub(r"/Fd\S+", "", cmd)

    proc = subprocess.run(
        cmd, cwd=entry.get("directory", REPO), shell=True,
        capture_output=True, text=True, errors="replace",
    )
    out = (proc.stdout or "") + (proc.stderr or "")
    ok = proc.returncode == 0

    codes = sorted(set(re.findall(r"\berror ([A-Z]\d+)", out)))

    if expect == "pass":
        good = ok
        why = "compiled" if ok else "did NOT compile: " + str(codes)
    else:
        good = (not ok) and (want_code in codes if want_code else True)
        if ok:
            why = "compiled, but must NOT"
        elif want_code and want_code not in codes:
            why = "failed with {} - expected {}".format(codes or "no error code", want_code)
        else:
            why = "rejected with " + str(want_code)

    print("{:<34} expect={:<5} {:<6} :: {}".format(
        os.path.basename(src), expect, "PASS" if good else "FAIL", why))
    for line in out.splitlines():
        if re.search(r"\berror [A-Z]\d+", line):
            print("      " + line.strip()[:150])
    if os.path.exists(obj):
        os.remove(obj)
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
