# Phase 01 — MVP

The first shippable cut of NarrativeEngine. Stands up every piece of plumbing the final mod will eventually need,
ships exactly **one** Director-issued effect that the player will notice during a normal play session, and defers
everything else.

---

## MVP concept

A bare-bones AI Director, running for real. It:

- Reads the recent gameplay-event stream from SkyrimNet's own event log on demand (via
  `PublicGetRecentEvents`) when building each evaluation's prompt context — no local event-log subsystem.
- Maintains a persistent **phase tracker** (the current Freytag's-Pyramid phase + how long we've been in it).
- On a real-time interval, builds a snapshot of recent context, sends it to an LLM via SkyrimNet's
  `PublicSendCustomPromptToLLM`, parses the response, and records the decision to a persistent decision log.
- Respects Alpha Canon: never acts during active combat, dialogue, or scripted scenes.
- Has **one** Director-issued in-world effect — see "The one feature" below.
- Pushes a live view of its state to an in-game **PrismaUI dashboard** built in TypeScript + React — see
  "The observability surface" below.
- Exposes a minimal **MCM page** (via MCM Helper) with mod credits in the left column and the dashboard
  hotkey rebinder in the right — see "The MCM" below.

The architecture is open-ended enough that the full mod's later phases (additional actions, background simulations,
suppression directives, dashboard, repetition guardrails) plug in without restructuring. But none of those are in
the MVP.

### The one feature: live narrative-state injection into every NPC's context

On every Director tick the plugin updates two pieces of internal state — a current **tension score** (0–100) and
a current **Freytag phase** (`Exposition` / `RisingAction` / `Climax` / `FallingAction` / `Resolution`). Those two
values are exposed to SkyrimNet through a pair of custom **decorators** (`ne_narrative_tension`,
`ne_narrative_phase`) that the plugin registers at startup via SkyrimNet's `PublicRegisterDecorator` C API.

A small **`system_head` submodule prompt template** ships in our `Data/` payload at
`SKSE/Plugins/SkyrimNet/prompts/submodules/system_head/0155_ne_narrative_state.prompt`. SkyrimNet auto-discovers
any `.prompt` file dropped into that folder and includes it in the head of every LLM prompt — regardless of which
NPC the prompt is being built for. The numeric prefix (`0155`) controls our submodule's order relative to other
system_head fragments.

The template calls the decorators to read the current tension and phase, then renders **one qualitative
sentence** that the LLM can read. Example rendered output:

> ## World mood
>
> The current mood of the world feels noticeably tense — small troubles feel close at hand. Narratively, events
> are clearly building toward something.

The decorators expose raw values; the *interpretation* lives in the prompt template. Mapping a numeric tension
score to a phrase ("calm", "watchful", "charged") rather than emitting the digit is deliberate: LLMs reliably
react to evocative language, and don't reliably translate "tension level 58/100" into shifted dialogue tone.

The effect on the player's experience: NPC dialogue subtly tracks the world's pacing. During Resolution the
village feels calmer; during Climax the same village feels charged. It is not loud — there's no popup, no spawn,
no quest. But on a careful play session, the player will notice that the social texture of the world breathes.

### Why this is the right MVP feature

- **Real end-to-end exercise.** Hits every piece of plumbing — SkyrimNet event read → snapshot → LLM call →
  parsed decision → state update visible to the next NPC prompt SkyrimNet builds.
- **Cannot ruin a play session.** No spawned enemies. No forced events. No vanilla content disturbed.
- **No `.esp` content required.** No quests, aliases, AI packages, keywords, factions, or globals. The
  user-authored `NarrativeEngine.esp` / `.esl` shell can stay empty for MVP.
- **No Papyrus required.** Zero `.psc` files, zero C++ ↔ Papyrus boundary crossings.
- **Pure C++ + one shipped `.prompt` file.** Two `PublicRegisterDecorator` calls at startup; the prompt template
  is a static asset SkyrimNet hot-loads. No per-tick SkyrimNet write API needed — the decorators read live state
  every time SkyrimNet renders a bio.
- **Visible in dialogue,** which is the primary surface SkyrimNet was built for, so the effect will manifest
  wherever the player engages with the mod stack.

### The observability surface: PrismaUI dashboard

