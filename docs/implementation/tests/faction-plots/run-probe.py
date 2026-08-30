#!/usr/bin/env python3
"""Compile, link and RUN a probe against real plugin sources.

Usage:
    run-probe.py <probe.cpp> [extra-source.cpp ...]

Several steps of PHASE_14_FACTION_PLOTS.md verify pure functions -- the step
label derivation, the tick schedule, the progress-race arithmetic -- and doing
that needs the code to actually execute, not merely compile. This builds a tiny
console executable from the probe plus whichever plugin translation units it
needs, runs it, and forwards its exit code.

Flags come from the build's own compile_commands.json, so the probe sees the
same standard level, defines and include paths the plugin is built with. Two
things are stripped: the precompiled header (the probe is not part of that
build) and the DLL-ish output flags.

This only works for translation units that are genuinely engine-free. A source
that calls into CommonLibSSE will fail to LINK here, and that failure is
informative rather than an obstacle: it means the code under test is not the
pure function the phase doc says it should be.

Must be run inside the VS Developer environment -- use run-probe.ps1.
"""
from __future__ import annotations

import json
import os
import pathlib
import re
import shlex
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[4]
DB = REPO / "build" / "local-release" / "compile_commands.json"
REF = "src/PlotModel.cpp"

# Flags that only make sense for the plugin's own build.
DROP_PREFIXES = ("/Yu", "/Fp", "/FI", "/Fo", "/Fd", "/Fa", "-c")


def flags_from_db() -> tuple[list[str], str]:
    with open(DB, encoding="utf-8") as f:
        db = json.load(f)
    entry = next((e for e in db if e["file"].replace("\\", "/").endswith(REF)), None)
    if entry is None:
        raise SystemExit("FATAL: no compile_commands entry for " + REF)

    tokens = shlex.split(entry["command"], posix=False)
    keep = []
    for tok in tokens[1:]:  # tokens[0] is cl.exe itself
        bare = tok.strip('"')
        if bare.endswith(".cpp"):
            continue
        if any(bare.startswith(p) for p in DROP_PREFIXES):
            continue
        keep.append(tok)
    return keep, entry.get("directory", str(REPO))


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    probe = pathlib.Path(sys.argv[1]).resolve()
    extras = [str((REPO / s).resolve()) if not os.path.isabs(s) else s for s in sys.argv[2:]]

    flags, cwd = flags_from_db()
    outdir = probe.parent
    exe = outdir / (probe.stem + ".exe")

    cmd = ["cl.exe"] + flags + [str(probe)] + extras + [
        "/Fe:" + str(exe),
        "/Fo:" + str(outdir) + os.sep,
        "/EHsc",
        "/nologo",
    ]

    build = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors="replace")
    if build.returncode != 0:
        print("PROBE DID NOT BUILD")
        for line in ((build.stdout or "") + (build.stderr or "")).splitlines():
            if re.search(r"\b(error|fatal)\b", line, re.I):
                print("    " + line.strip()[:170])
        return 2

    run = subprocess.run([str(exe)], capture_output=True, text=True, errors="replace")
    sys.stdout.write(run.stdout or "")
    sys.stderr.write(run.stderr or "")

    for junk in outdir.glob(probe.stem + ".*"):
        if junk.suffix in (".obj", ".exe", ".ilk", ".pdb"):
            junk.unlink(missing_ok=True)
    for extra in extras:
        obj = outdir / (pathlib.Path(extra).stem + ".obj")
        obj.unlink(missing_ok=True)

    return run.returncode


if __name__ == "__main__":
    sys.exit(main())
