# IntelEngine — Repo Map

> **Reading discipline.** Pure lookup table for **"where in the IntelEngine repos do I find X?"** Open it when you
> already know what IntelEngine subsystem or file you want to inspect for one specific question. It implies
> nothing about what NarrativeEngine should contain. See [`README.md`](README.md) for the full discipline.

IntelEngine's source was open-sourced as two separate repos. This document is the lookup table for **"where in the
IntelEngine repos do I find X?"**

---

## `C:\Projects\IntelEngine-NativePlugin\` — SKSE source tree

The C++ SKSE plugin source and the Papyrus source-of-truth. This is the **build** side.

```text
IntelEngine-NativePlugin/
├── README.md                # User-facing pitch (identical copy in GamePlugin)
├── ARCHITECTURE.md          # The detailed architecture analysis (preserved in architecture.md here)
├── LICENSE, NOTICE          # Open-source licensing
├── build.ps1                # Full build & deploy script
├── verify.ps1               # Deployment verification
├── SKSE/
│   ├── src/                 # C++ source (the native DLL)
│   ├── CMakeLists.txt       # Build configuration
│   ├── vcpkg.json           # Package dependencies (commonlibsse-ng, fmt, rapidfuzz,
│   │                        #   nlohmann-json, sqlite3)
│   └── Plugins/SkyrimNet/   # SkyrimNet plugin assets (action YAMLs, prompts, manifest,
│                            #   submodules). See skyrimnet-plugin-contract.md.
├── Source/Scripts/          # Papyrus .psc source files
├── Interface/
│   ├── MCMHelper/IntelEngine/config.json   # SkyUI MCM page layout
│   └── Translations/IntelEngine_ENGLISH.txt
├── web/dashboard/           # React 18 + Tailwind dashboard source
│   ├── src/                 # App.jsx + 7 tab components + dashboard.css
│   └── dist/                # Webpack 5 build output (deployed to GamePlugin's PrismaUI views)
└── docs/                    # Author's design docs
    ├── ANTI_HALLUCINATION.md
    ├── COMPREHENSIVE_PLAN.md
    ├── DLL_API.md
    ├── ESP_STRUCTURE.md     # Creation Kit setup (preserved in esp-structure.md here)
    └── PACKAGE_PRIORITIES.md
```

### `SKSE/src/` — C++ source files (one .cpp + .h each unless noted)

- **`Plugin.cpp/h`** — SKSE entry point, lifecycle handlers (kDataLoaded, kNewGame, kPostLoadGame,
  kPreLoadGame), per-save ID generation, script property fixup.
- **`Papyrus.cpp/h`** — The Papyrus↔C++ bridge — ~5000 lines, ~200 native functions, sole interface between
  layers.
- **`NPCIndex.cpp/h`** — Loaded-NPC indexing, search cascade, story candidate selection, archetype
  classification, dispatch history ring buffer.
- **`LocationResolver.cpp/h`** — Named + semantic location resolution, home door access management.
- **`ItemIndex.cpp/h`** — Weapon/armor/misc/spell-tome indexing for quest rewards.
- **`SlotTracker.cpp/h`** — **Authoritative** task state for 5 slots, SKSE co-save serialization.
- **`ActionValidator.cpp/h`** — Pre-dispatch validation (does the destination exist? is the NPC reachable?).
- **`AsyncDispatch.cpp/h`** — Worker-thread manager for the three-phase async pattern.
- **`BattleManager.cpp/h`** — Faction battle spawning, morale, waves, finalization.
- **`FactionPolitics.cpp/h`** — Political simulation singleton.
- **`FactionConfigLoader.cpp/h`** — YAML faction config parser.
- **`PoliticalDB.cpp/h`** — Embedded SQLite for faction politics (per-save isolation).
- **`MemoryDB.cpp/h`** — Thin client over SkyrimNet's memory API.
- **`CellAnalyzer.cpp/h`** — Door/furniture/spatial analysis.
- **`DepartureDetector.cpp/h`** — Tracks whether NPCs actually started moving.
- **`StuckDetector.cpp/h`** — Three-level escalating stuck recovery.
- **`OffScreenTracker.cpp/h`** — Travel-progress tracking in unloaded cells.
- **`ProximityMonitor.cpp/h`** — 150ms worker-thread arrival detection (replaces Papyrus OnUpdate).
- **`DialogueTracker.cpp/h`** — Per-NPC dialogue counting → triggers bio refresh.
- **`QuestStateTracker.h`** (header-only) — Active-quest metadata for decorators.
- **`DashboardUIManager.cpp/h`** — PrismaUI view management.
- **`DashboardConfig.cpp/h`** — Dashboard hotkey/settings YAML.
- **`Settings.cpp/h`** — INI configuration loader.
- **`SkyrimNetAPI.h`** (header-only) — Soft-linked SkyrimNet DLL interface (loaded via GetProcAddress).
- **`PrismaUI_API.h`** (header-only) — Soft-linked PrismaUI DLL interface.
- **`StringUtils.h`** (header-only) — Fuzzy match, Levenshtein, tokenizer, article-stripper.
- **`ProcessUtils.h`** (header-only) — `ForEachLoadedActor()` across all four process list tiers.