In parallel with the in-world feature, the MVP ships a small **in-game debug dashboard** rendered through
[PrismaUI](https://github.com/dec1mo/PrismaUI) — an SKSE plugin that hosts a Chromium-based webview overlay in
Skyrim. The dashboard is implemented in **TypeScript + React**, bundled with **Rollup** into a single self-
contained JS file, and deployed alongside a static HTML/CSS shell to PrismaUI's view directory.

What it shows (live, refreshed every Director tick):

- **Status banner** — SkyrimNet availability + API version, Director enabled/disabled, PrismaUI link state.
- **Phase panel** — current Freytag phase, time-in-phase, last evaluation's tension score.
- **Last evaluation** — narrative note, advanced-to phase (if any), Alpha Canon signals (if any).
- **Recent decisions** — last 10 `DecisionRecord` entries (newest first).
- **Recent events** — last 20 entries from SkyrimNet's event log (newest first), as returned by
  `PublicGetRecentEvents`.

Why a dashboard in the MVP — and why this technology choice:

- **Observability is plumbing.** Every later phase will want to see what the Director is doing. Standing the
  surface up at MVP means later phases just push more data through the same channel.
- **Soft dependency.** PrismaUI is `LoadLibraryA`-soft-linked. If a user doesn't have PrismaUI installed, the
  Director loop still runs end-to-end; only the dashboard is unavailable.
- **TypeScript + React + Rollup.** TypeScript catches the C++ ↔ JS state-shape contract at build time (one
  shared type definition; if C++ changes the schema we notice in the React build). React is overkill for the
  current state size but the component shape will absorb new fields cleanly. Rollup is a small, fast bundler
  with no dev-server / HMR overhead — appropriate for a single static bundle that's loaded once per game session.
- **Read-only by construction.** The MVP dashboard pushes state from C++ → JS only. No JS → C++ callbacks, no
  buttons that mutate Director state. Write-side debug affordances are a later-phase concern.

The dashboard's effect on the player's experience is minimal-and-deliberate: by default it's hidden; the player
presses a hotkey (configurable + supports arbitrary modifier combinations — defaults to `F8`, can be rebound to
anything from `7` to Ctrl+Alt+Shift+`7`) to toggle it. The intent is observability for the developer and for
curious players, not a gameplay-facing UI.

### The MCM

A small SkyUI Mod Configuration Menu page, declared through **MCM Helper** (an SKSE plugin that lets you
describe MCM pages in JSON rather than authoring Papyrus). Layout: one screen, two columns.

- **Left column — Mod credits.** Static text. Mod name and version, one-line pitch ("An AI Director for
  Skyrim, layered on top of SkyrimNet"), author handle, link/URL to the project. Pure information; no
  interactive elements.
- **Right column — Settings.** Initially a single setting: **Dashboard Hotkey**, with a current-binding
  display and a rebind action. The rebind must support arbitrary modifier combinations (Ctrl, Shift, Alt,
  any combination — including all three).

The MCM is intentionally minimal for MVP. Later phases will add more rows to the right column as new
user-tunable settings emerge; the left column stays static credits.

**Tech choice — MCM Helper rather than raw SkyUI MCM:**

- Declarative JSON config (`Data/MCM/Config/NarrativeEngine/config.json`); no Papyrus required from us.
- Built-in keybind option type stores its value in `Data/MCM/Settings/NarrativeEngine.ini` automatically.
- Our C++ plugin reads that INI on `Settings::Load()` and uses the value when registering the dashboard
  hotkey. Rebinds take effect on next save load (acceptable for MVP; live rebind without restart is a
  later-phase concern that would require a ModEvent and the tiny bit of Papyrus we're avoiding).
- Soft dependency. If MCM Helper isn't installed, no MCM page appears; the dashboard hotkey falls back to
  the value in `Data/SKSE/Plugins/NarrativeEngine.ini` (or the baked-in default).

---

## Scope

### In scope

Plumbing layers (each described in detail in the implementation plan below):

1. **SKSE plugin shell** — entry point, message routing, serialization callbacks, logger.
2. **SkyrimNet integration** — soft-link the DLL, call `FindFunctions()`, typed wrappers over the soft-loaded
   function pointers.
3. **Settings** — single INI loader (`Data/SKSE/Plugins/NarrativeEngine.ini`) with sensible defaults if absent.
4. **Phase tracker** — the current Freytag phase + unpaused-real-time spent in it.
5. **Decision log** — a persisted, bounded record of every Director evaluation (the result; the rationale; the
   action chosen, if any). Also doubles as the Director's record of its own past actions — every Beta-canon
   "the Director did X" is captured here (in `actionSelected` + `narrativeNote` + `advancedToPhase`), so no
   separate Beta-canon event store is needed.
6. **Async dispatch** — a worker thread + main-thread marshaling helper.
7. **Tick driver** — the real-time wall-clock interval that fires evaluations.
8. **Evaluation pipeline** — three-phase async: Phase A (main: snapshot game state into a value-only struct,
   pulling recent events from SkyrimNet); Phase B (worker: build prompt context blob); Phase C (worker: dispatch
   async LLM call); Phase D (main: parse response, apply decision, write decision log).
9. **Alpha Canon predicates** — the predicates that gate whether the Director may act.
10. **Co-save persistence** — SKSE serialization callbacks for the decision log and phase tracker. No third
    payload — the MVP feature is a stateless decorator pair backed by the live phase tracker and decision log,
    so it has nothing of its own to persist.
11. **The one feature: narrative-state injection** — two custom SkyrimNet decorators
    (`ne_narrative_tension`, `ne_narrative_phase`) registered at startup, plus a shipped `system_head` submodule
    prompt template (`0155_ne_narrative_state.prompt`) that renders the current Director state into every NPC's
    LLM context as one qualitative sentence.
12. **Director prompt template** — one `.prompt` file at
    `SKSE/Plugins/SkyrimNet/prompts/narrative_engine_story_eval.prompt` instructing the LLM to score tension
    and produce the Director's structured decision.
13. **PrismaUI integration** — `LoadLibraryA`-soft-linked C++ wrapper over PrismaUI's public API. Graceful
    degradation when PrismaUI is uninstalled.
14. **Dashboard build pipeline** — a `dashboard/` directory at the project root with `package.json`,
    `tsconfig.json`, `rollup.config.mjs`, and a React component tree under `dashboard/src/`. `npm run build`
    produces a single bundled `dashboard.js` plus the static `index.html` and `dashboard.css`.
15. **Dashboard runtime wiring** — `DashboardUIManager` C++ module that creates the PrismaUI view at
    `kDataLoaded`, composes a JSON state snapshot, pushes it to JS via PrismaUI's JS-interop after every
    Director `ApplyDecision`, and toggles visibility on a configurable hotkey (full modifier-combo support).
16. **MCM page** — single-screen MCM Helper-driven page with mod credits (left column) and one user setting:
    rebind the dashboard hotkey (right column). MCM Helper is a soft dependency; absent → no MCM, hotkey
    falls back to the plugin INI.

### Out of scope (deferred to later phases)

The full-mod features that are explicitly not in MVP. Plumbing the final mod needs that genuinely depends on these
is also deferred, **on purpose** — designing the framework speculatively without a concrete consumer would just be
guessing.

- **Background simulations** — zero sims; no sim-framework abstraction. When the first sim is designed in a
  future phase, the framework is designed alongside it.
- **Suppression directives** — no sims to suppress, so the directive type, persistence, and dispatch are deferred
  entirely. When sims arrive, suppression is added with them.
- **Additional actions beyond the one above.** No hostile spawning, no NPC dispatch, no courier letters, no
  ambient sound cues, no NPC interactions. All of those will earn their slot in the toolbox individually in later
  phases.
- **MCM beyond a credits + hotkey-rebind page.** The MVP MCM is one screen with one user setting. No
  sub-pages, no per-setting categories, no debug-mode toggle or tuning sliders — those are added in later
  phases as new tunables emerge. The SKSE log
  (`Documents/My Games/Skyrim Special Edition/SKSE/NarrativeEngine.log`) plus the PrismaUI dashboard remain
  the observability path.
- **Dashboard write-side affordances.** The MVP dashboard is **read-only** — pushes state from C++ to JS only.
  No "force phase advance," "fire action now," or other debug-write buttons; those are a later-phase concern.
- **Live hotkey rebind without save reload.** Rebinding in MCM takes effect on the next save load, not
  immediately. Live rebinding requires a ModEvent round-trip (which means a tiny Papyrus script), deferred
  out of MVP to keep the "no Papyrus" property intact.
- **`.esp` content.** Zero new Creation Kit forms. The user's existing shell stays as-is.
- **Papyrus scripts.** Zero `.psc` files. Zero ModEvents. Zero C++ ↔ Papyrus boundary crossings.
- **Repetition guardrails, multi-action composition, phase-duration policy enforcement, memory horizon
  retention, player-agency signals, cross-plugin coordination.** Each becomes interesting only when there's
  enough toolbox or sim activity to need them.

### What MVP "playable" looks like

The user installs the build, boots Skyrim, and plays normally for 30+ minutes. After the play session:

- The log file at `Documents/My Games/Skyrim Special Edition/SKSE/NarrativeEngine.log` contains a sequence of
  Director evaluations — each with the observed tension score, the phase determination, and a one-line
  narrative-note rationale.
- The Director has advanced the phase at least once if the play session contained any major events (a quest
  milestone, a hard fight, an extended quiet stretch).
- During play, dialogue with NPCs subtly reflects the current narrative phase. (Hard to verify objectively
  short of A/B comparison; the log + a debug-render of the assembled SkyrimNet prompt for any NPC, showing the
  `0155_ne_narrative_state.prompt` fragment with the right tension/phase wording, is the proxy verification.)
- The player can press the dashboard hotkey (default `F8`) to bring up the PrismaUI overlay, see the
  current Director state, watch entries appear in the recent-decisions list as ticks fire, and see
  SkyrimNet's recent-events feed refresh in the recent-events panel.

---

## Plumbing decisions and rationale

A short justification for each piece of plumbing — why it's in MVP, what alternative was considered, what we
chose not to do. Every later phase should be able to add features without rewriting any of this.

**Persistent decision log.** Plain C++ `std::deque` ring buffer, serialized via SKSE co-save. No DB needed
for MVP volumes. SKSE co-save survives saves correctly. Adding SQLite later (if a politics sim arrives)
doesn't break this surface.

**No local event log.** SkyrimNet maintains its own event log (combat, dialogue, etc.) and exposes it via
`PublicGetRecentEvents(formId=0, maxCount, eventTypeFilter)`. We query it on demand when building each
evaluation's prompt context — no subscriptions, no ring buffer, no co-save record. Beta-canon ("the Director
did X") is covered by the decision log (`actionSelected`, `narrativeNote`, `advancedToPhase`), so no separate
write surface is needed for MVP. SkyrimNet's public API has no global event-write function anyway — only
per-actor `PublicAddMemory` and condition-gated `PublicAddWorldKnowledge` — so writing back to its event
stream isn't an option even if we wanted it.

**Tick driver.** Dedicated `std::thread` sleeping for `Settings::tickIntervalSeconds`, gated on
`RE::UI::GameIsPaused()`. Real-time, not game-time (per DESIGN_GOALS). Single thread; no scheduler complexity
yet.

**Async dispatch.** One worker thread + `SKSE::GetTaskInterface()->AddTask` for main-thread marshaling.
SKSE's task interface is the canonical way to bounce back to the game thread. The 3-phase async shape
(snapshot → IO → effect) is dictated by SKSE's main-thread invariants for engine APIs, not by any prior art's
pattern.

**LLM call.** `SkyrimNetAPI::SendCustomPromptToLLM` async-with-callback. The callback fires off-thread; we
marshal the parsed decision back to the main thread for application.

**Narrative-state injection mechanism: decorators + system_head submodule prompt.** SkyrimNet exposes two
relevant write surfaces: `PublicAddWorldKnowledge` (for static or condition-gated facts) and `PublicAddMemory`
(per-actor). Neither fits a *global, mutates-every-tick* signal like narrative tension — world knowledge
isn't designed for high-churn entries, and per-actor memories don't represent a global mood. The right
mechanism instead is SkyrimNet's prompt-templating layer: register C++ decorators (`ne_narrative_tension`,
`ne_narrative_phase`) via `PublicRegisterDecorator`, then drop a `.prompt` file into
`SKSE/Plugins/SkyrimNet/prompts/submodules/system_head/` that calls them at render time. The decorators read
live values off `PhaseTracker` and `DecisionLog`; the prompt template maps numeric tension to qualitative
phrasing so the LLM gets evocative language instead of digits.

**No action interface or action registry in MVP.** The "one feature" is not an in-world action that fires and
mutates state — it's a continuously-rendered prompt fragment backed by always-readable plugin state. No
`Dispatch()`, no cooldown, no precondition gate. The abstract `Action` / `ActionRegistry` scaffolding has zero
current consumers, so it's deferred until a real in-world Director action (notification, sound cue, NPC dispatch,
etc.) arrives in a later phase. The interface gets designed *with* its first consumer rather than ahead of it.

**Alpha Canon.** Free functions returning `bool` over engine state; one `EvaluateAll()` aggregator.
Predicates are cheap and pure; no need for a class.

**Settings.** `simpleini` for the INI; one `Config` struct loaded once. Same dep added in the bootstrap
pass. Simple.

**`.esp` content.** None. The MVP action is pure C++ + SkyrimNet API. No forms needed.

**Papyrus.** None. No boundary crossings, no C++/Papyrus debugging, no script compile step.

**Dashboard.** PrismaUI overlay rendered from a TS + React bundle, built by Rollup, pushed-to from C++.
PrismaUI is a soft dependency; if absent, the dashboard simply doesn't appear and the rest of the plugin
works unchanged. React is overkill for the current state size, but the component shape will absorb new
fields cleanly as the toolbox grows. TypeScript catches the C++ ↔ JS schema contract at build time. Rollup
gives a single small bundle with no dev-server overhead.

**MCM.** MCM Helper-driven (declarative JSON), single-page, credits + one keybind. MCM Helper lets us
declare the page in JSON instead of authoring a `SKI_ConfigBase` Papyrus script — preserves the "no Papyrus"
property. Soft dependency: absent → no MCM, hotkey falls back to plugin INI. Modifier-combo support requires
verifying MCM Helper's keybind option type handles modifiers natively; if not, fall back to "main key +
three modifier toggles" presentation.

**Sim framework.** None. Speculative without a concrete sim. When the first sim arrives, the framework is
designed for that sim's actual needs, not from scratch in MVP.

**Suppression.** None. Speculative without sim consumers.

---

## Implementation plan

Sequential. Each step has clear deliverables and a verification that the implementer can run before moving on.
Where I am certain of the specific API surface, I name it; where the exact SkyrimNet/CK behavior needs verifying
in the live game, I say so.

Reference reading for the implementer:

- [`../DESIGN_GOALS.md`](../DESIGN_GOALS.md) — what the Director is and isn't allowed to do.
- SkyrimNet's `PublicAPI.h` at `$SKYRIMNET_DIR/CppAPI/PublicAPI.h` — the actual API surface we will call.
- The prior-art docs under [`../prior-art/`](../prior-art/) are **lookup only** — open them only when a specific
  question is in front of you and you want to see how someone else solved a similar one. Treat their specifics
  with skepticism (the discipline is in [`../../CLAUDE.md`](../../CLAUDE.md)).

Already in place from the project bootstrap (do not redo):

- `CMakeLists.txt`, `CMakePresets.json`, `CMakeUserPresets.json`, `vcpkg.json`, `vcpkg-configuration.json`,
  `PCH.h`, `include/logger.h`, `plugin.cpp` (minimal "Hello, World!"), `build.ps1`.
- vcpkg deps installed: `commonlibsse-ng-fork`, `directxtk`, `simpleini`, `nlohmann-json`.

Each implementation step below names the files it creates. Per CLAUDE.md: `.cpp` files in `src/`, `.h` files in
`include/`, included with angle brackets.

---

### Step 1 — Lifecycle shell and serialization callbacks

- [x] Complete

**Goal:** the plugin loads, registers an SKSE message listener that branches on every lifecycle event we care
about, and registers SKSE co-save serialization callbacks. The handler bodies are wired but do nothing yet
beyond logging — they'll get bodies as later steps add their subsystems.

**Files:**

- `include/Plugin.h` — declares `bool Startup(const SKSE::LoadInterface*)` and a 32-bit `kCoSaveUniqueID`
  constant.
- `src/Plugin.cpp` — implements `Startup`, the `OnMessage` listener (branches on `kDataLoaded`, `kNewGame`,
  `kPreLoadGame`, `kPostLoadGame`), and the three serialization callbacks (`OnSave`, `OnLoad`, `OnRevert`).
- `plugin.cpp` — replace the current minimal entry point with a one-line shim that calls into `Startup`.

**Specifics:**

- Pick a unique 4-byte ID for SKSE co-save (e.g. `'NRTV'`); document it as `kCoSaveUniqueID` in `Plugin.h`. Each
  subsystem will use its own per-payload type ID when calling `intfc->OpenRecord(typeID, version)`.
- `OnMessage` switch arms:
  - `kDataLoaded`: log a marker. Subsystem init goes here as later steps add them.
  - `kNewGame`: log a marker. Subsystem `Clear()` calls go here as added.
  - `kPreLoadGame`: log a marker. Subsystem in-memory reset (pre-deserialize) goes here.
  - `kPostLoadGame`: log a marker. Post-load hookups go here (e.g. starting the tick driver).
- `OnSave` / `OnLoad` / `OnRevert` are empty bodies. Later steps add `<Subsystem>::OnSave(intfc)` calls.
- In `OnLoad`, use `intfc->GetNextRecordInfo(type, version, length)` in a loop to dispatch each payload type to
  the right subsystem's `OnLoad`.

**Verify:** `pwsh -File build.ps1 build` succeeds. Boot Skyrim; the log shows the lifecycle messages firing in
order (`kPostLoad`, `kDataLoaded`, then either `kNewGame` or `kPreLoadGame`/`kPostLoadGame`).

---

### Step 2 — SkyrimNet integration wrapper

- [x] Complete

**Goal:** a typed, defensive C++ wrapper over the soft-loaded SkyrimNet API. Every other module calls into this
wrapper, not directly into `PublicAPI.h`.

**Files:**

- `include/SkyrimNetAPI.h` — declares wrapper functions in a `NarrativeEngine::SkyrimNetAPI` namespace.
- `src/SkyrimNetAPI.cpp` — **the only translation unit that includes `<PublicAPI.h>`.** The header defines its
  function pointers at file scope; multi-TU inclusion would break linkage.

**Specifics:**

- `Initialize()` — calls `::FindFunctions()` exactly once; logs the result (`PublicGetVersion()` value or
  "unavailable"). Returns true on success.
- `IsAvailable()` — returns whether the previous Initialize succeeded.
- `bool SendCustomPromptToLLM(promptName, variant, contextJson, std::function<void(string response, bool success)> callback)`
  — wraps `PublicSendCustomPromptToLLM`. Translates the SkyrimNet callback signature
  (`const char* response, int success`) into a `std::string` + `bool`.
- `std::string GetRecentEvents(uint32_t formId, int maxCount, const std::string& eventTypeFilter)` — wraps
  `PublicGetRecentEvents`. For MVP we always pass `formId=0` (global events) and an empty filter. Returns the
  raw JSON string SkyrimNet produces (an array of event objects with `type`, `text`, `gameTime`,
  `originatingActorName`, `targetActorName`); the snapshot builder hands this through to the prompt context
  verbatim — no need to parse it on our side for MVP.
- `bool RegisterDecorator(const std::string& name, const std::string& description,
  std::function<std::string(RE::Actor*)> callback)` — wraps `PublicRegisterDecorator`. Returns false if the
  underlying pointer is null or registration fails (name collision, etc.). The MVP registers exactly two
  decorators through this wrapper; see Step 12.
- `bool HasDecorator(const std::string& name)` — wraps `PublicHasDecorator`; used by Step 12 to log a warning if
  one of our decorator names collides with a SkyrimNet built-in.
- All wrappers null-check the underlying function pointer before calling and return a sensible default if it's
  null (empty string, `"[]"` for the JSON-returning ones, false, 0).

Call `SkyrimNetAPI::Initialize()` from `kDataLoaded` in `Plugin.cpp`.

**Verify:** Boot Skyrim. The log shows `SkyrimNetAPI: initialized (version=<N>)` where N is whatever
`PublicGetVersion` returns. If SkyrimNet is uninstalled, the log shows an error and `IsAvailable()` returns false
(but the plugin doesn't crash).

---

### Step 3 — PrismaUI integration wrapper

- [x] Complete

**Goal:** a typed, defensive C++ wrapper over the soft-loaded PrismaUI API, mirroring the shape of the
SkyrimNet wrapper. Every other module calls into this wrapper, not into PrismaUI's symbols directly.

**Files:**

- `include/PrismaUI.h` — declares wrapper functions in `NarrativeEngine::PrismaUI_API`. Named `PrismaUI.h`
  (not `PrismaUI_API.h`) so it doesn't shadow the upstream `PrismaUI_API.h` header on the include path.
- `src/PrismaUI.cpp` — soft-links PrismaUI via the upstream `RequestPluginAPI<IVPrismaUI1>()` helper. The only
  TU that includes the upstream `<PrismaUI_API.h>`.

**Build-time integration:** the upstream `PrismaUI_API.h` is not shipped alongside `PrismaUI.dll`. Point the
`PRISMA_UI_INCLUDE` environment variable at the directory containing it; `CMakeLists.txt` adds that directory
to the include path, mirroring how `SKYRIMNET_DIR` is handled. PrismaUI is a *runtime* soft dependency but a
*build-time* hard one — the wrapper TU has to compile against the header, but it degrades gracefully at
runtime when the DLL is absent.

**Specifics:**

- `Initialize()` — calls `PRISMA_UI_API::RequestPluginAPI<IVPrismaUI1>()` once. (Internally this does
  `GetModuleHandle(L"PrismaUI.dll")` + `GetProcAddress("RequestPluginAPI")`; PrismaUI is loaded by SKSE
  earlier so we don't `LoadLibrary` it ourselves.) On success, caches the V1 interface pointer and logs
  `PrismaUI: loaded`. On failure (DLL not loaded or `RequestPluginAPI` returned null), logs
  `PrismaUI: not found; dashboard disabled` at info-level and returns false. **Never fatal** — the Director
  loop runs without it.
- `IsAvailable()` — true after a successful Initialize.
- View management — minimum surface needed by `DashboardUIManager` (Step 16). Each wrapper maps 1:1 to an
  `IVPrismaUI1` virtual method and documents the mapping in a comment so future PrismaUI API drift is easy to
  trace. The wrapped operations:
  - `CreateView(htmlPath) → ViewHandle` (wraps `IVPrismaUI1::CreateView`; `ViewHandle` is a `uint64_t` alias
    over PrismaUI's `PrismaView` so callers don't need to include the upstream header).
  - `Destroy(view)` / `IsValid(view)` — view lifetime.
  - `Show(view)` / `Hide(view)` / `IsHidden(view)` — visibility (the `IsHidden` wrapper returns `true` when
    PrismaUI is unavailable, since a non-existent view is sensibly "hidden" from a toggle's perspective).
  - `InvokeJS(view, functionName, argument)` — wraps `IVPrismaUI1::InteropCall`, the fast path for calling
    `window.<functionName>(argument)` in the view's JS context. Used to push the dashboard's JSON state blob.
- **Hotkey registration is *not* a PrismaUI API.** The plan's earlier draft assumed PrismaUI exposed one;
  the actual `IVPrismaUI1` interface does not. Hotkey handling lives in `DashboardUIManager` (Step 16) via
  SKSE's input-event-sink (`RE::BSTEventSink<RE::InputEvent*>`).
- Every wrapper null-checks the cached interface pointer and returns a safe default (`kInvalidView`, `true`
  for `IsHidden`, no-op otherwise) when PrismaUI is unavailable.

**Verify:** Boot Skyrim with PrismaUI installed. The log shows `PrismaUI: loaded`. Uninstall PrismaUI; the log
shows `PrismaUI: not found; dashboard disabled` and the rest of the plugin loads without issue.

---

### Step 4 — Settings loader

- [x] Complete

**Goal:** an INI loader that reads `Data/SKSE/Plugins/NarrativeEngine.ini` once at `kDataLoaded`. Missing file
or missing keys fall back to defaults baked into the `Config` struct.

**Files:**

- `include/Settings.h` — a `Config` struct (POD) and `Load()` / `Get()` accessors in the
  `NarrativeEngine::Settings` namespace.
- `src/Settings.cpp` — implementation using `<SimpleIni.h>`.

**Specifics — initial keys + defaults:**

- `[General] bDebugMode=0`
- `[Director] iTickIntervalSeconds=90`
- `[Director] iDecisionLogMaxEntries=200`
- `[Director] iDecisionLogTailSizeForPrompt=10`
- `[Director] iSkyrimNetEventTailSizeForPrompt=40` (passed as `maxCount` to `PublicGetRecentEvents` when
  building each tick's snapshot)
- `[AlphaCanon] sDoNotDisturbCellEDIDsCSV=` (empty by default; admin can list cell EditorIDs to gate off)
- `[Dashboard] iHotkeyVK=119` (Windows VK code for `F8`; -1 disables)
- `[Dashboard] iHotkeyModifiers=0` (bitmask: 1=Ctrl, 2=Shift, 4=Alt; 0 = no modifier). Any combination is
  allowed — `7` means Ctrl+Shift+Alt, `5` means Ctrl+Alt, etc.

The Director's MCM page (added in a later step) writes the player's chosen hotkey + modifier combo into
`Data/MCM/Settings/NarrativeEngine.ini`. `Settings::Load()` reads `NarrativeEngine.ini` first, then reads the
MCM Helper-managed INI and overrides the `[Dashboard]` keys if present. If MCM Helper isn't installed, the MCM
INI simply doesn't exist and the plugin INI's values (or the baked-in defaults) win.

Call `Settings::Load()` from `kDataLoaded` **before** any other subsystem init so later subsystems can read
their config.

**Verify:** Boot Skyrim. The log shows `Settings: loaded from Data/SKSE/Plugins/NarrativeEngine.ini` (or "no INI
file at … using defaults"). Set `bDebugMode=1` in a hand-authored INI; the log shows `Settings: debug mode ON`.

---

### Step 5 — Phase tracker

- [x] Complete

**Goal:** tracks the current Freytag phase and the unpaused-real-time spent in it. Persists across save/load.

**Files:**

- `include/PhaseTracker.h` — `Phase` enum (`Exposition`, `RisingAction`, `Climax`, `FallingAction`,
  `Resolution`), `PhaseName(p)` / `PhaseFromName(s)` / `NextPhase(p)` helpers, and the namespace API
  (`Get`, `TimeInPhaseSeconds`, `AdvanceTo`, `Reset`, `Tick`, `OnSave`, `OnLoad`, `OnRevert`).
  - `NextPhase(p)` is **total** and **cyclical** — `NextPhase(Resolution) == Exposition`. There is no
    terminal phase; the Director loop arcs continuously.
- `src/PhaseTracker.cpp` — implementation. Storage: `std::mutex` + current phase + accumulated base seconds
  - a `std::chrono::steady_clock` anchor (`g_lastSampleTime`).

**Specifics:**

- The accumulator is treated as a **continuously sampleable** value rather than one that's only updated at
  tick boundaries. A private `SampleLocked()` helper computes elapsed real time since `g_lastSampleTime` and
  rolls it into `g_baseSeconds` (gated on `RE::UI::GameIsPaused()`), then re-anchors `g_lastSampleTime` to
  now. Every read path (`Tick()`, `TimeInPhaseSeconds()`, `OnSave`) calls `SampleLocked` first so the value
  reflects time-in-phase as of the moment of the call. Without this, OnSave would persist whatever the last
  Tick wrote, missing the unpaused time between that Tick and the save moment.
- `Tick()` is called from the real-time tick driver (Step 8) and just invokes `SampleLocked()` — no externally
  passed dt. Pause states (`RE::UI::GameIsPaused()` — true during menus, console, dialogue) are sampled at the
  moment of each call, so paused intervals don't add to the accumulator.
- `AdvanceTo(newPhase)` resets time-in-phase to 0 and logs the transition. (No event-log write — the
  advancement is already captured in the decision record that triggered it, via its `advancedToPhase` field.)
- Initial state on `kNewGame`: `Exposition`, time-in-phase 0.
- `OnPreLoadGame` sets to a safe-init state (Exposition, 0) that the load callback overwrites.
- Per-payload type ID `'PHTR'`.

**Verify:** From a debug build, manually advance the phase via a temporary console command or test hook; the
log shows the transition and the time-in-phase resets. Save/reload preserves both fields.

---

### Step 6 — Decision log

- [x] Complete

**Goal:** persists every Director evaluation (including no-action evaluations).

**Files:**

- `include/DecisionLog.h` — `DecisionRecord` struct + public API
  (`Append`, `Tail`, `Clear`, `SetMaxEntries`, `OnSave`, `OnLoad`).
- `src/DecisionLog.cpp` — implementation. Storage: `std::deque<DecisionRecord>` capped at
  `Settings::decisionLogMaxEntries`. Thread-safety: `std::shared_mutex` (snapshot reads concurrent with
  appends). Serialization mirrors the field order; use length-prefixed strings for any `std::string` fields.

**`DecisionRecord` fields:**

- `double realTimeSec`, `float gameDaysPassed`.
- `uint32_t tensionScore` (0–100).
- `Phase currentPhase`, `std::optional<Phase> advancedToPhase`.
- `std::string actionSelected` (empty = no action).
- `std::string actionParametersJSON`.
- `std::string narrativeNote` (LLM-supplied rationale).
- `uint32_t alphaCanonActiveSignals` (bitmask snapshot at evaluation time).

Per-payload type ID `'DCLG'`.

**Verify:** Save/load round-trip preserves all fields. (Direct end-to-end verification waits until the
evaluation pipeline is wired through Phase D (Step 14).)

---

### Step 7 — Alpha Canon predicates

- [x] Complete

**Goal:** free C++ functions that test whether vanilla content is currently "in charge." The Director consults
them when deciding whether to act.

**Files:**

- `include/AlphaCanon.h` — `Signal` bitmask enum (`InActiveCombat`, `InScriptedScene`, `InDialogue`,
  `InDoNotDisturbCell`), individual `bool` predicates, `EvaluateAll()` aggregator returning the bitmask, and
  `Names(bitmask)` helper for log/prompt rendering.
- `src/AlphaCanon.cpp` — implementations.

**Predicate implementations:**

- `IsInActiveCombat()` — `RE::PlayerCharacter::GetSingleton()->IsInCombat()`.
- `IsInDialogue()` — `RE::UI::GetSingleton()->IsMenuOpen(RE::DialogueMenu::MENU_NAME)`.
- `IsInScriptedScene()` — `RE::PlayerCharacter::GetSingleton()->GetCurrentScene()` non-null AND
  `scene->isPlaying`.
- `IsInDoNotDisturbCell()` — compare the player's current cell's EditorID against the comma-separated list in
  `Settings::Get().doNotDisturbCellEDIDsCSV`. (Empty list → always false.)

`EvaluateAll()` runs all predicates and returns a bitmask. `Names(mask)` returns the string names of set bits
for prompt context and log output.

(Note: we deliberately defer the `IsMidQuestStageTransition` heuristic. If it's needed in a later phase, it
can be built by querying `SkyrimNetAPI::GetRecentEvents(0, N, "quest_milestone")` — no local event subscription
required.)

**Verify:** Deliberately enter combat; `EvaluateAll()` returns a non-zero mask containing `InActiveCombat`.
Exit combat; mask returns to 0.

---

### Step 8 — Async dispatch and the tick driver

- [x] Complete

**Goal:** one worker thread for off-main-thread work (the LLM call), one tick-driver thread that fires the
Director's evaluation cadence.

**Files:**

- `include/AsyncDispatch.h` — `Start`, `Stop`, `EnqueueWork(std::function<void()>)`,
  `MarshalToMainThread(std::function<void()>)`.
- `src/AsyncDispatch.cpp` — `std::thread` worker pulling from a `std::deque<std::function<void()>>` guarded by
  mutex + condition variable. `MarshalToMainThread` calls `SKSE::GetTaskInterface()->AddTask(fn)`.
- `include/Tick.h` — `Start`, `Stop`.
- `src/Tick.cpp` — `std::thread` that sleeps for `Settings::tickIntervalSeconds`, wakes, and (a) calls
  `PhaseTracker::Tick(dt)` on the main thread, then (b) calls `EvaluationPipeline::BeginEvaluation()` on the
  main thread (added in Step 9).

**Lifecycle:**

- `AsyncDispatch::Start()` from `kDataLoaded`.
- `Tick::Start()` from `kPostLoadGame` / `kNewGame` (only when there's a real game).
- `Tick::Stop()` from `kPreLoadGame` (so the in-flight tick can't fire mid-deserialize).
- `AsyncDispatch::Stop()` is not strictly required (the plugin lifetime equals the process lifetime), but
  declare it for symmetry.

**Verify:** With `bDebugMode=1`, the log shows the tick thread firing every `tickIntervalSeconds`, and the
phase tracker's time-in-phase ticks up during unpaused gameplay (and pauses during menus).

---

### Step 9 — Snapshot type and Phase A builder

- [x] Complete

**Goal:** a value-only `Snapshot` struct captured on the main thread per tick, safe to pass to the worker
thread (no `RE::*` pointers).

**Files:**

- `include/Snapshot.h` — the `Snapshot` struct and the `PlayerContext` child type.
- `include/EvaluationPipeline.h` — declarations for `BeginEvaluation()`, `BuildSnapshot()`,
  `BuildPromptContext()`, `ParseDecision()`, `ApplyDecision()`, `IsEvaluationInFlight()`.
- `src/EvaluationPipeline.cpp` — implementation across all four phases.

**`Snapshot` fields (value-only):**

- `double realTimeSec`.
- `std::string currentPhase`, `float timeInPhaseSeconds`.
- `std::string skyrimNetEventsJSON` — raw JSON array string from
  `SkyrimNetAPI::GetRecentEvents(0, Settings::skyrimNetEventTailSizeForPrompt, "")`. Passed straight through
  to the prompt context; no client-side parsing.
- `std::vector<DecisionRecord> decisionLogTail`.
- `PlayerContext player` — formID, location formID + name, cell formID + name + isInterior,
  `gameDaysPassed`, `timeOfDayHours`.
- `std::vector<std::string> alphaCanonSignals` (name strings) + `uint32_t alphaCanonSignalBitmask`.

(No `availableActions` field for MVP — there are zero in-world Director actions. When the first action arrives
in a later phase, the field is added here alongside the registry that produces it.)

**`BuildSnapshot()` (Phase A, main thread):**

- Calls `SkyrimNetAPI::GetRecentEvents`, `DecisionLog::Tail`,
  `PhaseTracker::Get`/`TimeInPhaseSeconds`, `AlphaCanon::EvaluateAll`, and queries player state via
  `RE::PlayerCharacter::GetSingleton()`, `RE::Calendar::GetSingleton()`.
- `SkyrimNetAPI::GetRecentEvents` is documented as thread-safe, so in principle the worker thread could fetch
  the event tail itself — but keeping the fetch in Phase A keeps the "snapshot is a value-only struct, worker
  touches no live state" invariant clean. The call is cheap (a JSON serialization of ~40 rows).

**Verify:** Add a `--dump-snapshot` debug log line on each tick; verify the captured fields look right
(in-game cell name matches the location the player is standing in, etc.).

---

### Step 10 — Phase B prompt-context builder

- [x] Complete

**Goal:** the worker-thread function that takes a `Snapshot` and produces a JSON context blob to feed the LLM.

**Specifics (in `src/EvaluationPipeline.cpp`):**

- `std::string BuildPromptContext(const Snapshot&)` runs on the worker thread.
- Uses `nlohmann::json` to assemble a JSON object containing:
  - `current_phase`, `time_in_phase_seconds`.
  - `recent_events`: SkyrimNet's event-array JSON, parsed via `nlohmann::json::parse(skyrimNetEventsJSON)`
    and embedded as a sub-array (so the LLM sees the same shape SkyrimNet documents:
    `{type, text, gameTime, originatingActorName, targetActorName}`). On parse failure (shouldn't happen,
    but guard against it), fall back to an empty array and log a warning.
  - `decision_log_tail`: array of `{t, tension_score, phase, action, narrative_note, advanced_to?}`.
  - `player_context`: `{player_form_id, location_name, cell_name, cell_is_interior, game_days_passed, time_of_day_hours}`.
  - `alpha_canon_signals`: array of name strings.
- Returns the dumped JSON string.
- Pure data work — no engine API calls in this function.

**Verify:** With a debug build, log the produced context string for a few ticks; verify it is well-formed JSON
and contains the expected keys.

---

### Step 11 — Phase C: the LLM call

- [x] Complete

**Goal:** wraps `SkyrimNetAPI::SendCustomPromptToLLM` with the prompt template name MVP uses; the callback
parses the response and marshals back to the main thread.

**Specifics:**

- The Director uses prompt template name `"narrative_engine_story_eval"` (created in Step 13) and variant
  `"narrative_engine_director"` (declared in Step 13's manifest).
- `BeginEvaluation()` flow:
  1. Atomic compare-and-swap on an `inFlight` flag to guarantee single concurrent evaluation.
  2. `Snapshot s = BuildSnapshot();` on the main thread.
  3. `AsyncDispatch::EnqueueWork([s = std::move(s)]() mutable { … })` for the worker.
  4. Worker calls `BuildPromptContext(s)` → `std::string ctx`.
  5. Worker calls `SkyrimNetAPI::SendCustomPromptToLLM("narrative_engine_story_eval",
     "narrative_engine_director", ctx, callback)`.
  6. Callback (on SkyrimNet's thread) calls `ParseDecision(response, snapshot)` → `DecisionRecord`.
  7. Callback `MarshalToMainThread([rec] { ApplyDecision(rec); inFlight = false; })`.
- If `SendCustomPromptToLLM` returns false (queue failure), set `inFlight = false` and log a warning. Don't
  retry on the same tick — next tick will try again.

**Verify:** With SkyrimNet's LLM configured, each tick produces a callback invocation visible in the log; the
response is logged at debug level.

---

### Step 12 — The one feature: narrative-state injection (decorators + `system_head` submodule)

- [ ] Complete

**Goal:** make every NPC's LLM context aware of the Director's current tension score and Freytag phase, by
registering two custom SkyrimNet decorators and shipping a `system_head` submodule `.prompt` template that
renders a qualitative sentence about the current state.

**Mechanism — two pieces working together:**

1. **Decorators (C++ → SkyrimNet):** at plugin startup we register two zero-state-of-its-own decorators with
   SkyrimNet via `PublicRegisterDecorator`. They are pure readers — each one returns a snapshot of live state
   pulled from `PhaseTracker` and `DecisionLog`:
   - `ne_narrative_tension` — returns an integer 0–100 (formatted as a decimal string, since SkyrimNet
     decorators are string-typed at the API boundary). The value is the most recent decision's tension score;
     pre-first-evaluation it is `0` (the world starts calm, by default).
   - `ne_narrative_phase` — returns the current `PhaseTracker::Get()` phase name (`"Exposition"`,
     `"RisingAction"`, `"Climax"`, `"FallingAction"`, `"Resolution"`).

   The `PublicRegisterDecorator` callback signature is `std::function<std::string(RE::Actor*)>` — the actor is
   the *context NPC* SkyrimNet is currently rendering for. Both of our decorators **ignore that argument** —
   narrative state is a global property of the world, not a per-NPC fact.

2. **Submodule prompt (asset):** a single `.prompt` file shipped in our `Data/` payload at
   `Data/SKSE/Plugins/SkyrimNet/prompts/submodules/system_head/0155_ne_narrative_state.prompt`. SkyrimNet
   auto-discovers `.prompt` files dropped in this folder and includes them in the head of every LLM prompt the
   plugin builds — regardless of which NPC the prompt is for. The numeric prefix `0155` controls our submodule's
   ordering relative to other `system_head` fragments. (The `prompts/submodules/character_bio/` folder is the
   *per-NPC* equivalent and is the wrong location for a global signal.)

**Files:**

- `include/Decorators.h` — namespace `NarrativeEngine::Decorators` with `void Register()`.
- `src/Decorators.cpp` — implements `Register()`, which calls
  `SkyrimNetAPI::RegisterDecorator("ne_narrative_tension", …, callback)` and the equivalent for
  `ne_narrative_phase`. Logs success/failure for each.
- `Data/SKSE/Plugins/SkyrimNet/prompts/submodules/system_head/0155_ne_narrative_state.prompt` — the Inja/Jinja
  template (lives in the mod folder, not in the C++ source tree).
- A small getter on `DecisionLog` — `std::optional<uint32_t> LatestTensionScore()` returning the most recent
  record's score, or `std::nullopt` if the log is empty. Add it to `include/DecisionLog.h` + `src/DecisionLog.cpp`.

**Decorator implementation details:**

- Both callbacks ignore the `RE::Actor*` argument.
- `ne_narrative_tension` callback:

  ```cpp
  [](RE::Actor*) -> std::string {
      // Pre-first-evaluation default is 0 (calm); after that, the most recent decision's score.
      return std::to_string(DecisionLog::LatestTensionScore().value_or(0));
  }
  ```

  The "is the log empty?" distinction is intentionally collapsed into the value here — the template's job is
  to render a mood, not to gate on whether the Director has run, and starting calm is a reasonable default
  for a session that hasn't ticked yet.

- `ne_narrative_phase` callback:

  ```cpp
  [](RE::Actor*) -> std::string {
      return std::string{PhaseName(PhaseTracker::Get())};
  }
  ```

- **Thread-safety.** SkyrimNet calls these from its prompt-rendering thread (potentially any thread). The
  `PhaseTracker` and `DecisionLog` getters already take their internal mutexes, so the callbacks are safe.

- **Name-collision check.** Before each `RegisterDecorator` call, `SkyrimNetAPI::HasDecorator(name)` is queried
  — if SkyrimNet already has a decorator by that name (built-in or another plugin's), log a warning. The `ne_`
  prefix should keep us out of trouble in practice.

**Prompt template — `0155_ne_narrative_state.prompt`:**

The template's job is to map the numeric tension and the phase name to a single qualitative sentence the LLM
can react to. Emitting raw numbers like "tension level 58" gives the model nothing it knows how to act on;
evocative language ("noticeably tense", "charged") gives it something to lean into.

Sketch (final wording to be refined in implementation):

```jinja
{# 0155_ne_narrative_state.prompt — Director narrative-state injection.
   Reads two custom decorators registered by NarrativeEngine's SKSE plugin
   and renders one qualitative sentence describing the current world mood. #}
{% set tnum  = ne_narrative_tension(actor_uuid) | int %}
{% set phase = ne_narrative_phase(actor_uuid) %}
{% if   tnum < 20 %}{% set mood = "calm and unhurried" %}
{% elif tnum < 45 %}{% set mood = "watchful, with a faint undercurrent of unease" %}
{% elif tnum < 70 %}{% set mood = "noticeably tense — small troubles feel close at hand" %}
{% elif tnum < 90 %}{% set mood = "charged, as though the next hour might change everything" %}
{% else            %}{% set mood = "hot enough that the air itself seems to crackle" %}
{% endif %}
{% if   phase == "Exposition"    %}{% set arc = "the story is still settling in" %}
{% elif phase == "RisingAction"  %}{% set arc = "events are clearly building toward something" %}
{% elif phase == "Climax"        %}{% set arc = "matters are coming to a head" %}
{% elif phase == "FallingAction" %}{% set arc = "the worst seems to have passed, but its echoes linger" %}
{% else                          %}{% set arc = "things have settled into a new normal" %}
{% endif %}

## World mood

The current mood of the world feels {{ mood }}. Narratively, {{ arc }}.
```

Notes on the template:

- No silence-before-first-tick gate. `ne_narrative_tension` returns `"0"` until the Director has produced its
  first evaluation, which renders as "calm and unhurried" — a reasonable default for a session that hasn't
  ticked yet. Likewise `ne_narrative_phase` always returns a valid phase (default `Exposition`).
- The mapping table is intentional and editable. If a play session shows the LLM reacting badly to a particular
  phrasing, the cure is to edit this file and reload the game — no C++ recompile.
- The exact decorator-call syntax (`ne_narrative_tension(actor_uuid)` vs zero-arg form) should be verified
  against SkyrimNet's template runtime before commit. The `PublicRegisterDecorator` signature accepts an
  `RE::Actor*`, so passing the per-render `actor_uuid` and ignoring it on the C++ side is the safe shape.
- The `## World mood` heading is markdown; SkyrimNet's prompt assembly treats `.prompt` output as raw markdown
  fed to the LLM, so the heading helps the LLM partition this fragment from surrounding context.

**Why no SkyrimNet world-knowledge / memory writes:** SkyrimNet's `PublicAddWorldKnowledge` is designed for
**static or condition-gated facts** — entries that get filtered by NPC eligibility and surfaced when relevant.
It's not designed for a single global signal that mutates every tick. `PublicAddMemory` is per-actor. Neither
fits a global, high-churn mood signal. The right SkyrimNet surface for this is the prompt-templating layer —
which is exactly what `submodules/system_head/*.prompt` is for. See
[`../prior-art/SKYRIMNET_PLUGIN_CONTRACT.md`](../prior-art/SKYRIMNET_PLUGIN_CONTRACT.md) for the broader picture
of SkyrimNet's plugin surface.

**Registration wiring (Step 17 wires the call):**

`Decorators::Register()` is called once from `kDataLoaded`, after `SkyrimNetAPI::Initialize()` succeeds. Safe
to no-op if SkyrimNet is unavailable (the wrapper returns false from `RegisterDecorator` and we log a warning).

**Verify:**

1. Boot Skyrim. The log shows `Decorators: registered ne_narrative_tension`,
   `Decorators: registered ne_narrative_phase`. If either name was already taken, the log shows a warning.
2. Talk to any NPC **before** the first Director tick has fired. The assembled prompt contains the
   `## World mood` paragraph rendered against tension=0 / phase=Exposition (i.e. "calm and unhurried…the story
   is still settling in"). Verifiable via a SkyrimNet debug-render of the prompt for that NPC.
3. Wait one tick interval for the first evaluation to complete. Talk to any NPC again. The `## World mood`
   paragraph now reflects the just-written tension band and phase.
4. Reload an earlier save where the Director state was at a different tension/phase. The first NPC interaction
   after the load shows the updated phrasing.

---

### Step 13 — The Director's prompt template + SkyrimNet manifest

- [ ] Complete

**Goal:** the actual prompt SkyrimNet hands to the LLM when we call
`SendCustomPromptToLLM("narrative_engine_story_eval", "narrative_engine_director", …)`, plus the manifest
that declares the `narrative_engine_director` LLM-config variant (without which the call falls back to
SkyrimNet's default Dialogue LLM, which is tuned for creative writing rather than per-tick classification).

**Files:**

- `statics/SKSE/Plugins/SkyrimNet/prompts/narrative_engine_story_eval.prompt` — the prompt template.
  Deployed to `<mod folder>/SKSE/Plugins/SkyrimNet/prompts/narrative_engine_story_eval.prompt`.
- `statics/SKSE/Plugins/SkyrimNet/config/plugins/NarrativeEngine/manifest.yaml` — the plugin manifest.
  Deployed to `<mod folder>/SKSE/Plugins/SkyrimNet/config/plugins/NarrativeEngine/manifest.yaml`.

**Manifest specifics:**

SkyrimNet auto-discovers any plugin folder under `Data/SKSE/Plugins/SkyrimNet/config/plugins/<plugin>/`
(filesystem-based, no registration call needed). The manifest has three top-level sections:

- `plugin:` — name, version, description. Identifies our plugin in SkyrimNet's UI.
- `variants:` — declares one variant `narrative_engine_director`. The variant is a *named LLM-config
  profile*: when we invoke `SendCustomPromptToLLM(prompt, "narrative_engine_director", …)`, SkyrimNet
  takes its base LLM config, layers the per-plugin overrides on top, and sends the result. The variant is
  intentionally generic (not tied to a specific prompt) so future NarrativeEngine prompts (e.g. an
  action-selection prompt in a later phase) can reuse the same Director-voice config without manifest
  changes.
- `schema.fields:` — declares the "LLM Overrides" category exposing `llm.endpoint`, `llm.api_key`,
  `llm.model_name`, `llm.temperature`, `llm.max_tokens`, `llm.timeout`. SkyrimNet surfaces these in its
  in-game settings; the user can override any of them per-plugin and SkyrimNet honors the overrides when
  invoking our variant. The plugin's own code reads none of these — SkyrimNet handles the layering
  internally. Plugin-specific (non-LLM) settings can be added in their own categories later.

No C++ "register variant" call exists or is needed. The plugin's contract is: ship the manifest
declaratively, pass the variant name as the 2nd arg to `SendCustomPromptToLLM`. (IntelEngine follows the
same pattern — see its `manifest.yaml` for the precedent.)

**Specifics:**

- Jinja2-style template (SkyrimNet renders these through its Inja engine). Follows the house style in
  [`../CUSTOM_PROMPTS.md`](../CUSTOM_PROMPTS.md) — `[ system ] ... [ end system ]` + `[ user ] ... [ end user ]`,
  format-first/format-last, no trigger-cadence language, `{% if %}`-hide whole sections rather than rendering
  placeholders.
- The template receives the context JSON from `BuildPromptContext`. It can reference the keys directly:
  `{{ current_phase }}`, `{{ next_phase }}`, `{{ time_in_phase_seconds }}`, etc.
- **Binary stay/advance phrasing.** We deliberately don't ask the LLM to pick from all five phase names. The
  prompt presents two things — the current phase and the immediate next phase (from `BuildPromptContext`'s
  `next_phase` field, which the C++ side computes from `PhaseTracker::NextPhase`) — and asks for a boolean
  `advance_phase`. Because the loop is cyclical (`NextPhase(Resolution) == Exposition`), `next_phase` is
  always a valid Freytag-pyramid phase name; the prompt has no terminal special case. The "never skip, never
  go backwards" invariant lives in C++ and isn't cognitive load on the model.
- The instructions tell the LLM to:
  - Score current tension (0–100).
  - Decide whether to stay (`advance_phase: false`) or advance to the named next phase (`advance_phase: true`).
  - Lean toward continuity when any Alpha Canon signal is active.
  - Produce a one-sentence narrative note explaining the rationale.
- Response format is a strict JSON object — no markdown fences:

  ```json
  {
    "tension_score": <int 0..100>,
    "advance_phase": <bool>,
    "narrative_note": "<≤200 chars>"
  }
  ```

- MVP has no in-world action for the LLM to choose, so the response schema deliberately omits an `action` field.
  (When the first real Director action is added in a later phase, the schema grows an optional `action` /
  `action_parameters` pair and the prompt is updated to describe the available choices.)

The implementer drafts this by hand against the keys `BuildPromptContext` produces. Keep it short — the
Director's decision space is narrow.

SkyrimNet auto-discovers any `.prompt` file dropped into `SKSE/Plugins/SkyrimNet/prompts/` (and, recursively,
its `submodules/*` subfolders). The `.prompt` itself doesn't need a manifest entry to be *invokable* — the
manifest's role is separate (declaring the LLM-config variant and exposing the user-override schema). The
same is true for the `system_head` submodule from Step 12.

**Verify:**

1. First Director evaluation completes end-to-end: the log shows the prompt being sent, the callback firing
   with a response, and the response parsing into a valid `DecisionRecord` written to the decision log. On
   the next NPC interaction, the `0155_ne_narrative_state.prompt` fragment in the assembled bio reflects the
   new tension/phase.
2. In SkyrimNet's in-game settings UI, a "NarrativeEngine" plugin entry appears with an "LLM Overrides"
   category exposing endpoint / key / model / temperature / max-tokens / timeout fields. Setting any of them
   to a non-default value and triggering a tick should cause SkyrimNet to use the override when calling the
   `narrative_engine_director` variant.

---

### Step 14 — Phase D: decision parser and applier

- [ ] Complete

**Goal:** parses the LLM JSON response into a `DecisionRecord`, gates phase advancement, and writes the
decision log entry. There is no in-world action to fire — the narrative-state injection from Step 12 reads
live `DecisionLog::LatestTensionScore()` and `PhaseTracker::Get()`, so the act of writing the new record
into `DecisionLog` (and optionally advancing the phase tracker) is the entire "apply" step.

**Specifics (in `src/EvaluationPipeline.cpp`):**

- `DecisionRecord ParseDecision(jsonString, snapshot)` (worker thread):
  - Pre-fills `realTimeSec`, `gameDaysPassed`, `currentPhase`, `alphaCanonActiveSignals` from the snapshot
    (so even a parse failure produces a valid record).
  - Parses the JSON via `nlohmann::json::parse(jsonString, nullptr, false)`. On parse failure, sets
    `narrativeNote = "parse_failure: <what>"` and returns.
  - Extracts `tension_score` (clamp 0..100).
  - Extracts `advance_phase` (bool). When `true`, sets `advancedToPhase` to
    `NextPhase(snapshot.currentPhase)` — which is always a valid phase since the loop is cyclical
    (`NextPhase(Resolution) == Exposition`). When `false`, leaves `advancedToPhase` as `nullopt`. The "no
    skip, no rewind" invariant is enforced here in C++ rather than asked of the LLM; the prompt only exposes
    the binary stay/advance choice.
  - Extracts `narrative_note` (clamp to 200 chars).
  - `actionSelected` is left as the empty string for MVP (no actions exist).
- `void ApplyDecision(const DecisionRecord&)` (main thread):
  - `DecisionLog::Append(record);` — this is what the `ne_narrative_tension` decorator will read on the next
    NPC bio render.
  - If `record.advancedToPhase`: `PhaseTracker::AdvanceTo(*record.advancedToPhase);` — this is what the
    `ne_narrative_phase` decorator will read.

**Verify:** Over 5+ ticks, the decision log accumulates entries; each one has a sensible tension score and a
narrative note that references real recent events (verified by spot-checking against what was happening in-game
at the timestamp). Talking to an NPC after each tick shows the `0155_ne_narrative_state.prompt` fragment
reflecting the just-written tension band.

---

### Step 15 — Dashboard build pipeline (TypeScript + React + Rollup)

- [ ] Complete

**Goal:** stand up a self-contained JS bundle, built from TypeScript React source via Rollup, that PrismaUI can
load as the dashboard view. No CMake involvement — the dashboard build runs independently of the C++ build via
`npm` scripts.

**Directory layout** (everything under a new `dashboard/` directory at the project root):

```text
dashboard/
├── package.json
├── tsconfig.json
├── rollup.config.mjs
├── index.html              # static shell; copied to dist/
├── styles.css              # → dist/dashboard.css
└── src/
    ├── index.tsx           # entry; mounts React + registers window.updateFullState
    ├── App.tsx
    ├── types.ts            # DirectorState type — must match C++ JSON schema
    └── components/
        ├── StatusBanner.tsx
        ├── PhasePanel.tsx
        ├── LastEvaluation.tsx
        ├── DecisionList.tsx
        └── EventList.tsx
```

**`dashboard/package.json` — key fields:**

- `"name": "narrativeengine-dashboard"`, `"private": true`, `"type": "module"`.
- `"scripts"`:
  - `"build": "rollup -c"` — produces `dist/dashboard.js` (and copies the HTML/CSS shell).
  - `"deploy": "node deploy.mjs"` — copies `dist/*` into the mod folder's PrismaUI views path (see below).
  - `"build:deploy": "npm run build && npm run deploy"` — convenience for "after every dashboard edit".
- `"devDependencies"`: `react`, `react-dom` plus their `@types/*`, `typescript`, `rollup`, and the Rollup
  plugins listed below.

**Rollup plugins** (in `dashboard/rollup.config.mjs`):

- `@rollup/plugin-typescript` — TS → JS.
- `@rollup/plugin-node-resolve` — resolves the bundled `react` / `react-dom` from `node_modules`.
- `@rollup/plugin-commonjs` — React is CJS-shipped; needed to bundle it.
- `@rollup/plugin-replace` — sets `process.env.NODE_ENV` to `"production"` so React strips its dev assertions
  and the bundle is small.
- `rollup-plugin-copy` (or a small inline plugin) — copies `index.html` and `styles.css` into `dist/`,
  renaming `styles.css` to `dashboard.css` for symmetry with the JS bundle name.

**Rollup output config:**

- `output.file: "dist/dashboard.js"`
- `output.format: "iife"` — PrismaUI loads via a `<script>` tag in plain browser context; IIFE is the right
  shape and avoids any module-loading runtime requirement.
- `output.name: "NarrativeEngineDashboard"`
- `output.sourcemap: true` (helpful for debugging during development; can be turned off later).

**`dashboard/tsconfig.json` — key compiler options:**

- `"target": "ES2020"`, `"module": "ESNext"`, `"moduleResolution": "node"`.
- `"jsx": "react-jsx"`, `"jsxImportSource": "react"`.
- `"strict": true`, `"noUncheckedIndexedAccess": true`.
- `"include": ["src/**/*"]`.

**`dashboard/src/types.ts` — schema contract:**

This file defines the `DirectorState` type the React app expects. The C++ side (Step 16) MUST produce JSON
matching exactly this shape. Treat changes to this type and the C++ composer as a single coordinated edit.

```ts
export interface DirectorState {
  status: {
    skyrim_net_available: boolean;
    skyrim_net_version: number;
    director_enabled: boolean;
    prisma_ui_available: boolean;
  };
  current_phase: "Exposition" | "RisingAction" | "Climax" | "FallingAction" | "Resolution";
  time_in_phase_seconds: number;
  last_evaluation: {
    timestamp: number;          // realTimeSec
    tension_score: number;      // 0..100
    narrative_note: string;
    advanced_to: string | null;
    alpha_canon_signals: string[];
  } | null;
  recent_decisions: DecisionEntry[];
  recent_events: EventEntry[];
}

export interface DecisionEntry {
  timestamp: number;
  tension_score: number;
  phase: string;
  action: string | null;
  narrative_note: string;
}

export interface EventEntry {
  // Mirrors SkyrimNet's PublicGetRecentEvents JSON shape; populated by the C++ composer
  // by parsing the JSON string SkyrimNet returns.
  type: string;                       // e.g. "dialogue", "combat_start"
  text: string;
  gameTime: number;
  originatingActorName: string;
  targetActorName: string;
}
```

**`dashboard/src/index.tsx`:**

- Mounts `<App />` into a `#root` element.
- Registers `window.updateFullState = (jsonString: string) => { ... }` which parses the string into a
  `DirectorState` and updates a shared store (React `useState` lifted to App via context, or a tiny
  hand-rolled subscribe-based singleton — either is fine at MVP scale).
- Wraps the parse in try/catch; on parse failure, sets an error banner on the App rather than throwing.

**`dashboard/src/App.tsx`:**

- Holds the `DirectorState | null` in state. Initial value `null` ("waiting for first push").
- Subscribes to updates from the singleton populated by `window.updateFullState`.
- Renders the component tree:

  ```tsx
  <div className="dashboard">
    <StatusBanner status={state.status} />
    <PhasePanel phase={state.current_phase} timeInPhaseSeconds={state.time_in_phase_seconds} />
    <LastEvaluation evaluation={state.last_evaluation} />
    <DecisionList items={state.recent_decisions} />
    <EventList items={state.recent_events} />
  </div>
  ```

- When `state === null`, renders a placeholder ("Awaiting first Director evaluation…").

**Sub-components** are each ~30–50 lines: render the data passed to them, no internal state, no fetches.

**`dashboard/index.html`:**

- Minimal HTML5 shell. `<link rel="stylesheet" href="dashboard.css">`, `<div id="root"></div>`,
  `<script src="dashboard.js"></script>`. UTF-8, viewport tag.

**`dashboard/styles.css`:**

- Minimal styling — dark background, monospace font for the log lists, padding and borders to separate the
  panels. Aim for "looks fine in a webview"; this is not a designed UI.

**`dashboard/deploy.mjs`** — a small Node script that copies `dist/*` to
`$SKYRIM_MODS_FOLDER/NarrativeEngine/PrismaUI/views/NarrativeEngine/dashboard/`. Reads the env var; errors out
with a useful message if unset. Creates the destination directory if missing.

**`.gitignore`** at the project root: add `dashboard/node_modules/` and `dashboard/dist/`.

**Verify:**

1. From the project root: `cd dashboard && npm install`. No errors.
2. `npm run build`. Produces `dashboard/dist/dashboard.js` (~150–250 KB minified-ish; React contributes most of
   it) plus `dashboard/dist/index.html` and `dashboard/dist/dashboard.css`. No TypeScript errors. No Rollup
   warnings.
3. Open `dashboard/dist/index.html` in any browser. The dashboard renders the placeholder ("Awaiting first
   Director evaluation…"). From the browser console, call `window.updateFullState(JSON.stringify(...))` with a
   sample object matching the `DirectorState` shape — e.g.:

   ```js
   window.updateFullState(JSON.stringify({
     status: {
       skyrim_net_available: true,
       skyrim_net_version: 9,
       director_enabled: true,
       prisma_ui_available: true,
     },
     current_phase: "Exposition",
     time_in_phase_seconds: 42,
     last_evaluation: null,
     recent_decisions: [],
     recent_events: [],
   }));
   ```

   The dashboard re-renders with the supplied state.
4. `npm run deploy`. The files appear at
   `C:\Modlists\NGVO\mods\NarrativeEngine\PrismaUI\views\NarrativeEngine\dashboard\`.

---

### Step 16 — `DashboardUIManager` and Director-loop wiring

- [ ] Complete

**Goal:** the C++ module that composes the JSON state blob, pushes it to the React dashboard via PrismaUI's
JS-interop after every Director `ApplyDecision`, and toggles dashboard visibility on a configurable hotkey.

**Files:**

- `include/DashboardUIManager.h` — `Initialize()`, `Shutdown()`, `PushFullState()`, `ToggleVisibility()`,
  `ComposeFullStateJSON()` (exposed for unit testing).
- `src/DashboardUIManager.cpp` — implementation.

**`Initialize()` (called from `kDataLoaded`, after `PrismaUI_API::Initialize()`):**

1. If `PrismaUI_API::IsAvailable() == false`, log `DashboardUIManager: PrismaUI absent; dashboard disabled`
   and return. The rest of the plugin works unchanged.
2. Resolve the absolute path to `index.html`:
   `<game data folder>/PrismaUI/views/NarrativeEngine/dashboard/index.html`. (Compute via SKSE's data-handler
   / current-working-directory rather than hardcoding the mod folder — the game loads from `Data/`.)
3. Call the PrismaUI wrapper's view-creation function with that path. Stash the returned view handle.
4. Register the hotkey via the PrismaUI wrapper's hotkey-registration function, using
   `Settings::Get().dashboardHotkeyVK` and `Settings::Get().dashboardHotkeyModifiers`. The hotkey callback
   calls `ToggleVisibility()`. If PrismaUI's API doesn't expose a hotkey-registration symbol, fall back to
   the SKSE input-event-sink pattern (`RE::BSTEventSink<RE::InputEvent*>`) and dispatch on matching key
   events.
5. **Modifier matching rule.** The configured `iHotkeyModifiers` bitmask must match **exactly** when deciding
   whether to fire — not "at least these mods." So a hotkey registered as `F8 + Shift` (mask `2`) fires only
   when Shift is held alone, not when Ctrl+Shift+F8 is pressed. Player-bound combos like Ctrl+Alt+Shift+`7`
   (mask `7`) require all three modifiers held with no others. At keypress time, query each modifier's state
   (`GetAsyncKeyState(VK_CONTROL)`, `VK_SHIFT`, `VK_MENU` for the Alt-key VK; or the equivalent from the SKSE
   input event sink's modifier flags) and compare the assembled bitmask to the configured one.
6. Log `DashboardUIManager: initialized (view created, hotkey VK=<N> mods=<M>)`.

**`ComposeFullStateJSON()` (pure function, main thread; safe to call from `ApplyDecision`):**

- Builds an `nlohmann::json` object matching the `DirectorState` schema declared in
  `dashboard/src/types.ts` exactly. Keys, types, and nesting must match. If C++ and TS drift apart, the
  React app silently shows stale/missing fields — so the implementer should keep this composer and
  `dashboard/src/types.ts` updated as a single coordinated change.
- Sources its fields from `SkyrimNetAPI` (including `GetRecentEvents(0, 20, "")` for the recent-events
  panel — parsed and re-emitted as the `EventEntry[]` shape declared in `dashboard/src/types.ts`),
  `PrismaUI_API`, `PhaseTracker`, and `DecisionLog::Tail(10)`.
- Returns the dumped string.

**`PushFullState()` (called from `ApplyDecision` in Step 14's Phase D, and from `ToggleVisibility()` on show):**

- If PrismaUI is unavailable, return immediately.
- `const std::string json = ComposeFullStateJSON();`
- Call the PrismaUI wrapper's JS-invoke function: `InvokeJS("updateFullState", json)`. The JS side's
  `window.updateFullState(jsonString)` (Step 15's `index.tsx`) parses and re-renders.

**`ToggleVisibility()`:**

- Track a local `bool g_visible` flag.
- On show: push fresh state first (so the player sees current data), then call PrismaUI's show-view function.
- On hide: call PrismaUI's hide-view function.

**Director-loop wiring (modifies Step 14 — Phase D):**

- At the end of `ApplyDecision(record)`, after the action dispatch, call `DashboardUIManager::PushFullState();`
  unconditionally. Cheap — JSON compose is microseconds; the JS interop is async on PrismaUI's end.

**Verify:**

1. With PrismaUI installed and the dashboard deployed (Step 15 `npm run build:deploy`), boot Skyrim, load a
   save, wait for the first Director tick. The Papyrus log shows the tick firing. Press `F8`. The dashboard
   appears, rendering the current state.
2. Wait for another tick to fire. Without pressing anything, the dashboard's "Last evaluation" panel and the
   recent-decisions list update with the new entry.
3. Press `F8` again. The dashboard hides.
4. Uninstall PrismaUI, restart Skyrim. The log shows `PrismaUI: not found` and
   `DashboardUIManager: PrismaUI absent; dashboard disabled`. The Director loop continues running normally.

---

### Step 17 — MCM page (MCM Helper)

- [ ] Complete

**Goal:** a one-screen Mod Configuration Menu page declared through MCM Helper. Left column: static mod
credits. Right column: a single setting — the dashboard hotkey, with arbitrary modifier-combo support.

**Files:**

- `C:\Modlists\NGVO\mods\NarrativeEngine\MCM\Config\NarrativeEngine\config.json` — MCM Helper page config.
- `C:\Modlists\NGVO\mods\NarrativeEngine\MCM\Config\NarrativeEngine\settings.ini` — initial default values
  (MCM Helper reads this as the seed; the live values get written to `Data/MCM/Settings/NarrativeEngine.ini`).
- (No C++ or Papyrus files needed for the MCM itself — MCM Helper handles the UI and storage.)

**`config.json` structure (verify exact schema in MCM Helper's docs before implementing):**

MCM Helper organizes pages by sub-pages, each sub-page having groups of options. Our page has one sub-page,
two groups (one per column), and the right column has the keybind option(s).

Conceptual shape (the actual keys depend on MCM Helper's schema):

```jsonc
{
  "modName": "NarrativeEngine",
  "displayName": "NarrativeEngine",
  "pages": [
    {
      "pageDisplayName": "",  // single page; no tabs
      "content": [
        {
          "text": "## Mod Credits",
          "type": "header"
        },
        {
          "text": "NarrativeEngine v0.1.0\\n\\nAn AI Director for Skyrim, layered on top of SkyrimNet.\\n\\nAuthor: <user handle>\\nProject: <repo URL>",
          "type": "text"
        },
        {
          "text": "## Settings",
          "type": "header"
        },
        {
          "text": "Dashboard Hotkey",
          "type": "keymap",
          "valueOptions": {
            "sourceType": "ModSettingInt",
            "sourceName": "iHotkeyVK:Dashboard",
            "modifierSourceName": "iHotkeyModifiers:Dashboard"
          },
          "help": "Press a key (and optional modifiers) to bind. Supports Ctrl, Shift, Alt in any combination."
        }
      ]
    }
  ]
}
```

If MCM Helper's `keymap` option doesn't natively support modifier combinations, the fallback is to render four
fields instead — one `keymap` for the main key, plus three boolean toggles for Ctrl, Shift, Alt — bound to
`iHotkeyVK`, `iHotkeyCtrl`, `iHotkeyShift`, `iHotkeyAlt`. The plugin then assembles the modifier bitmask from
the three booleans at `Settings::Load()` time. Verify which mechanism MCM Helper supports before choosing.

**Plugin-side change (modifies the Settings step's `Load()`):**

After reading `Data/SKSE/Plugins/NarrativeEngine.ini`, attempt to read
`Data/MCM/Settings/NarrativeEngine.ini`. If it exists, read the `[Dashboard]` section's `iHotkeyVK` and
`iHotkeyModifiers` keys; if present, they override the values from the plugin INI. If the MCM INI doesn't
exist (MCM Helper not installed, or the user has never opened the MCM page), the override pass is a no-op
and the plugin INI / defaults stand.

Log the resolved hotkey after settings load: `Settings: dashboard hotkey VK=<N> mods=<M>` so the implementer
can verify rebinds took effect after a save reload.

**Verify:**

1. Install MCM Helper. Boot Skyrim with the mod loaded. Open MCM via SkyUI. A "NarrativeEngine" entry
   appears in the mod list.
2. Open the NarrativeEngine page. The left column shows the credits text; the right column shows the
   "Dashboard Hotkey" row with the current binding displayed.
3. Click the hotkey to rebind. Press Ctrl+Shift+`8` (or any combo). MCM Helper records the new value and
   writes it to `Data/MCM/Settings/NarrativeEngine.ini`.
4. Save the game, exit to main menu, reload. The plugin log shows
   `Settings: dashboard hotkey VK=56 mods=3` (or whatever the new combo decoded to).
5. Press the new hotkey combo in-game. The PrismaUI dashboard toggles.
6. Uninstall MCM Helper. Boot Skyrim. The plugin loads cleanly. The MCM entry is gone; the hotkey falls back
   to whatever's in `Data/SKSE/Plugins/NarrativeEngine.ini` (or the default F8).

---

### Step 18 — Wire it all into `Plugin.cpp`

- [ ] Complete

**Goal:** the lifecycle handlers from Step 1 now have real bodies that call into every subsystem.

**`OnDataLoaded` (in execution order):**

1. `Settings::Load();`
2. `DecisionLog::SetMaxEntries(Settings::Get().decisionLogMaxEntries);`
3. `SkyrimNetAPI::Initialize();` (log warning if it fails; the rest still loads)
4. `Decorators::Register();` (Step 12 — registers `ne_narrative_tension` and `ne_narrative_phase` with
   SkyrimNet. No-ops gracefully if SkyrimNet is unavailable.)
5. `AsyncDispatch::Start();`

**`OnNewGame`:**

- `DecisionLog::Clear(); PhaseTracker::Reset(Phase::Exposition);`
- `Tick::Start();`

**`OnPreLoadGame`:**

- `Tick::Stop();`
- Reset all subsystem in-memory state to fresh-init defaults (the load callback overwrites with the saved
  state).

**`OnPostLoadGame`:**

- `Tick::Start();`

**`OnSave`:**

- `DecisionLog::OnSave(intfc); PhaseTracker::OnSave(intfc);`

**`OnLoad`:**

- Loop `intfc->GetNextRecordInfo`; dispatch each known type ID to the right subsystem.

**`OnRevert`:**

- Clear every subsystem.

**Verify:** Build, deploy, boot Skyrim. Start a new game (or load an existing one). Within `tickIntervalSeconds`
the log shows the first evaluation: snapshot → prompt → response → decision log entry. Talking to any NPC
afterward shows the `0155_ne_narrative_state.prompt` fragment in the assembled bio. Continue playing for 10+
minutes; ticks accumulate cleanly. Save, exit to main menu, reload; all state is restored exactly.

---

### Step 19 — Integration verification

- [ ] Complete

The plumbing-and-feature MVP is complete when all of the below are true:

- [ ] Plugin loads without errors. `NarrativeEngine.log` shows `SkyrimNetAPI: initialized (version=<N>)` and
      `Decorators: registered ne_narrative_tension`, `Decorators: registered ne_narrative_phase`.
- [ ] During a 10-minute play session with SkyrimNet's LLM available, the decision log accumulates ≥ 5 entries;
      each entry's `narrative_note` plausibly references real recent events.
- [ ] After the first Director tick has fired, talking to any NPC produces an assembled SkyrimNet prompt that
      contains the `## World mood` paragraph from `0155_ne_narrative_state.prompt`, with the qualitative wording
      matching the current `DecisionLog::LatestTensionScore()` band and `PhaseTracker::Get()` phase. (Verifiable
      via a SkyrimNet debug-render of the prompt for the target NPC, or by inspecting SkyrimNet's own logs.)
- [ ] Before the first tick fires, the `## World mood` paragraph is still present, rendered against tension=0
      / phase=Exposition (i.e. "calm and unhurried…the story is still settling in"). The pre-first-tick state
      is a deliberate default, not a silence gate.
- [ ] Triggering vanilla combat results in combat events appearing in `SkyrimNetAPI::GetRecentEvents(0, 40, "")`
      output (verifiable from the dashboard's recent-events panel or from a debug log dump of the snapshot).
      The Director's logged decisions reference the combat in their narrative notes within a tick or two.
- [ ] Saving mid-session and reloading preserves the decision log, phase, and time-in-phase. The decorators
      have nothing of their own to persist — they read live state — so a load immediately produces correctly
      phrased injections from the restored tension/phase.
- [ ] Removing SkyrimNet from the load order causes the plugin to load with a clear error message; the tick
      driver still runs but evaluations are skipped (no crash).
- [ ] With PrismaUI installed, pressing the configured hotkey (default `F8`) toggles the dashboard view. The
      view renders the current Director state and updates within one tick of each evaluation.
- [ ] Removing PrismaUI from the load order causes the plugin to load with a `PrismaUI: not found; dashboard
      disabled` log line. The hotkey is unregistered. The Director loop continues running.
- [ ] `npm run build` in the `dashboard/` directory produces `dashboard/dist/{index.html, dashboard.js,
      dashboard.css}` with no TypeScript errors and no Rollup warnings.
- [ ] With MCM Helper installed, a "NarrativeEngine" entry appears in SkyUI's MCM list. Opening it shows the
      credits text in the left column and the dashboard-hotkey binder in the right.
- [ ] Rebinding the dashboard hotkey to a multi-modifier combo (e.g. Ctrl+Shift+`7`) in MCM, saving, and
      reloading the save makes the new combo work in-game. Pressing the key alone, or with a wrong modifier
      set, does NOT toggle the dashboard.
- [ ] Removing MCM Helper from the load order: the plugin loads cleanly, the MCM entry is gone, the hotkey
      falls back to the value in `Data/SKSE/Plugins/NarrativeEngine.ini` (or the baked-in default `F8`).

---

## After MVP

The next phase chosen by [`../DESIGN_GOALS.md`](../DESIGN_GOALS.md) priorities will go in
`docs/implementation/PHASE_02_<NAME>.md`. Plausible candidates, in roughly the order they earn their slot
*for NarrativeEngine's own reasons* (not because IntelEngine had them):

- **The first in-world Director action**, and with it the `Action` interface and `ActionRegistry` scaffolding
  designed against that first concrete consumer. The simplest worth its risk is an in-game notification on
  phase advancement (which earns its Alpha Canon gate from day one — the player shouldn't get notification
  spam mid-combat).
- The Alpha Canon hard gate as a first-class step in `ApplyDecision`, refined alongside that first action's
  rollout.
- Cooldown tracking on the action registry, once the toolbox has more than one entry.
- A genuinely tension-affecting action (e.g. an ambient sound cue on phase advance).
- The first background simulation, designed alongside the harness that hosts it.

Each gets its own MVP-style plan doc in `docs/implementation/`.

## File inventory (MVP final state)

C++:

- `plugin.cpp` (shim into `NarrativeEngine::Startup`)
- `include/Plugin.h` + `src/Plugin.cpp`
- `include/Settings.h` + `src/Settings.cpp`
- `include/SkyrimNetAPI.h` + `src/SkyrimNetAPI.cpp`
- `include/PrismaUI.h` + `src/PrismaUI.cpp`
- `include/DecisionLog.h` + `src/DecisionLog.cpp`
- `include/PhaseTracker.h` + `src/PhaseTracker.cpp`
- `include/AlphaCanon.h` + `src/AlphaCanon.cpp`
- `include/AsyncDispatch.h` + `src/AsyncDispatch.cpp`
- `include/Tick.h` + `src/Tick.cpp`
- `include/Snapshot.h`
- `include/EvaluationPipeline.h` + `src/EvaluationPipeline.cpp`
- `include/Decorators.h` + `src/Decorators.cpp`
- `include/DashboardUIManager.h` + `src/DashboardUIManager.cpp`

Dashboard (TypeScript + React + Rollup):

- `dashboard/package.json`
- `dashboard/tsconfig.json`
- `dashboard/rollup.config.mjs`
- `dashboard/index.html` — static shell loaded by PrismaUI; references `dashboard.js` and `dashboard.css`
- `dashboard/styles.css` — minimal stylesheet (copied to deploy as `dashboard.css`)
- `dashboard/src/index.tsx` — entry point; mounts the React root and registers `window.updateFullState`
- `dashboard/src/App.tsx` — top-level component
- `dashboard/src/types.ts` — TypeScript schema for the C++ → JS state push
- `dashboard/src/components/StatusBanner.tsx`
- `dashboard/src/components/PhasePanel.tsx`
- `dashboard/src/components/LastEvaluation.tsx`
- `dashboard/src/components/DecisionList.tsx`
- `dashboard/src/components/EventList.tsx`

Build output (gitignored; produced by `npm run build`):

- `dashboard/dist/index.html`, `dashboard/dist/dashboard.js`, `dashboard/dist/dashboard.css`

Deployed dashboard payload (post-`npm run build` + deploy step):

- `C:\Modlists\NGVO\mods\NarrativeEngine\PrismaUI\views\NarrativeEngine\dashboard\index.html`
- `C:\Modlists\NGVO\mods\NarrativeEngine\PrismaUI\views\NarrativeEngine\dashboard\dashboard.js`
- `C:\Modlists\NGVO\mods\NarrativeEngine\PrismaUI\views\NarrativeEngine\dashboard\dashboard.css`

SkyrimNet assets:

- `C:\Modlists\NGVO\mods\NarrativeEngine\SKSE\Plugins\SkyrimNet\prompts\narrative_engine_story_eval.prompt` — the
  Director's own LLM prompt (Step 13).
- `C:\Modlists\NGVO\mods\NarrativeEngine\SKSE\Plugins\SkyrimNet\config\plugins\NarrativeEngine\manifest.yaml`
  — declares the `narrative_engine_director` LLM-config variant and the user-overridable LLM schema
  (Step 13).
- `C:\Modlists\NGVO\mods\NarrativeEngine\SKSE\Plugins\SkyrimNet\prompts\submodules\system_head\0155_ne_narrative_state.prompt`
  — the per-NPC narrative-state injection submodule (Step 12).

MCM Helper assets (in the mod folder):

- `C:\Modlists\NGVO\mods\NarrativeEngine\MCM\Config\NarrativeEngine\config.json` — page layout declaration
- `C:\Modlists\NGVO\mods\NarrativeEngine\MCM\Config\NarrativeEngine\settings.ini` — seed defaults

MCM Helper-managed (auto-written by the player's MCM interactions; lives in the game's `Data/` tree):

- `Data/MCM/Settings/NarrativeEngine.ini`

INI (plugin-author tunables; optional, defaults are fine):

- `Data/SKSE/Plugins/NarrativeEngine.ini`

**Zero** `.psc` files. **Zero** new Creation Kit forms.
