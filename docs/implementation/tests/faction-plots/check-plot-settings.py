#!/usr/bin/env python3
"""Three-way consistency check for the [Plots] settings surface.

Every plot setting exists in three places, and nothing in the build makes them
agree:

  1. include/Settings.h            -- the baked-in default the plugin uses when
                                      no INI supplies a value.
  2. statics/.../NarrativeEngine.ini -- the shipped author default.
  3. docs/implementation/PHASE_14_FACTION_PLOTS.md -- the settings table the
                                      phase doc states.

Drift between them is silent and, in the INI case, actively misleading: a
player reading the INI sees a number the plugin is not using. This asserts all
three carry the same value for every key, and that no key exists in one place
and not the others.

Run from anywhere:  python check-plot-settings.py
Exit 0 if consistent, 1 otherwise.

Step 1 of PHASE_14_FACTION_PLOTS.md. Kept rather than thrown away because
every later step that adds a setting wants this run again.
"""
from __future__ import annotations

import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[4]
HEADER = REPO / "include" / "Settings.h"
INI = REPO / "statics" / "SKSE" / "Plugins" / "NarrativeEngine.ini"
DOC = REPO / "docs" / "implementation" / "PHASE_14_FACTION_PLOTS.md"

# INI key -> the Settings::Config field it populates. The mapping is explicit
# rather than derived, because a typo in either name is exactly the class of
# bug this script exists to catch and a clever derivation would hide it.
KEYS = {
    "bPlotsEnabled": "plotsEnabled",
    "bPlotLogEnabled": "plotLogEnabled",
    "fPlotTickIntervalGameHours": "plotTickIntervalGameHours",
    "iPlotMaxOutstandingTicks": "plotMaxOutstandingTicks",
    "iPlotMaxConcurrent": "plotMaxConcurrent",
    "fPlotMastermindCooldownDays": "plotMastermindCooldownDays",
    "fPlotActorCooldownDays": "plotActorCooldownDays",
    "iPlotMaxAdaptations": "plotMaxAdaptations",
    "iPlotStepHistoryCap": "plotStepHistoryCap",
    "fPlotTerminalRetentionDays": "plotTerminalRetentionDays",
    "fPlotProgressRateMin": "plotProgressRateMin",
    "fPlotProgressRateMax": "plotProgressRateMax",
    "fPlotProgressMaxFraction": "plotProgressMaxFraction",
    "bPlotMishapEnabled": "plotMishapEnabled",
    "fPlotMishapChanceBase": "plotMishapChanceBase",
    "iPlotRandomSeed": "plotRandomSeed",
}


def normalise(raw: str) -> str:
    """Compare values by meaning, not by spelling: 12.0, 12.0f and 12 are one."""
    v = raw.strip().rstrip("f").strip()
    low = v.lower()
    if low in ("true", "false"):
        return low
    try:
        return "{:.6g}".format(float(v))
    except ValueError:
        return v


def read_header() -> dict[str, str]:
    text = HEADER.read_text(encoding="utf-8")
    out = {}
    for field in KEYS.values():
        m = re.search(r"^\s*(?:bool|int|float)\s+" + re.escape(field) + r"\s*=\s*([^;]+);", text, re.M)
        if m:
            out[field] = normalise(m.group(1))
    return out


def read_ini() -> dict[str, str]:
    text = INI.read_text(encoding="utf-8")
    body = text.split("[Plots]", 1)[1] if "[Plots]" in text else ""
    # Stop at the next section header, so a later [Section] cannot leak in.
    body = re.split(r"^\[", body, maxsplit=1, flags=re.M)[0]
    out = {}
    for line in body.splitlines():
        line = line.strip()
        if not line or line.startswith(";"):
            continue
        if "=" in line:
            k, _, v = line.partition("=")
            out[k.strip()] = normalise(v)
    return out


def read_doc() -> dict[str, str]:
    text = DOC.read_text(encoding="utf-8")
    out = {}
    for line in text.splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 2:
            continue
        key = cells[0].strip("`")
        if key in KEYS:
            out[key] = normalise(cells[1])
    return out


def main() -> int:
    header, ini, doc = read_header(), read_ini(), read_doc()
    problems = []

    for ini_key, field in sorted(KEYS.items()):
        h = header.get(field)
        i = ini.get(ini_key)
        d = doc.get(ini_key)
        if h is None:
            problems.append("{}: no default found in Settings.h (field {})".format(ini_key, field))
            continue
        if i is None:
            problems.append("{}: missing from the shipped INI's [Plots] block".format(ini_key))
            continue
        if d is None:
            problems.append("{}: missing from the phase doc's settings table".format(ini_key))
            continue
        if not (h == i == d):
            problems.append(
                "{}: Settings.h={}  INI={}  doc={}".format(ini_key, h, i, d))

    for stray in sorted(set(ini) - set(KEYS)):
        problems.append("{}: present in the INI's [Plots] block but unknown to this check".format(stray))

    if problems:
        print("[Plots] settings are INCONSISTENT:")
        for p in problems:
            print("  - " + p)
        return 1

    print("[Plots] settings consistent across Settings.h, the shipped INI and the phase doc "
          "({} keys).".format(len(KEYS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
