# IntelEngine — Deployment Layout

> **Reading discipline.** This describes the shape of **IntelEngine's** `Data/` payload. NarrativeEngine's
> deployment layout follows whatever NarrativeEngine actually ships — which is much smaller than IntelEngine's
> for Phase 1 (no React dashboard build, no faction-config YAMLs, …). Use this file to see what *categories* of
> file a SkyrimNet plugin can ship and which tool reads each one; don't read it as a checklist of artifacts we
> should produce. See [`README.md`](README.md) for the full discipline.

What ships to the player's `Data/` folder for IntelEngine, by file type and tool that consumes it.

---

## Two-repo source-of-truth split

IntelEngine separates **build inputs** from **shipped artifacts** at the repo level:

- **`IntelEngine-NativePlugin/`** — C++ source, Papyrus `.psc` source, React dashboard source, build
  scripts, CK design docs. *Modifiable by hand: yes — primary work area.*
- **`IntelEngine-GamePlugin/`** — The `Data/` payload: `.esp`, compiled `.pex`, compiled `.dll`, MCM JSON,
  built dashboard, SkyrimNet asset YAMLs/prompts, FOMOD. *Modifiable by hand: no — generated/deployed by
  `build.ps1`.*

`build.ps1` in NativePlugin compiles each layer and copies output into GamePlugin's tree. The GamePlugin tree mirrors
what gets zipped into a release for users.

---

## The `Data/` payload, by tool

Everything below is **relative to the Skyrim `Data/` folder** when installed:

### `IntelEngine.esp` (root of Data)

The plugin record file, authored in Creation Kit. Contains the quest, aliases, packages, keywords, globals, and faction
— see [`ESP_STRUCTURE.md`](ESP_STRUCTURE.md).

### `Scripts/*.pex` (compiled Papyrus, consumed by Skyrim's Papyrus VM)

One per Papyrus script + the CK-generated quest fragment:

- `IntelEngine.pex` — the native API surface (Hidden global script)
- `IntelEngine_Core.pex`, `IntelEngine_Travel.pex`, etc. — one per subsystem
- `QF_IntelEngine_02000D61.pex` — auto-generated quest fragment

### `Source/Scripts/*.psc` (Papyrus source, optional in shipped mods but included here)

Mirrors the `.pex` set. Ships so users can recompile / inspect.

### `SKSE/Plugins/IntelEngine.dll` (consumed by SKSE)

The compiled SKSE native plugin. Loaded at SKSE startup if the build is compatible.

### `SKSE/Plugins/SkyrimNet/...` (consumed by SkyrimNet)

The SkyrimNet plugin extension surface — see [`SKYRIMNET_PLUGIN_CONTRACT.md`](SKYRIMNET_PLUGIN_CONTRACT.md). This is
data: YAML action definitions, manifest, prompt templates. SkyrimNet scans the tree, so the plugin doesn't have to
register them in code.

### `Interface/MCMHelper/IntelEngine/config.json` (consumed by MCM Helper / SkyUI)

Declarative SkyUI MCM page layout. MCM Helper renders this without the plugin author having to write the equivalent
Papyrus MCM code by hand.

### `Interface/Translations/IntelEngine_ENGLISH.txt` (consumed by SkyUI / MCM Helper)

String table for MCM display labels — keeps the JSON layout language-neutral.

### `PrismaUI/views/IntelEngine/dashboard/` (consumed by PrismaUI)

The built React dashboard:

- `index.html` — entry HTML PrismaUI loads in its web overlay
- `dashboard.js` — Webpack 5 bundle
- `dashboard.css` — Tailwind + custom styles
- `dashboard.js.LICENSE.txt` — license attributions

PrismaUI itself is an optional SKSE plugin; IntelEngine's SKSE side soft-links it and degrades gracefully if absent.

### `fomod/` (consumed by FOMOD-aware mod managers: Mod Organizer 2, Vortex)

The installer config:

- `ModuleConfig.xml` — the install steps. IntelEngine's is single-step + single-required-group (everything ships together)
- `info.xml` — installer metadata
- `screenshot.png` — installer thumbnail

The FOMOD declares `requiredInstallFiles` (the `.esp`, `Scripts/`, `Interface/`, `SKSE/`, `PrismaUI/`) and one "Install"
group typed `Required`.

---

## What's *not* in the deployment

For clarity, these belong only in `IntelEngine-NativePlugin/`, not the deployed artifact:

- `SKSE/src/*.cpp` and `.h` — C++ source
- `SKSE/CMakeLists.txt`, `SKSE/vcpkg.json` — build config
- `web/dashboard/src/` — React source (only the `dist/` build output is deployed)
- `docs/` — author design notes
- `build.ps1`, `verify.ps1` — build tooling

---

## Implications for NarrativeEngine

If NarrativeEngine follows IntelEngine's shape, its release artifact will need:

1. **Its own `<Name>.esp`** with whatever Creation Kit forms its design requires (quest + aliases + packages +
   keywords + globals + faction marker, by analogy).
2. **Compiled Papyrus `.pex`** for any scripts it defines.
3. **An SKSE DLL** at `SKSE/Plugins/<Name>.dll` if it needs native code.
4. **A SkyrimNet plugin asset tree** at `SKSE/Plugins/SkyrimNet/` with its actions, manifest, prompts, and submodules —
   but only the additions/overrides it provides (these merge with other installed plugins' trees).
5. **An MCM Helper JSON** if it wants a SkyUI MCM page.
6. **(Optional)** A PrismaUI view if it provides a dashboard.
7. **A FOMOD** if it needs install-time choices, or a flat archive if not.

The two-repo split (build inputs vs. shipped tree) is also worth copying — it makes builds reproducible and keeps
generated binaries from polluting source diffs.
