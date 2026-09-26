# Development

Everything a contributor needs to build, format, deploy, and reason about NarrativeEngine's prior-art reference. The
top-level [`README.md`](../README.md) covers the mod's purpose and its relationship to SkyrimNet; this file covers how
work happens.

## Prior art reference: IntelEngine

The creator of another SkyrimNet plugin called **IntelEngine** stopped developing it and released the source as open
source. It is split across two local repos:

- `C:\Projects\IntelEngine-NativePlugin\` — SKSE source tree (C++ + Papyrus + dashboard source + CK design docs)
- `C:\Projects\IntelEngine-GamePlugin\` — deployment / `Data/` payload (compiled `.esp`, `.dll`, `.pex`, SkyrimNet
  asset YAMLs/prompts, MCM Helper JSON, PrismaUI views, FOMOD)

Both are **read-only reference** for NarrativeEngine work.

### Detailed notes live in `docs/prior-art/`

Everything we've learned about IntelEngine is organized in [`docs/prior-art/`](prior-art/README.md). It is a
**lookup index, not a checklist** — consult it when a specific NarrativeEngine design question is in front of you
and you want to see whether the IntelEngine author solved a similar one.

[`docs/prior-art/REPO_MAP.md`](prior-art/REPO_MAP.md) — Where in the IntelEngine repos each subsystem,
asset, and file lives.

[`docs/prior-art/FEATURE_OVERVIEW.md`](prior-art/FEATURE_OVERVIEW.md) — What IntelEngine does at the
gameplay level (feature list, pitch).

[`docs/prior-art/ARCHITECTURE.md`](prior-art/ARCHITECTURE.md) — Full preserved architecture analysis:
layers, every C++ module, every Papyrus script, data flow, threading, persistence, dependency graph.

[`docs/prior-art/SKYRIMNET_PLUGIN_CONTRACT.md`](prior-art/SKYRIMNET_PLUGIN_CONTRACT.md) — **The SkyrimNet
plugin extension contract** — actions, categories, manifest/variants/schema, prompts, character-bio
submodules, decorators, ModEvents, StorageUtil namespace.

[`docs/prior-art/ESP_STRUCTURE.md`](prior-art/ESP_STRUCTURE.md) — `.esp` / Creation Kit setup: quest,
alias slot pattern, AI packages (speed variants + linked-ref keying), keywords, globals, TaskFaction, package
priorities.

[`docs/prior-art/DEPLOYMENT_LAYOUT.md`](prior-art/DEPLOYMENT_LAYOUT.md) — What ships in the `Data/`
folder and which tool consumes each file.

[`docs/prior-art/PATTERNS_AND_LESSONS.md`](prior-art/PATTERNS_AND_LESSONS.md) — Reusable patterns and
lessons: three-phase async, fuzzy cascade, escalating recovery, soft dependency loading, dispatch ring
buffer, save-scum recovery, single source of truth.

## Working directory conventions

- The repo lives at `C:\Projects\NarrativeEngine\`.
- `docs/prior-art/` is the IntelEngine reference library — extend it (don't rewrite it) if new learnings come in about IntelEngine.
- The IntelEngine repos at `C:\Projects\IntelEngine-NativePlugin\` and `C:\Projects\IntelEngine-GamePlugin\` are
  **read-only reference**.
- `C:\Projects\spriggit-output\` is a Spriggit YAML export of every vanilla Skyrim SE + DLC master — **read-only
  reference**, and the authority on what records exist in the retail game. See
  [`docs/VANILLA_RECORD_REFERENCE.md`](VANILLA_RECORD_REFERENCE.md).

## C++ source layout

These rules apply to **every** C++ source file in this repo:

- `.cpp` files go in `src/`.
- `.h` files go in `include/`.
- Unit tests are **co-located**: the tests for `src/Foo.cpp` go in `src/Foo.test.cpp`, beside the code they
  test. There is no `tests/` tree and there should not be one. See [Unit tests](#unit-tests) below.

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
  `plugin.name: NarrativeEngine`) and the Beta 25 content plugin id (`pdusen.narrative-engine`) — SkyrimNet's own
  plugin identifier surface.
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

## Unit tests

Tests use **Catch2 v3**, pulled in through the same vcpkg manifest as every other dependency, and run under
CTest. Run them with:

```sh
pwsh -File build.ps1 test
```

That builds only the `NarrativeEngineTests` target — no ESP sync, no Papyrus compile, no dashboard bundle —
and then runs `ctest --output-on-failure`. `catch_discover_tests` registers every `TEST_CASE` with CTest
individually, so `ctest -R LLMTextSanitizer` selects one and a failure names the case rather than "the test
binary". You can also run `build/local-release/NarrativeEngineTests.exe` directly for Catch2's own CLI
(`--list-tests`, tag filters like `[LLMTextSanitizer]`, `-s` for passing assertions).

### Two test executables

There are two, because engine-free code and engine-coupled code need different link closures:

| File | Target | Notes |
| --- | --- | --- |
| `src/Foo.cpp` | `NarrativeEngine` (the SKSE DLL) | Picked up by the glob, compiled with `PCH.h`, may touch the engine freely. |
| `src/Foo.test.cpp` | `NarrativeEngineTests` | Pure-core test. Excluded from the DLL glob by an `EXCLUDE REGEX`, so a co-located test can never end up in the shipped plugin. |
| `src/Foo.engine.test.cpp` | `NarrativeEngineEngineMockTests` | Test for code that calls CommonLibSSE. Also matches `*.test.cpp`, so the same exclusion keeps it out of the DLL. |
| Files in `NARRATIVEENGINE_CORE_SOURCES` | `NarrativeEngineCore` (static lib), linked into both the DLL and the core tests | Engine-free production code, compiled once and shared. |
| Files in `NARRATIVEENGINE_MOCKED_SOURCES` | Compiled into `NarrativeEngineEngineMockTests` (and, separately, into the DLL) | Engine-coupled production code under test. |

Adding a test needs no CMake edit — write the file and the glob finds it. Making a *new module* testable
does, and which list you add it to is the decision described in the next two sections.

### Mocking CommonLibSSE

Production code that genuinely calls `RE::` functions can be unit-tested without being reshaped to avoid
them, and without a Skyrim process. `src/EngineUtils.engine.test.cpp` is the worked example.

The mechanism rests on how CommonLibSSE is built. Most of it is headers: struct layouts, member offsets and
inline helpers all compile straight into our object files. Only some functions are out-of-line, and those
live in `CommonLibSSE.lib`, where each one resolves its address inside a running `SkyrimSE.exe` through the
address library. Those out-of-line functions are the only part a test process can't execute — and they are
ordinary symbols, so the linker will take a different definition if one is offered.

So `NarrativeEngineEngineMockTests` compiles production sources against the **real** CommonLibSSE headers,
with the same defines and the same forced `PCH.h` as the DLL, and then **does not link `CommonLibSSE.lib`**.
`testsupport/EngineMock.cpp` defines the engine functions instead. Production code is compiled unmodified:
`EngineUtils.cpp` still calls `RE::Calendar::GetSingleton()`, and reaches our definition rather than
Bethesda's.

Three things make this pleasant to live with:

- **The linker is the checklist.** Any engine function the code under test touches that nobody has mocked is
  an unresolved external, and the build fails naming it. There is no silent fallthrough into real engine
  code and no way to miss a dependency.
- **`REL::Module::mock()`.** CommonLibSSE-NG ships its own unit-testing hook, unlocked by
  `ENABLE_COMMONLIBSSE_TESTING`, which fills in the module singleton so the inline `REL::Module::IsAE()` /
  `IsVR()` branches throughout the headers resolve deterministically. That is what lets a test pick a
  runtime — and therefore reach the SE-vs-VR divergence that `EngineUtils::AddFastTravelEndSink` exists to
  guard against, which otherwise needs a VR install to exercise.
- **Engine singletons are opaque storage.** The mocked accessors answer out of `EngineMock` rather than by
  reading through the pointers they hand back, so the "objects" only need a stable, correctly-aligned,
  suitably-sized address. Nothing has to construct a real `RE::UI`.

`testsupport/CommonLibSSERuntimeStubs.cpp` supplies the rest: `REX::W32` (CommonLibSSE's private Win32
re-declaration) forwards to the real Win32 API, and the `REL` address-library entry points abort with a
message telling you to add a mock. Reaching one of those means production code called an engine function
nothing stands in for.

To put a module under mocked-engine test: add its `.cpp` to `NARRATIVEENGINE_MOCKED_SOURCES`, write
`src/Foo.engine.test.cpp`, build, and add whatever engine functions the linker names to
`testsupport/EngineMock.cpp`.

### Relocated inline functions

The linker is only half the checklist. CommonLibSSE also has functions declared **inline in a header**
whose body resolves an address library id at runtime:

```cpp
inline BSFixedString* ctor8(const char* a_data)
{
    using func_t = decltype(&BSFixedString::ctor8);
    REL::Relocation<func_t> func{ RELOCATION_ID(67819, 69161) };
    return func(this, a_data);
}
```

There is no symbol, so the linker says nothing and the substitution above cannot reach it. It surfaces only
when a test runs. `testsupport/RelocationMocks.cpp` handles this second kind.

`REL::Relocation` resolves an id to `Module::base() + IDDatabase::id2offset(id)`, and three facts make that
interceptable: `Module::mock()` leaves the base at zero; `mapping_t::offset` is a full 64 bits; and
`IDDatabase::_id2offset` is a plain `std::span` that `CommonLibSSERuntimeStubs.cpp` can assign, because it
defines `IDDatabase::load_file` itself and a member definition reaches private members. With a base of zero
an "offset" *is* an absolute address, so the table is literally `{id, &OurFunction}` — no executable memory,
no hand-written thunks. CommonLibSSE's own source notes that "member functions == free functions in x64",
which is why a free function taking the object as its first argument stands in for a relocated member.

Registered today: `BSFixedString`'s constructor and its pool release, and `MemoryManager`'s singleton,
allocate, deallocate and reallocate. Between them those cover string construction and `new` on any engine
type, which is what most code needs. To add one, put the function and **both** of its `RELOCATION_ID` values
(SE and AE) in the table in `RelocationMocks.cpp`.

`id2offset` binary-searches the table and, off VR, does **not** verify the entry it landed on carries the id
it asked for — so an unregistered id would otherwise resolve to a neighbouring stand-in and call it with the
wrong signature. The table closes that by poisoning: every registered id `X` also registers `X-1` pointing at
an abort handler, so any unregistered id is found by a poison entry first and dies with a message naming the
problem. `testsupport/RelocationMocks.test.cpp` is the harness's own self-test.

**Known limits.** Anything that calls a *virtual* on a fabricated engine object needs that object's vtable,
which is a much larger undertaking than mocking a free function — that is the real remaining ceiling.
Interning is not reproduced for strings (two equal `BSFixedString`s get two allocations), which no caller can
observe because nothing compares them by pointer identity.

**When not to reach for this.** Mocking keeps production code unchanged, which is exactly right when the
module's engine coupling *is* its job. It is the wrong tool when the coupling is incidental — see the next
section.

### Testing code that talks to the engine

Most of `src/` can't join the core list as written, because engine access is woven through the logic rather
than sitting at its edges. The way in is to move the engine dependency to the module's boundary, so the
module keeps its behaviour and the caller supplies what only the game can provide. Two shapes cover almost
everything, and `SenderCooldownTable` is the worked example of both:

**Reading ambient engine state → make it a parameter.** The table used to call
`EngineUtils::GetCurrentGameHours()` itself; now `Stamp` and `IsOnCooldown` take a `nowGameHours` argument
and the beats read the clock. No interface, no injection machinery — "what time is it" became an ordinary
value a test passes. Reach for this whenever the module only *reads* something (the clock, a setting, a
player position): gather it at the call site, pass it in.

**Calling an engine service → define a narrow port.** Some dependencies can't be reduced to a value, because
the behaviour worth testing *is* the conversation. Co-save serialization is the clear case: what matters is
how many bytes came back, what happens when a record is truncated mid-entry, and what becomes of a FormID
whose plugin has left the load order. For those, declare an interface naming only the operations the module
actually uses — `include/CosaveIO.h` is three methods — and let the DLL bind it to the real API
(`src/SKSECosaveIO.cpp`) while tests bind it to a fake.

Keep the production adapter a straight transcription with no branching of its own. It is the one part that
unit tests can't reach, so anything that could be wrong belongs on the other side of the port. If an adapter
starts growing decisions, that's the signal the port is drawn in the wrong place.

What this does **not** justify is inventing a port for everything. A module that merely reads engine data to
decide something is better served by gathering a plain snapshot and testing the decision — cheaper, and it
leaves no interface to maintain. Ports are for behaviour you have to stand in for, not for data you can
simply hand over.

### Test style

Tests are organised as nested `TEST_CASE` / `SECTION` blocks, read like `describe` / `it`. Catch2 re-runs
everything above a `SECTION` for each leaf path beneath it, so setup written at the top of a `TEST_CASE`
behaves like a `beforeEach` and each expectation gets fresh state. Nested sections name contextual
conditions (`"when the response is malformed UTF-8"`, `"and a stray continuation byte appears on its own"`);
leaf sections name one observable behaviour (`"should drop the orphan byte and keep the rest"`). A failure
prints the whole path, which is what makes the naming worth the trouble.

Use ordinary values and RAII rather than mocks. Objects constructed in a section are destroyed when that
section's path finishes; there is no reset hook to write and none is wanted.

`src/LLMTextSanitizer.test.cpp` is the worked example of all of the above.

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

- `esp/plugin/` — authoritative repo-side ESP as a [Spriggit](https://github.com/Mutagen-Modding/Spriggit)-serialized
  YAML tree. What CK writes to `NarrativeEngine.esp` gets serialized here; what's committed here gets
  deserialized back to the mod folder. The binary `esp/NarrativeEngine.esp` is **not** tracked.
- `esp/.sync-state.json` — machine-local sync baseline (SHA-256 of the mod-folder ESP and the plugin
  tree at the last successful sync). Gitignored; written by `sync-esp.ps1`.
- `.spriggit` (repo root) — Spriggit config file identifying the package (`Spriggit.Yaml`), the game
  release (`SkyrimSE`), and the Spriggit version to use.
- `esp/Source/Scripts/*.psc` — authoritative Papyrus source. Junctioned (see below) so CK and VS Code edit
  these files directly.
- `NarrativeEngine.ppj.in` (repo root) — template for the Papyrus project file. CMake `configure_file`
  substitutes machine-specific absolute paths into `NarrativeEngine.ppj` (gitignored) at the repo root.
- `setup-mod-folder.ps1` (repo root) — one-time per-machine setup; creates the mod folder and the
  `Source/Scripts/` junction.
- `sync-esp.ps1` (repo root) — bidirectional ESP sync via Spriggit serialize/deserialize, invoked by
  CMake on every build and by the pre-commit hook.

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

### ESP flow (bidirectional, content-hash driven)

The `.esp` exists in two representations: `<repo>/esp/plugin/` (a Spriggit-serialized YAML tree,
version-controlled) and `<mod-folder>/NarrativeEngine.esp` (the binary CK edits and Skyrim loads). On
every build, `sync-esp.ps1` reconciles them via the [Spriggit CLI](https://github.com/Mutagen-Modding/Spriggit):

- mod → repo uses `spriggit serialize -i <mod-esp> -o esp/plugin`
- repo → mod uses `spriggit deserialize -i esp/plugin -o <mod-esp>`

Because serialize/deserialize is not a byte-preserving round-trip, we can't use mtime comparison to
decide which side is "newer." Instead, each successful sync records SHA-256s of both sides in
`esp/.sync-state.json` (machine-local, gitignored) and later runs compare current hashes to that
baseline to see which side has drifted:

- Only the mod-folder ESP hash changed → CK edits; serialize mod → repo.
- Only the plugin tree hash changed → git pull or manual edit; deserialize repo → mod.
- Both changed → divergence; the script refuses to auto-resolve. Re-run with `-Prefer mod` (keep the
  CK edits, overwrite the repo) or `-Prefer repo` (keep the committed tree, overwrite the mod folder).

On a fresh clone (no `.sync-state.json` yet), the script bootstraps by whichever side actually exists;
if both exist without a baseline, it tiebreaks on mtime (mod-ESP vs. newest file under `esp/plugin/`)
the way the old script did, or you can pass `-Prefer` explicitly.

Prerequisite: `Spriggit.CLI.exe` on `PATH` (or point `$env:SPRIGGIT_CLI` at its full path). Install
from [github.com/Mutagen-Modding/Spriggit](https://github.com/Mutagen-Modding/Spriggit/releases).

Override the auto-sync via `-DNE_SKIP_ESP_SYNC=ON` on the cmake configure line if you have a specific reason
to bypass it (e.g. you're hand-editing one side and don't want the build clobbering the other).

### Pre-commit hook

`setup-mod-folder.ps1` installs `.git/hooks/pre-commit`. The hook runs two stages before every commit:

1. **ESP sync.** Snapshot the current on-disk hash of `esp/plugin/`, run `sync-esp.ps1`, then re-hash.
   If the sync pulled in a CK edit from the mod folder (serialize mod → repo produced new YAML), `git
   add` the updated `esp/plugin/` tree so those changes ride along in the same commit. If
   `sync-esp.ps1` fails for any reason, the commit aborts.
2. **Formatters / linters.** Invoke `pre-commit run --hook-stage pre-commit`, which executes every hook in
   [`.pre-commit-config.yaml`](../.pre-commit-config.yaml) against the staged files (clang-format for C++,
   markdownlint for Markdown, gersemi for CMake, prettier for YAML/JSON, PSScriptAnalyzer for PowerShell,
   plus generic whitespace/EOL hygiene). Any hook failure aborts the commit; run `pwsh -File format.ps1`
   to auto-fix, re-stage, and retry. Skipped silently if `pre-commit` isn't on `PATH` so a fresh clone
   can commit before tool setup is complete — see [Linting and autoformatting](#linting-and-autoformatting).

What this means in practice: you can `git commit -m "..."` immediately after a CK session and the latest
ESP state is guaranteed to be part of the commit, without remembering to sync first — and the diff is
guaranteed to be formatted the way the project expects.

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

## Linting and autoformatting

Every text filetype in this repo has an autoformatter and/or linter attached to it, orchestrated by
[pre-commit](https://pre-commit.com/) via [`.pre-commit-config.yaml`](../.pre-commit-config.yaml). The full
matrix:

| Filetype                     | Tool                                              | Config                                                    |
| ---------------------------- | ------------------------------------------------- | --------------------------------------------------------- |
| C / C++ (`.cpp`, `.h`, …)    | `clang-format`                                    | [`.clang-format`](../.clang-format)                       |
| Markdown (`.md`)             | `markdownlint --fix`                              | [`.markdownlint.json`](../.markdownlint.json)             |
| CMake (`CMakeLists.txt`, `.cmake`) | `gersemi`                                   | (defaults)                                                |
| YAML / JSON                  | `prettier`                                        | (defaults; `package-lock.json` and `CMakeUserPresets.json` excluded) |
| PowerShell (`.ps1`, `.psm1`) | `Invoke-Formatter` + `Invoke-ScriptAnalyzer`      | [`scripts/lint-powershell.ps1`](../scripts/lint-powershell.ps1) |
| Whitespace / EOL / large files | `pre-commit-hooks`                              | (defaults; `.ps1`/`.bat`/`.cmd` keep CRLF)                |
| Papyrus (`.psc`)             | *no formatter — PapyrusCompiler acts as lint at build time* | —                                             |

The runtime editor discipline is codified in [`.editorconfig`](../.editorconfig), which any modern editor
picks up automatically.

### One-time tool setup

The hooks shell out to a mix of native tools (`clang-format`) and package-manager-installed tools
(`markdownlint-cli` from npm, `pre-commit`/`gersemi` from pip, `PSScriptAnalyzer` from PSGallery). Install
them once per development machine:

```pwsh
# 1. Python + pre-commit (drives the whole thing). Python 3.10+ recommended.
pip install --user pre-commit gersemi

# 2. Node.js + markdownlint-cli + prettier.
npm install -g markdownlint-cli prettier

# 3. clang-format. Any modern LLVM release works; the config targets clang-format 19.
#    Options: `winget install LLVM.LLVM`, `choco install llvm`, or bundled with
#    Visual Studio's C++ workload (`clang-format.exe` under
#    `%ProgramFiles%\Microsoft Visual Studio\<year>\<edition>\VC\Tools\Llvm\bin`).

# 4. PowerShell module for the PowerShell hook.
Install-Module PSScriptAnalyzer -Scope CurrentUser
```

Verify everything resolves:

```pwsh
pre-commit --version
clang-format --version
markdownlint --version
prettier --version
gersemi --version
Get-Module -ListAvailable PSScriptAnalyzer
```

On the **first** invocation of `pre-commit run` (either via the git hook or `format.ps1`), pre-commit
downloads and caches the pinned versions of each hook repo into `~/.cache/pre-commit`. This takes 1–2
minutes; subsequent runs are seconds.

### Running formatters on demand

```pwsh
pwsh -File format.ps1                      # every hook against every tracked file (rewrites in place)
pwsh -File format.ps1 -Staged              # only files currently `git add`ed
pwsh -File format.ps1 -Hooks clang-format  # only the clang-format hook
```

`format.ps1` is a thin wrapper around `pre-commit run`; use it whenever you want to reformat without
committing, or to re-run a single hook after adjusting its config.

### Do NOT run `pre-commit install`

The `.git/hooks/pre-commit` file is hand-managed by `setup-mod-folder.ps1` because it also has to run
`sync-esp.ps1` (see [Pre-commit hook](#pre-commit-hook)). Running `pre-commit install` will overwrite it
with a stock pre-commit-framework hook and break the ESP sync. If it happens by accident, re-run
`pwsh -File setup-mod-folder.ps1` to restore the correct hook.

### Papyrus is not autoformatted

There is no maintained autoformatter for `.psc` source. The closest lint we have is the PapyrusCompiler
run that `pwsh -File build.ps1 build` triggers — syntax and type errors surface there. If a Papyrus
formatter appears in the ecosystem, add its hook to `.pre-commit-config.yaml` under the existing
`local` block or as its own repo entry.

## Writing SkyrimNet `.prompt` files

The `.prompt` files we ship under `statics/SKSE/Plugins/SkyrimNet/prompts/` are Jinja templates that render to
Markdown chat messages sent to an LLM. They use SkyrimNet's `[ system ] ... [ end system ]` /
`[ user ] ... [ end user ]` section markers and follow specific conventions about what to tell the LLM (and
what to deliberately hide — e.g. the cadence at which the call fires). When authoring or editing one, read and
follow [`docs/CUSTOM_PROMPTS.md`](CUSTOM_PROMPTS.md).

The build deploys every prompt twice: to the loose `SkyrimNet/prompts/` folder that SkyrimNet reads before Beta 25
(0.25.0), and to `SkyrimNet/external/pdusen.narrative-engine/prompts/`, the plugin folder Beta 25 reads instead.
The settings schema, `SkyrimNet/config/plugins/NarrativeEngine/manifest.yaml`, is deployed a second time as
`external/pdusen.narrative-engine/settings/NarrativeEngine.yaml`, which Beta 25 prefers over the old path. Keep one
copy in the source tree, at the old paths; `CMakeLists.txt` makes the second. The plugin folder's `manifest.json`
lives in `statics/` like any other file, and the configure step fails if its `id` differs from its folder name or its
`version` differs from the one in `manifest.yaml`.

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

After creating or editing any markdown file, run markdownlint against it and resolve any remaining issues before
considering the change done. The pre-commit hook enforces this at commit time (see
[Linting and autoformatting](#linting-and-autoformatting)), but the fastest local loop is:

```sh
pwsh -File format.ps1 -Hooks markdownlint    # all markdown files, auto-fix
markdownlint --fix path/to/FILE.md           # single-file, if the tool is on PATH directly
```

Always pass `--fix` by default — it auto-corrects most formatting issues in place (blank lines around
headings/lists/fences, list-item style, trailing whitespace, etc.) so you only have to hand-fix what the linter can't.
If a rule consistently fires for a stylistic choice the project wants to keep, update
[`.markdownlint.json`](../.markdownlint.json) rather than ignore the warning ad hoc. The current config sets `MD013`
(line-length) to 120 with exemptions for code blocks, tables, headings, and unbreakable lines (e.g. long URLs),
relaxes `MD024` (no-duplicate-heading) to `siblings_only` so repeated subheadings under different parents are allowed,
and disables `MD060` (table-column-style); everything else is the default rule set.
