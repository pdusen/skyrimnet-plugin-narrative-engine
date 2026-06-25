# NarrativeEngine

A Skyrim mod, in active development. The mod is a **plugin for SkyrimNet** — not a standalone mod.

**SkyrimNet** is itself a Skyrim mod (combining an `.esp` and an SKSE plugin) that puts LLMs in control of various
emergent gameplay systems — most notably ad-hoc dialogue generation for NPCs. SkyrimNet exposes a plugin surface so
other mods can extend its LLM-driven systems.

NarrativeEngine ships both an `.esp` and an SKSE plugin of its own (the same shape as SkyrimNet itself, and the same
shape as the IntelEngine prior-art reference described below).

## Prior art reference: IntelEngine

The creator of another SkyrimNet plugin called **IntelEngine** stopped developing it and released the source as open
source. It is split across two local repos:

- `C:\Projects\IntelEngine-NativePlugin\` — SKSE source tree (C++ + Papyrus + dashboard source + CK design docs)
- `C:\Projects\IntelEngine-GamePlugin\` — deployment / `Data/` payload (compiled `.esp`, `.dll`, `.pex`, SkyrimNet
  asset YAMLs/prompts, MCM Helper JSON, PrismaUI views, FOMOD)

Both are **read-only reference** for NarrativeEngine work.

### Detailed notes live in `docs/prior-art/`

Everything we've learned about IntelEngine is organized in [`docs/prior-art/`](docs/prior-art/README.md). It is a
**lookup index, not a checklist** — consult it when a specific NarrativeEngine design question is in front of you
and you want to see whether the IntelEngine author solved a similar one.

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

## Working directory conventions

- The repo lives at `C:\Projects\NarrativeEngine\`.
- `docs/prior-art/` is the IntelEngine reference library — extend it (don't rewrite it) if new learnings come in about IntelEngine.
- The IntelEngine repos at `C:\Projects\IntelEngine-NativePlugin\` and `C:\Projects\IntelEngine-GamePlugin\` are
  **read-only reference**.

## C++ source layout

These rules apply to **every** C++ source file in this repo:

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

## Environment setup

Configure, build, ESP sync, and Papyrus compile all read paths from environment variables that
point at this machine's vcpkg root, MO2 mods folder, Skyrim install, SkyrimNet location, and a
couple of dependency-specific spots. Set these once per development machine (typically as Windows
user environment variables) before running `setup-mod-folder.ps1` or `build.ps1`.

Any of these can alternatively be pinned per-preset in `CMakeUserPresets.json` (gitignored) —
useful when you want different values per build preset or prefer not to pollute your global
environment.

### Required for every build

- `VCPKG_ROOT` — absolute path to your vcpkg checkout. CMake's toolchain file is resolved as
  `$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake` (see `CMakePresets.json`).
- `SKYRIM_MODS_FOLDER` — absolute path to your MO2 (or Vortex) `mods/` folder. The build deploys
  the compiled DLL, `statics/` payload, dashboard bundle, and `.pex` files under
  `$SKYRIM_MODS_FOLDER/NarrativeEngine/`. `setup-mod-folder.ps1` and `sync-esp.ps1` also read it.
- `PRISMA_UI_INCLUDE` — absolute path to the directory containing `PrismaUI_API.h`. CMake
  `FATAL_ERROR`s if the header isn't found at this path.

### SkyrimNet location (one of)

CMake needs SkyrimNet's `CppAPI/` headers on the include path. It looks in two places, in order:

- `SKYRIMNET_DIR` — explicit absolute path to the SkyrimNet mod folder (the one containing
  `CppAPI/PublicAPI.h`). Takes precedence when set.
- `$SKYRIM_MODS_FOLDER/SkyrimNet/` — automatic fallback when `SKYRIM_MODS_FOLDER` is set and
  SkyrimNet is installed at the standard subpath. No additional env var needed in the common case.

CMake `FATAL_ERROR`s if neither resolves to a valid `CppAPI/` directory.

### Required once `.psc` sources exist

The repo now contains Papyrus sources under `esp/Source/Scripts/`, so these are required at
configure time (not deferred):

- `PAPYRUS_COMPILER` — absolute path to Bethesda's `PapyrusCompiler.exe`, typically
  `<CK_DIR>/Papyrus Compiler/PapyrusCompiler.exe`.
- `NE_PAPYRUS_IMPORT_SKYRIM` — vanilla Skyrim Papyrus source folder. Auto-defaults to
  `$SKYRIM_FOLDER/Data/Source/Scripts` when `SKYRIM_FOLDER` is set, so you can usually leave this
  unset.
- `NE_PAPYRUS_IMPORT_SKSE` — SKSE Papyrus source folder. No auto-detection — ships in the SKSE
  archive and must be pointed at explicitly.

### Optional

- `SKYRIM_FOLDER` — absolute path to your Skyrim Special Edition install. Used (a) as a fallback
  `OUTPUT_FOLDER` when `SKYRIM_MODS_FOLDER` is unset (rare — most contributors are on a mod
  manager) and (b) to auto-default `NE_PAPYRUS_IMPORT_SKYRIM`.

### Verifying your setup

In a fresh PowerShell window after setting the variables:

```pwsh
pwsh -File setup-mod-folder.ps1      # creates mod folder + junction + git pre-commit hook
pwsh -File build.ps1 configure       # confirms CMake can locate vcpkg, SkyrimNet, PrismaUI, the Papyrus compiler, and the imports
```

If `configure` exits 0, every required path resolved correctly. After that,
`pwsh -File build.ps1 build` performs incremental builds.

## Building C++ changes

Always build through `build.ps1` at the repo root, invoked via **PowerShell** (not Bash). The
script loads the Visual Studio Developer environment, picks up the user-specific presets from
`CMakeUserPresets.json` (gitignored — preset names `local-debug` / `local-release`), and forwards to
`cmake`.

**CRT linkage: dynamic `/MD`.** SkyrimNet's `PublicAPI.h` documents an ABI requirement: "Both DLLs must use
the same MSVC version and CRT linkage (dynamic `/MD`)." This matters because SkyrimNet's exported APIs (e.g.
`PublicGetRecentEvents`) return `std::string` by value. With `/MD`, every DLL in the process shares one CRT
instance (`ucrtbase.dll`) and therefore one heap, so a buffer SkyrimNet allocated can be freed by our DLL's
destructor without crashing. CMakePresets.json bakes this in via `VCPKG_TARGET_TRIPLET=x64-windows-static-md`
(deps still link statically into our DLL) plus `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded[Debug]DLL`.

**Default preset: `local-release`.** Debug builds don't work at runtime against the installed SkyrimNet
because of a **debug vs. release CRT mismatch**: `/MDd` debug builds link against `ucrtbased.dll`, SkyrimNet
release links against `ucrtbase.dll`. Different CRTs, different heaps. So `build.ps1` defaults to release and
you should not pass `-Preset local-debug` for everyday testing.

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
- `sync-esp.ps1` (repo root) — bidirectional ESP sync (newest copy wins), invoked by CMake on every build.

### One-time setup

After cloning, run:

```pwsh
pwsh -File setup-mod-folder.ps1
```

This does three things:

- Creates `$SKYRIM_MODS_FOLDER/NarrativeEngine/` if needed.
- Creates an NTFS directory junction at `<mod-folder>/Source/Scripts/` pointing at
  `<repo>/esp/Source/Scripts/`. Junctions don't require admin or Developer Mode and are transparent to MO2's
  USVFS. We chose junctions over file symlinks because junctions are NTFS-native reparse points with a long,
  boring track record in Skyrim modding tooling; file symlinks have reported MO2 / CK compatibility quirks.
- Installs a git pre-commit hook (see [Pre-commit hook](#pre-commit-hook) below).

The script is idempotent — safe to re-run. A second run reports each piece as "already exists" or
"updated" and exits cleanly.

### ESP flow (bidirectional, newest wins)

The `.esp` exists in two locations: `<repo>/esp/NarrativeEngine.esp` (version-controlled) and
`<mod-folder>/NarrativeEngine.esp` (what CK edits and what Skyrim loads). On every build, `sync-esp.ps1`
compares their modification times and copies whichever is newer over the older. Two cases this handles
without any manual intervention:

- After a CK session, the mod-folder copy is newer → it propagates back into the repo so you can commit.
- After a `git pull` that brings in an ESP change from elsewhere, the repo copy is newer → it propagates
  out to the mod folder so Skyrim picks it up on the next launch.

When both copies have equal mtime, nothing happens.

Override the auto-sync via `-DNE_SKIP_ESP_SYNC=ON` on the cmake configure line if you have a specific reason
to bypass it (e.g. you're hand-editing one side and don't want the build clobbering the other).

### Pre-commit hook

`setup-mod-folder.ps1` installs `.git/hooks/pre-commit` that runs `sync-esp.ps1` before every commit and
stages the repo's ESP if the sync updated it. The full behavior:

1. Snapshot the current `git hash-object` of `esp/NarrativeEngine.esp`.
2. Run `sync-esp.ps1`. If it fails for any reason, the commit aborts.
3. Re-hash the ESP. If the hash changed (the sync pulled in a CK edit from the mod folder), `git add` the
   ESP so it rides along in the same commit you were already making.

What this means in practice: you can `git commit -m "..."` immediately after a CK session and the latest
ESP state is guaranteed to be part of the commit, without remembering to sync first.

Operational notes:

- **Detection by marker comment.** The script identifies its own hook via a marker line in the script
  body. If a third-party `pre-commit` hook already exists, the setup script warns and leaves it alone
  rather than clobbering. If the marker is present, the setup script overwrites freely — re-running setup
  is how you pick up any future hook-body changes.
- **LF line endings.** The hook is written with Unix line endings so git-bash on Windows can execute it
  cleanly.
- **Escape hatch.** `git commit --no-verify` bypasses the hook the standard git-wide way. Use it
  intentionally when you want to commit without picking up the latest CK state.

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

`PAPYRUS_COMPILER`, `NE_PAPYRUS_IMPORT_SKYRIM`, and `NE_PAPYRUS_IMPORT_SKSE` are required at
configure time once `.psc` files exist under `esp/Source/Scripts/` (which they now do). See
[Environment setup](#environment-setup) for the full list and their default-resolution rules.

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