### `Source/Scripts/` — Papyrus scripts

- **`IntelEngine.psc`** — Hidden global script declaring all native functions as `Global Native` (the
  Papyrus-side API surface).
- **`IntelEngine_Core.psc`** — Quest backbone: slot allocation, Maintenance(), decorator registration,
  SkyrimNet bio injection.
- **`IntelEngine_Travel.psc`** — Multi-phase travel state machine, linger system, meeting rendezvous.
- **`IntelEngine_NPCTasks.psc`** — FetchNPC, DeliverMessage, SearchForActor, EscortTarget.
- **`IntelEngine_Schedule.psc`** — Up to 10 time-based scheduled tasks, early-departure calculation.
- **`IntelEngine_StoryEngine.psc`** — Story DM + NPC social tick; 10+ story types.
- **`IntelEngine_Politics.psc`** — Faction political tick orchestration.
- **`IntelEngine_Battle.psc`** — Battle spawning, wave management, finalization.
- **`IntelEngine_PlayerAlias.psc`** — Minimal auto-init (replaces SEQ) on first install and every load.
- **`IntelEngine_MCM.psc`** — SkyUI MCM (extends SKI_ConfigBase).

---

## `C:\Projects\IntelEngine-GamePlugin\` — Deployment / `Data/` payload

This is what ships to the player's Skyrim `Data/` folder. Compiled artifacts plus game-data assets — **no C++
source**, but the Papyrus sources are mirrored here too.

```text
IntelEngine-GamePlugin/
├── README.md, CHANGELOG.md  # User-facing
├── IntelEngine.esp          # The plugin record file (binary, authored in Creation Kit)
├── IntelEngine.esp.bak      # Backup
├── Scripts/                 # Compiled Papyrus
│   ├── IntelEngine.pex
│   ├── IntelEngine_<Subsystem>.pex   # One per .psc above
│   └── QF_IntelEngine_02000D61.pex   # Creation Kit-generated quest fragment for the quest record
├── Source/Scripts/          # Papyrus sources (mirrors NativePlugin's set + QF fragment .psc)
├── Interface/MCMHelper/IntelEngine/
│   └── config.json          # SkyUI MCM page layout (consumed by MCM Helper)
├── PrismaUI/views/IntelEngine/
│   └── dashboard/           # Built React dashboard (index.html, dashboard.js, dashboard.css)
├── SKSE/Plugins/
│   ├── IntelEngine.dll      # Compiled SKSE plugin
│   └── SkyrimNet/           # SkyrimNet plugin assets — the extension contract
│       ├── config/actions/  # 13 action YAMLs + 3 category YAMLs
│       ├── config/plugins/IntelEngine/   # manifest.yaml, settings.sample.yaml, factions.sample.yaml
│       └── prompts/
│           ├── intel_story_dm.prompt
│           ├── intel_story_npc_dm.prompt
│           ├── intel_political_dm.prompt
│           ├── intel_schedule_safety_net.prompt
│           └── submodules/character_bio/
│               └── NNNN_intel_<name>.prompt   # 7 numbered Jinja2 bio fragments
└── fomod/                   # FOMOD installer
    ├── info.xml
    ├── ModuleConfig.xml
    └── screenshot.png
```

### Quick "where does X live?" cheatsheet

- **SkyrimNet action YAMLs (LLM-facing tool definitions)** →
  `IntelEngine-GamePlugin/SKSE/Plugins/SkyrimNet/config/actions/`
- **Plugin manifest (variants, schema, MCM-like fields)** →
  `IntelEngine-GamePlugin/SKSE/Plugins/SkyrimNet/config/plugins/IntelEngine/manifest.yaml`
- **Top-level "DM" prompts (story, political, social, safety-net)** →
  `IntelEngine-GamePlugin/SKSE/Plugins/SkyrimNet/prompts/*.prompt`
- **Per-NPC bio submodules (the runtime context the LLM sees)** →
  `IntelEngine-GamePlugin/SKSE/Plugins/SkyrimNet/prompts/submodules/character_bio/`
- **C++ implementation of a Papyrus native** → `IntelEngine-NativePlugin/SKSE/src/Papyrus.cpp` (start there;
  it dispatches to the modules)
- **Papyrus quest/state-machine logic** → `IntelEngine-NativePlugin/Source/Scripts/IntelEngine_*.psc`
- **Creation Kit `.esp` structure (aliases, packages, keywords, globals, factions)** →
  `IntelEngine-NativePlugin/docs/ESP_STRUCTURE.md` — and the preserved copy in `esp-structure.md` here
- **MCM page layout** → `IntelEngine-GamePlugin/Interface/MCMHelper/IntelEngine/config.json`
- **Dashboard React source** → `IntelEngine-NativePlugin/web/dashboard/src/`
- **Built dashboard (what the player loads)** → `IntelEngine-GamePlugin/PrismaUI/views/IntelEngine/dashboard/`
- **Soft-linked SkyrimNet C API surface** → `IntelEngine-NativePlugin/SKSE/src/SkyrimNetAPI.h`
- **FOMOD installer config** → `IntelEngine-GamePlugin/fomod/ModuleConfig.xml`
