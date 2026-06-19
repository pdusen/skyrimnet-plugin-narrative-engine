# NarrativeEngine — Project Context

## What this project is

A Skyrim mod, in the **planning phase**. The mod will be a **plugin for SkyrimNet** — not a standalone mod.

**SkyrimNet** is itself a Skyrim mod (combining an `.esp` and an SKSE plugin) that puts LLMs in control of various
emergent gameplay systems — most notably ad-hoc dialogue generation for NPCs. SkyrimNet exposes a plugin surface so
other mods can extend its LLM-driven systems.

NarrativeEngine will likely require both an `.esp` and an SKSE plugin of its own (the same shape as SkyrimNet itself,
and the same shape as the IntelEngine prior-art reference described below).

## Current phase: planning only

We are creating **planning documents** in this directory. **No code yet.** Do not scaffold a build, create source
files, write a CMakeLists, or initialize a repo unless explicitly asked.

The working concept, target SkyrimNet integration surface, and SKSE toolchain are all **still open questions** — the
user has not yet committed to answers. Do not assume them.

## Prior art reference: IntelEngine

The creator of another SkyrimNet plugin called **IntelEngine** stopped developing it and released the source as open
source. It is split across two local repos:

- `C:\Projects\IntelEngine-NativePlugin\` — SKSE source tree (C++ + Papyrus + dashboard source + CK design docs)
- `C:\Projects\IntelEngine-GamePlugin\` — deployment / `Data/` payload (compiled `.esp`, `.dll`, `.pex`, SkyrimNet
  asset YAMLs/prompts, MCM Helper JSON, PrismaUI views, FOMOD)

Both are **read-only** for NarrativeEngine work — do not modify files there.

### Detailed notes live in `docs/prior-art/`

Everything Claude has learned about IntelEngine is organized in [`docs/prior-art/`](docs/prior-art/README.md). It is a
**lookup index, not a checklist**. Consult it when a specific NarrativeEngine design question is in front of you and
you want to see whether the IntelEngine author solved a similar one; do not browse it for inspiration, and do not
pre-import its structures. See "IntelEngine's role in this project" below for the discipline.

[`docs/prior-art/REPO_MAP.md`](docs/prior-art/REPO_MAP.md) — Where in the IntelEngine repos each subsystem,
asset, and file lives.

[`docs/prior-art/FEATURE_OVERVIEW.md`](docs/prior-art/FEATURE_OVERVIEW.md) — What IntelEngine does at the
gameplay level (feature list, pitch).

[`docs/prior-art/ARCHITECTURE.md`](docs/prior-art/ARCHITECTURE.md) — Full preserved architecture analysis:
layers, every C++ module, every Papyrus script, data flow, threading, persistence, dependency graph.

[`docs/prior-art/SKYRIMNET_PLUGIN_CONTRACT.md`](docs/prior-art/SKYRIMNET_PLUGIN_CONTRACT.md) — **The SkyrimNet
plugin extension contract** — actions, categories, manifest/variants/schema, prompts, character-bio
submodules, decorators, ModEvents, StorageUtil namespace.

[`docs/prior-art/ESP_STRUCTURE.md`](docs/prior-art/ESP_STRUCTURE.md) — `.esp` / Creation Kit setup: quest,
alias slot pattern, AI packages (speed variants + linked-ref keying), keywords, globals, TaskFaction, package
priorities.

[`docs/prior-art/DEPLOYMENT_LAYOUT.md`](docs/prior-art/DEPLOYMENT_LAYOUT.md) — What ships in the `Data/`
folder and which tool consumes each file.

[`docs/prior-art/PATTERNS_AND_LESSONS.md`](docs/prior-art/PATTERNS_AND_LESSONS.md) — Reusable patterns and
lessons: three-phase async, fuzzy cascade, escalating recovery, soft dependency loading, dispatch ring
buffer, save-scum recovery, single source of truth.

### IntelEngine's role in this project

IntelEngine is **one solved instance** of a similar-shape project — but it solved its own problems, not ours. Treat
the prior-art docs as a *reference for specific problems you've decided to solve*, not as a list of features to mirror
or an architecture to echo.

The right workflow when a design question comes up:

1. **Start from NarrativeEngine's actual needs.** What does our current scope (the feature in front of you, the phase
   you're in, the action being authored) genuinely require? Decide from first principles.
2. **Then check prior art, only if relevant.** If you've decided you need to solve problem X, the IntelEngine docs may
   show one working solution. If they do, adopt it where it fits, adapt where it almost fits, or ignore where our
   problem is different.
3. **Never let IntelEngine's structure dictate ours by default.** If you find yourself adding a form, a subsystem, a
   pattern, or a phase task "because IntelEngine has one," stop and re-justify it on NarrativeEngine's own terms.
   Anything that can't be justified that way doesn't belong here, even if IntelEngine had it.

Examples of the anti-pattern to avoid:

- "IntelEngine has speed-variant Travel packages, so we need them." → No. Phase 1 has no `ChangeSpeed` action; one
  walk-pace package is enough.
- "IntelEngine has 5 agent alias slots, so we need 5." → No. Pick the minimum the actual cooldown / dispatch math
  supports.
- "IntelEngine declares MCM globals for every setting, so we should too." → No. Our INI settings already cover the
  same surface; CK globals would be duplication.
- "IntelEngine has a faction-politics sim, so a sim is the right pattern for background simulation." → Only if our
  design needs that *shape*. Otherwise design from scratch.

The IntelEngine docs are useful precisely *because* they're a frozen snapshot of one author's choices — but those
choices answered IntelEngine's questions, not ours.

### Even when IntelEngine fits, ask if there's a better design

"Fits the problem" is not the same as "is the best solution." Even when an IntelEngine pattern matches a problem
NarrativeEngine actually has, ask whether a better design exists along these axes before adopting:

- **Simplicity** — is IntelEngine's solution more complicated than NarrativeEngine needs? IntelEngine's scope was
  much broader (faction politics, story DM, battle manager, in-game React dashboard, …) and its solutions were
  sized to that scope. What was a reasonable cost there may be over-engineering here.
- **Efficiency** — is IntelEngine doing more work than NarrativeEngine needs? Extra indexing passes, polling loops,
  state to track, LLM calls per cycle, save/load payload, and so on. Strip anything our actual feature surface
  doesn't justify.
- **Encapsulation** — is IntelEngine crossing the C++ / Papyrus boundary more than necessary? Boundary crossings
  (native bridge calls, ModEvents, StorageUtil reads/writes) are expensive to write, expensive to debug, and a
  recurring source of subtle bugs. Keep work on one side of the boundary unless there's a clear reason to cross.
  IntelEngine's ~5000-line `Papyrus.cpp` exposing ~200 natives is the cautionary example.
- **Maintainability** — is IntelEngine's solution organized in a way that's hard to understand or change? Sprawling
  single files, tightly-coupled subsystems, dense property surfaces on Papyrus scripts, and reliance on
  out-of-band conventions all add cost. A simpler, smaller, more localized design is almost always better when the
  feature surface allows for it.

Adopting wholesale without this check is the same anti-pattern as "we need it because IntelEngine has it" — just
in a different shape. The first time you find yourself thinking "IntelEngine does it this way, so I'll do the
same," stop and ask: can I do it better? If yes, do that instead and write down why.

### Treat IntelEngine's code with skepticism, not deference

IntelEngine shipped — but it was active development that was abandoned mid-cycle, not a polished or verified
reference. The original author would not claim it was bug-free. The code is full of subtle quirks, edge cases that
were never fully resolved, magic numbers chosen by feel, and patterns that worked well enough to ship without
being verifiably correct. We've already surfaced examples of this in the prior-art docs (fabricated CK field
names, non-existent AI Package "Priority" field, "Initially Cleared" alias flag that doesn't exist).

When adopting from IntelEngine, read it as **"here's how someone got far enough to ship,"** not **"here's the
verified-correct way."** Specific values, field names, condition forms, threshold tunings, and edge-case
handling should all be cross-checked against authoritative sources before being trusted:

- CK Wiki / Bethesda's Creation Kit documentation for any CK form details.
- CommonLibSSE-NG source / headers for any C++ engine API claim.
- Papyrus language reference for any script-language claim.
- The actual game (in-game testing, console commands, xEdit inspection of the live `IntelEngine.esp`) for any
  runtime-behavior claim.
- SkyrimNet's `PublicAPI.h` for any SkyrimNet integration claim.

If IntelEngine does something a certain way and the authoritative source contradicts it, the authoritative source
wins. If IntelEngine has unexplained magic in its code, treat that as a smell, not a precedent.

## What NOT to assume

- That NarrativeEngine will reuse IntelEngine's architecture, module decomposition, or naming.
- That NarrativeEngine wants any specific subset of IntelEngine's features (task slots, faction politics, dashboard,
  story DM, etc.). The user has not yet described the mod's concept.
- That the SKSE toolchain is decided (CommonLibSSE-NG vs. alternatives) or that the Papyrus/C++ split will mirror IntelEngine's.

## Working directory conventions

- Planning artifacts live in this directory (`C:\Projects\NarrativeEngine\`).
- `docs/prior-art/` is the IntelEngine reference library — extend it (don't rewrite it) if new learnings come in about IntelEngine.
- The IntelEngine repos at `C:\Projects\IntelEngine-NativePlugin\` and `C:\Projects\IntelEngine-GamePlugin\` are
  **read-only reference** — do not modify files there.

## C++ source layout

These rules apply to **every** C++ source file we author:

- `.cpp` files go in `src/`.
- `.h` files go in `include/`.

The CMake build picks up `src/*.cpp` automatically (via the glob in `CMakeLists.txt`) and puts `include/` on the
include path. That means our own headers should be included with angle brackets — `#include <Foo.h>` — to match the
existing convention used for `<logger.h>` and `<PublicAPI.h>`.

**Exception: files that already exist at the project root stay at the project root.** Specifically:

- `plugin.cpp` — the SKSE `SKSEPluginLoad` entry point. The CMake target adds it explicitly alongside the
  `src/*.cpp` glob; it's kept thin and forwards into `src/Plugin.cpp`.
- `PCH.h` — the project-wide precompiled header, wired in via `target_precompile_headers`.

Do not move or rename these. Anything new follows the `src/` + `include/` split.

## EditorID naming convention

Every form, Papyrus script, and ModEvent name we author for this mod uses the prefix **`_ne_`** (short for
"NarrativeEngine"). The leading underscore is deliberate — most CK form lists sort it to the top, which makes our
forms easy to find in long lists. Examples:

- Quest: `_ne_Quest`
- Keywords: `_ne_TravelTarget`, `_ne_SandboxLocation`, `_ne_TaskAssigned`
- Faction: `_ne_TaskFaction`
- AI Packages: `_ne_TravelPackage_Walk`, `_ne_SandboxPackage`, …
- Globals: `_ne_DebugMode`, `_ne_TickIntervalSeconds`
- ReferenceAliases: `_ne_PlayerAlias`, `_ne_AgentAlias00`, `_ne_TargetAlias00`, …
- Papyrus scripts: `_ne_Core`, `_ne_PlayerAlias`, `_ne_Natives`
- SKSE ModEvents: `_ne_Dispatch`, `_ne_Maintenance`

What does *not* take the prefix:

- The plugin file (`NarrativeEngine.esp`) — that's a filename, not an EditorID.
- The SkyrimNet plugin folder + manifest name (`SKSE/Plugins/SkyrimNet/config/plugins/NarrativeEngine/`,
  `plugin.name: NarrativeEngine`) — SkyrimNet's own plugin identifier surface.
- C++ namespaces / classes (`namespace NarrativeEngine`, `class ClosureDeliveryAction`) — these live entirely on
  the C++ side and the form-naming convention doesn't reach them.
- The mod's mod-manager folder (`$SKYRIM_MODS_FOLDER/NarrativeEngine/`) — also a filename.

When adding any new CK form, Papyrus script, or ModEvent, give it the `_ne_` prefix unless one of the above
exceptions applies.

## Building C++ changes

Always build through `build.ps1` at the repo root, invoked via the **PowerShell** tool (not Bash). The
script loads the Visual Studio Developer environment, picks up the user-specific presets from
`CMakeUserPresets.json` (gitignored — preset names `local-debug` / `local-release`), and forwards to
`cmake`.

**CRT linkage: dynamic `/MD`.** SkyrimNet's `PublicAPI.h` documents an ABI requirement: "Both DLLs must use
the same MSVC version and CRT linkage (dynamic `/MD`)." This matters because SkyrimNet's exported APIs (e.g.
`PublicGetRecentEvents`) return `std::string` by value. With `/MD`, every DLL in the process shares one CRT
instance (`ucrtbase.dll`) and therefore one heap, so a buffer SkyrimNet allocated can be freed by our DLL's
destructor without crashing. CMakePresets.json bakes this in via `VCPKG_TARGET_TRIPLET=x64-windows-static-md`
(deps still link statically into our DLL) plus `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded[Debug]DLL`.

**Default preset: `local-release`.** Debug builds still don't work at runtime against the installed SkyrimNet,
but the reason now is **debug vs. release CRT mismatch**, not iterator debug level: `/MDd` debug builds link
against `ucrtbased.dll`, SkyrimNet release links against `ucrtbase.dll`. Different CRTs, different heaps.
So `build.ps1` defaults to release and you should not pass `-Preset local-debug` for everyday testing.

`-Preset local-debug` is still available for the rare case where you want STL asserts / iterator debug
checks on code paths that **don't** touch SkyrimNet. But any test path that triggers a SkyrimNet call will
crash the game in debug.

```sh
pwsh -File build.ps1 build       # incremental release build (auto-configures if needed)
pwsh -File build.ps1 configure   # explicit re-configure
pwsh -File build.ps1 rebuild     # configure + build
pwsh -File build.ps1 clean       # remove build/local-release
pwsh -File build.ps1 build -Preset local-debug   # only when SkyrimNet isn't on the test path
```

After C++ edits, the right workflow is:

1. `pwsh -File build.ps1 build` — verify the build succeeds before considering the change done.
2. On success, the DLL is auto-deployed by the existing CMake post-build step to
   `$SKYRIM_MODS_FOLDER/NarrativeEngine/SKSE/Plugins/NarrativeEngine.dll`.

Notes:

- **Do not invoke `cmake` directly from Bash** — `cl.exe` and Ninja aren't on the Bash `PATH`, and CMake
  won't pick up the user presets without the VS Dev Shell. The script handles all of that.
- First-time configure is slow (vcpkg installs `commonlibsse-ng-fork`, `simpleini`, etc. — ~5 min on a
  fresh checkout). Subsequent builds are seconds.
- A `'vswhere.exe' is not recognized` warning at the top of the dev-shell load output is harmless and
  expected; ignore it.
- `CMakeUserPresets.json` is gitignored on purpose — it pins absolute paths specific to this machine
  (`VCPKG_ROOT`, `SKYRIMNET_DIR`, `SKYRIM_MODS_FOLDER`). Don't try to commit it.
- A clean rebuild is rarely needed. Reach for `clean` only when CMake itself is confused (e.g., after
  changing presets, the toolchain, or `CMakeLists.txt` in ways that affect cache validity).
- Release builds still produce `.pdb` files (CMake's release config emits them), so Visual Studio's
  debugger can attach to a release build with mostly-meaningful stack traces. You lose some local-variable
  visibility to optimization, but it's enough for the rare interactive-debug session.

## Using the `statics/` folder

`statics/` is a verbatim deploy tree — its layout mirrors the runtime mod folder, and the CMake post-build step
copies every file under it into `$SKYRIM_MODS_FOLDER/NarrativeEngine/` preserving relative paths (see
`CMakeLists.txt` around the `STATICS_SOURCE_DIR` block).

### When to put a file in `statics/`

Put a file there if **all** of these are true:

- It is a hand-authored runtime asset that ships with the mod (INI defaults, SkyrimNet plugin manifest /
  prompts / character-bio YAMLs, MCM Helper JSON, PrismaUI views, etc.).
- Its content is static — not generated by the build, not produced by the Creation Kit, not compiled from
  source. Build outputs (the `.dll`, compiled `.pex`, the `.esp`) and CK-authored forms do **not** go here.
- Its final on-disk location under `Data/` is known and stable.

### How to add a file

1. Create the file at the path it should occupy at runtime, rooted at `statics/`. The relative path under
   `statics/` is the relative path under the deployed mod folder — no rewriting at copy time.
   - Example: a settings INI that must end up at `Data/SKSE/Plugins/NarrativeEngine.ini` goes at
     `statics/SKSE/Plugins/NarrativeEngine.ini`.
2. Re-run CMake configure (or a full build via `pwsh -File build.ps1 build`) so the `CONFIGURE_DEPENDS` glob
   picks up the new file. Incremental builds after that will `copy_if_different` it on each build.
3. Do **not** add per-file copy logic to `CMakeLists.txt` — the existing glob handles every file under
   `statics/` uniformly. If you find yourself wanting a special case, reconsider whether the file belongs
   somewhere else (e.g. generated output, Papyrus source which has its own deploy step).

### What not to do

- Don't stage build outputs under `statics/` — the build deploys those itself.
- Don't put planning docs, design notes, or other repo-only files under `statics/` — anything there ships
  to the player's `Data/` folder on every build.
- Don't rename or restructure `statics/` subfolders to differ from the runtime layout; the 1:1 mapping is
  the whole point.

## ESP and Papyrus workflow

CK-authored content (the `.esp`) and Papyrus source (`.psc`) live under `esp/` in the repo. The build deploys
or syncs everything into the MO2 mod folder at `$SKYRIM_MODS_FOLDER/NarrativeEngine/` so CK, the player's game,
and our tooling all see the same files.

### Repo paths

- `esp/NarrativeEngine.esp` — authoritative repo-side ESP. A version-controlled mirror of what CK edits.
- `esp/Source/Scripts/*.psc` — authoritative Papyrus source. Junctioned (see below) so CK and VS Code edit
  these files directly.
- `NarrativeEngine.ppj.in` (repo root) — template for the Papyrus project file. CMake `configure_file`
  substitutes machine-specific absolute paths into `NarrativeEngine.ppj` (gitignored) at the repo root.
- `setup-mod-folder.ps1` (repo root) — one-time per-machine setup; creates the mod folder and the
  `Source/Scripts/` junction.
- `sync-esp.ps1` (repo root) — mod folder → repo ESP sync, invoked by CMake on every build.

### One-time setup

After cloning, run:

```pwsh
pwsh -File setup-mod-folder.ps1
```

This creates `$SKYRIM_MODS_FOLDER/NarrativeEngine/` if needed, and creates an NTFS directory junction at
`<mod-folder>/Source/Scripts/` pointing at `<repo>/esp/Source/Scripts/`. Junctions don't require admin or
Developer Mode and are transparent to MO2's USVFS. The script is idempotent — safe to re-run.

We chose junctions over file symlinks because junctions are NTFS-native reparse points with a long, boring
track record in Skyrim modding tooling. File symlinks have reported MO2 / CK compatibility quirks; junctions
don't.

### ESP flow (one-direction: mod folder → repo)

The `.esp` only ever flows from the mod folder into the repo. CK edits the file in place (via MO2's
virtualized Data folder, which resolves to the mod folder). On every build, `sync-esp.ps1` compares
timestamps and copies the mod-folder file into the repo if it's newer.

The build never pushes the repo's `.esp` back to the mod folder. In the rare case where the repo has a newer
ESP than the mod folder (e.g. after a `git pull` that includes ESP changes from elsewhere — uncommon in solo
development), you must manually copy `<repo>/esp/NarrativeEngine.esp` → `<mod-folder>/NarrativeEngine.esp`.
The build will not do this for you.

Override the auto-sync via `-DNE_SKIP_ESP_SYNC=ON` on the cmake configure line if you have a specific reason
to bypass it.

### Papyrus flow

`.psc` files live in `esp/Source/Scripts/`. CK and VS Code both edit them via the junction (so a CK
quest-fragment edit lands in the repo immediately). On every build, the CMake `compile_papyrus` target
invokes `PapyrusCompiler.exe` against the project's generated `NarrativeEngine.ppj`, and the `.pex` output
deploys directly into `<mod-folder>/Scripts/`. `.pex` files are never tracked in the repo — they're build
output that lives only in the mod folder.

The Papyrus compile target is **conditional** on `.psc` files existing under `esp/Source/Scripts/`. Until
the first `.psc` is authored, the step is dormant — no compiler invocation, no `PAPYRUS_COMPILER` env-var
requirement.

### Required env vars (when Papyrus is active)

- `PAPYRUS_COMPILER` — absolute path to `PapyrusCompiler.exe`, typically `<CK_DIR>/Papyrus Compiler/PapyrusCompiler.exe`.
- `NE_PAPYRUS_IMPORT_SKYRIM` — vanilla Skyrim Papyrus source folder. Defaults to `$SKYRIM_FOLDER/Data/Source/Scripts`
  when `SKYRIM_FOLDER` is set.
- `NE_PAPYRUS_IMPORT_SKSE` — SKSE Papyrus source folder (no auto-detection; ships in the SKSE archive).

These are only checked when `.psc` files are present, so they aren't needed for the initial Step 1 / Step 2
setup.

### VS Code Papyrus extension

The generated `NarrativeEngine.ppj` at the repo root is what the VS Code Papyrus extension uses to discover
source folders, output folders, and imports. Run a CMake configure (`pwsh -File build.ps1 configure`) at
least once so the `.ppj` is generated before opening VS Code; from then on the extension auto-discovers it.

## Writing SkyrimNet `.prompt` files

The `.prompt` files we ship under `statics/SKSE/Plugins/SkyrimNet/prompts/` are Jinja templates that render to
Markdown chat messages sent to an LLM. They use SkyrimNet's `[ system ] ... [ end system ]` /
`[ user ] ... [ end user ]` section markers and follow specific conventions about what to tell the LLM (and
what to deliberately hide — e.g. the cadence at which the call fires). When authoring or editing one, read and
follow [`docs/CUSTOM_PROMPTS.md`](docs/CUSTOM_PROMPTS.md).

## Markdown conventions

These rules apply to **every** markdown file in this repository.

### Filename casing: SCREAMING_SNAKE_CASE

All markdown files use SCREAMING_SNAKE_CASE (a.k.a. MACRO_CASE / CONSTANT_CASE) for their basenames — e.g.
`REPO_MAP.md`, `SKYRIMNET_PLUGIN_CONTRACT.md`, `PATTERNS_AND_LESSONS.md`. Words are uppercase and separated by
underscores; no hyphens, no spaces, no lowercase. This includes `README.md` and `CLAUDE.md` (already conformant as
single-word all-caps).

When creating a new markdown file or renaming an existing one, use this convention. When updating links to a renamed
file, search the whole repo (e.g. `grep -r '](old-name.md)'`) to catch every reference.

### Lint every edit with `markdownlint --fix`

After creating or editing any markdown file, run `markdownlint --fix` against it and resolve any remaining issues
before considering the change done. **Always pass `--fix` by default** — it auto-corrects most formatting issues in
place (blank lines around headings/lists/fences, list-item style, trailing whitespace, etc.) so you only have to
hand-fix what the linter can't.

```sh
markdownlint --fix path/to/FILE.md
```

If `markdownlint --fix` reports no remaining output and exits 0, the file is clean. `markdownlint` is available on
this machine (verified v0.48.0). If a rule consistently fires for a stylistic choice the project wants to keep, the
right move is to update [`.markdownlint.json`](.markdownlint.json) at the repo root rather than ignore the warning ad
hoc. The current config sets `MD013` (line-length) to 120 with exemptions for code blocks, tables, headings, and
unbreakable lines (e.g. long URLs), relaxes `MD024` (no-duplicate-heading) to `siblings_only` so repeated subheadings
under different parents are allowed, and disables `MD060` (table-column-style); everything else is the default rule
set.
