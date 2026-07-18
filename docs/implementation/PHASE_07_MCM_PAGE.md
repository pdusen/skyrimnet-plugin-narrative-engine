# Phase 07 — MCM Configuration Page

Phase 01's MVP shipped a working dashboard behind a
hardcoded-default hotkey (`F7`) configured through the plugin
INI. **Step 17 of Phase 01 (the MCM page) was explicitly
deferred** because bringing up any MCM page requires the
consuming mod to ship an `.esp` carrying a Quest with an
attached MCM Papyrus script, and Phase 01 deliberately kept
the mod C++-only. Phases 04 and 05 have since introduced the
`.esp` file, the Papyrus source tree, and the CK compile
pipeline — the tools that made Step 17 uneconomic to build
are now in place for their own reasons.

This phase brings up that MCM page in the shape Step 17
sketched: a **single page, two columns**. Left: static mod
credits and identifying information. Right: one control —
the dashboard hotkey rebinder, with modifier-combo support.
No other MCM options. No per-beat toggles, no tuning knobs,
no dev-mode surfaces.

The page is authored as an **imperative SkyUI MCM** — a
Papyrus script extending SkyUI's `SKI_ConfigBase` directly,
rather than going through an intermediate plugin like MCM
Helper. The layout is simple enough (four static items and
one keymap control) that the declarative-JSON tooling would
have been more moving parts than the page itself, and MCM
Helper would have been an extra SKSE-plugin dependency for
downstream users to install. `SKI_ConfigBase` gives us the
imperative API — `AddHeaderOption`, `AddTextOption`,
`AddKeyMapOptionST`, `OnKeyMapChangeST` — that's already
part of the SkyUI SDK anyone building the mod already has.

The phase also folds in a small input-space cleanup that
surfaced during Phase 04–05 dev work but wasn't worth
touching in isolation: SKSE input events and SkyUI keymap
controls both speak **DirectX scan codes**, while our
`HotkeySink` was translating to Windows VK codes via
`MapVirtualKeyW` before comparing. That translation was a
needless round-trip and, once SkyUI's `OnKeyMapChangeST`
starts feeding rebinds back to C++, it means the scan code
SkyUI gives us can't be compared to a scan code SKSE gives
us without an extra conversion. Phase 07 aligns the C++ side
on scan-code space so SkyUI's output flows straight through.

---

## Why this phase exists

Concrete pain points, in order:

1. **The dashboard hotkey is unconfigurable in-game.** The
   only way to change it today is to hand-edit
   `Data/SKSE/Plugins/NarrativeEngine.ini` and restart the
   game. That's fine for us during development, but it's a
   dead end the first time anyone else tries the mod — the
   INI file lives inside MO2's virtualization and its exact
   location isn't discoverable from the game.
2. **The mod has no in-game identity.** A player who
   installs the plugin and boots the game has no way to see
   "yes, NarrativeEngine is running, and here's what version
   it is" without opening the log. The MCM page's left
   column solves that at negligible cost.
3. **Input-space mismatch waiting to happen.** The moment
   SkyUI's `AddKeyMapOptionST` starts delivering rebinds
   back to C++, we'd have SkyUI giving us DirectX scan
   codes and our `HotkeySink` comparing VK codes translated
   from other scan codes. Cleaner to normalize the whole
   pipeline on scan-code space now — before there's a live
   rebind writer whose values would be silently
   misinterpreted.

---

## Scope

### In scope

- One new **`.esp` quest form** (`_ne_MCMQuest`) carrying an
  attached Papyrus MCM script (`_ne_MCM`) that extends
  `SKI_ConfigBase`. The quest is start-game-enabled, no
  stages, and carries one ReferenceAlias — the standard
  SkyUI Player alias with `SKI_PlayerLoadGameAlias`
  attached, which is what fires the script's
  `OnGameReload` hook on save-load.
- The **`_ne_MCM.psc` script** itself, implementing the
  imperative SkyUI hooks: `OnConfigInit` (mod name, one
  page), `OnPageReset` (layout — two columns of options),
  `OnKeyMapChangeST` (rebind handler), `OnDefaultST` (reset
  gesture handler), `OnGameReload` (propagate persisted
  binding back to C++ on save load).
- A tiny **C++ ModEvent sink** that receives hotkey-change
  ModEvents from Papyrus and updates the two Settings
  fields (`dashboardHotkeyDXSC`, `dashboardHotkeyModifiers`)
  in place. No new cosave record — the values live in
  Papyrus save data via auto properties.
- **Removal of `Settings::ApplyMcmOverride`** — the
  `Data/MCM/Settings/NarrativeEngine.ini` read path was
  designed for MCM Helper's INI output and has no consumer
  under the SkyUI-imperative approach. Dropping it avoids
  two competing MCM-value sources and simplifies
  `Settings::Load`.
- A **C++ input-space normalization** step: rename the
  `dashboardHotkey*` fields to name their real content
  (DirectX scan code, SkyUI-convention modifier bitmask),
  rework `HotkeySink` to compare in scan-code space
  directly, update the plugin INI's `[Dashboard]` section
  and its defaults. Documented in **Core concepts →
  Input-space normalization** below.
- Verification that the MCM page appears in SkyUI, that a
  rebind persists across save/reload, and that a fresh
  install with no rebind ever performed falls back cleanly
  to the plugin INI defaults.

### Deferred (explicitly out)

- **More MCM controls.** No tuning of Director cadence, no
  per-beat enable toggles, no beat-cooldown knobs, no
  debug-mode toggle. Any of those are legitimate future
  additions, but each of them either duplicates the INI
  surface that already exists (per the CLAUDE.md warning
  about MCM globals for every setting) or opens a
  round-trip design question about MCM ↔ INI precedence
  that isn't worth answering for this phase.
- **Multi-page or tabbed MCM.** One page, two columns. If
  the phase runs long, the page can grow; it can't grow
  into multiple pages.
- **MCM-driven dashboard content.** The dashboard's Dispatch
  tab already lets the player toggle beats mid-session. No
  duplicate surface in MCM.
- **Localization.** English strings are hard-coded in the
  Papyrus. Anyone shipping a localized build later can
  convert the strings to `$NE_MCM_...` tokens and drop in
  translation files under `Interface/Translations/`; the
  schema changes are localized to the MCM script.

---

## Core concepts

### Imperative SkyUI MCM as the vehicle

SkyUI's MCM framework is driven by a Papyrus script that
extends `SKI_ConfigBase` and overrides three main hooks:

- `OnConfigInit()` — called once when SkyUI discovers the
  page. Sets the mod name and any one-time initialization
  (default values on freshly-installed profiles).
- `OnPageReset(string page)` — called every time the
  player opens the page. Rebuilds the page's option layout
  from scratch by calling `AddHeaderOption`,
  `AddTextOption`, `AddKeyMapOptionST`, etc. Because it
  runs on every open, it can reflect changes to the
  current binding immediately.
- Per-option event handlers registered via **"state"**
  keywords in Papyrus — `OnKeyMapChangeST` fires when the
  player rebinds the keymap option, `OnDefaultST` when
  they long-press it to reset. Each option is bound to a
  named state (`"DashboardHotkey"`) in
  `AddKeyMapOptionST` and the handler is written under a
  matching `state DashboardHotkey ... endState` block.

Values are persisted as **script auto properties** on the
MCM quest's script instance. The vanilla save file
serializes Papyrus scripts' properties automatically, so a
rebind survives save/load without any explicit persistence
work in Papyrus.

The C++ side learns about rebinds via a **ModEvent**: the
Papyrus rebind handler calls `SendModEvent("_ne_DashboardHotkeyChanged",
argString, argFloat)` with the new (dxsc, modifiers) tuple
packed into the two arguments, and a small C++ sink
registered at plugin init reads them out and updates
`Settings::Get()`'s two hotkey fields in place. The
ModEvent also fires from `OnGameReload()` on save load, so
the C++ side re-syncs its in-memory values to whatever the
Papyrus save carried.

No MCM Helper plugin. No `config.json`. No translation
files. No separate SKSE dependency beyond SkyUI (which the
player already has for the MCM framework itself).

### Two-column layout

SkyUI's MCM lays out options in a two-column grid by
default. The `OnPageReset` handler controls placement with
`SetCursorFillMode(TOP_TO_BOTTOM)` (fill left column
top-to-bottom, then right column top-to-bottom) and calls
options in order:

- Left column: `AddHeaderOption("About")`, then
  `AddTextOption("Version", "0.1.0 (dev)", OPTION_FLAG_DISABLED)`
  and `AddTextOption`s for author and description.
  Disabled-flagged text options render as read-only lines
  — no interactivity.
- Right column: `AddHeaderOption("Controls")`, then one
  `AddKeyMapOptionST("DashboardHotkey", "Dashboard Hotkey", ...)`
  bound to the two auto properties (main key + modifiers).

`SetCursorFillMode` before the About header, then
`SetCursorPosition(1)` before the Controls header, forces
the split cleanly. Exact call shape lives in the Papyrus
source in Step 2.

### The hotkey control and input-space normalization

**Today's shape (about to change; see Step 1 which is now
complete).** SKSE's button events carry a **DirectX scan
code** in `btn->GetIDCode()`. The old `HotkeySink`
translated that scan code to a **Windows VK code** via
`MapVirtualKeyW(sc, MAPVK_VSC_TO_VK)` and compared it
against `Settings::Get().dashboardHotkeyVK`, which the
plugin INI stored in VK space (default `118` == `VK_F7`).
Modifier state was read via `GetAsyncKeyState(VK_*)` and
compared against a bitmask whose convention was
`1=Ctrl, 2=Shift, 4=Alt`.

**Why it was about to break.** SkyUI's `AddKeyMapOptionST`
delivers the rebound key as a **DirectX scan code** (Skyrim's
native input space; the same integer SKSE's button event
already carries) via `OnKeyMapChangeST(int newKeyCode, ...)`.
If Papyrus stored that scan code and sent it to C++, and
C++ compared it against a translated-from-scan VK code,
they'd match only for keys where the scan-to-VK translation
happens to preserve the same integer — a subset that
excludes essentially every letter and function key. SkyUI's
own modifier bitmask (`Input.IsShiftHeld` etc.) also uses
convention `1=Shift, 2=Ctrl, 4=Alt` — Shift and Ctrl
swapped from our old scheme.

**Resolution.** The normalization work landed in Step 1
(complete): C++ now uses `dashboardHotkeyDXSC` (default `65`
= `DIK_F7`), `HotkeySink` compares scan codes directly, and
the `kMod*` constants match SkyUI convention. When Papyrus
starts sending rebinds via ModEvent in Step 2, the values
flow through untouched.

### Persistence: Papyrus properties + ModEvent handshake

The rebound key needs to survive both save/load and
game-restart. Options considered:

- **Papyrus auto properties + ModEvent to C++.** ✅
  Chosen. Values live in the vanilla save via Papyrus's
  automatic property serialization. On save load, SkyUI
  fires `OnGameReload` on the MCM script; the handler
  re-sends the current property values to C++ via
  ModEvent, and C++ updates its in-memory Settings.
- **Papyrus writes an INI directly.** ❌ Requires
  PapyrusUtil or JContainers as an additional dependency.
  Overkill for two integers.
- **ESP-side `GlobalVariable` forms.** ❌ Duplicates state
  the Papyrus script already carries. Would still need a
  ModEvent (or `RE::TESGlobal` lookup) to reach C++.

The chosen approach keeps the SKSE cosave untouched — no
new cosave record, no `OnSave`/`OnLoad`/`OnRevert` glue on
the C++ side. The Papyrus save data does the persistence
work.

**Race window at boot.** When the game boots, C++'s
`Settings::Load` runs at `kDataLoaded` and populates the
hotkey fields from the plugin INI defaults. If the player
then loads a save whose Papyrus data carries a rebound
hotkey, the `_ne_DashboardHotkeyChanged` ModEvent from
`OnGameReload` overwrites the INI-loaded values within one
Papyrus tick of the load. Between "game booted" and "save
loaded" the plugin INI hotkey is authoritative — but the
dashboard can't open before a save loads anyway (nothing
to display), so this is inert.

### The Papyrus script shape

`_ne_MCM.psc` is not boilerplate under this approach —
it's a real 50-to-80-line script. Full source lands in
Step 2, but the shape is:

```papyrus
Scriptname _ne_MCM extends SKI_ConfigBase

; --- persisted binding ---
int  Property DashboardHotkeyDXSC       = 65 Auto  ; DIK_F7
int  Property DashboardHotkeyModifiers  = 0  Auto  ; SkyUI convention

; --- SKI_ConfigBase overrides ---
Event OnConfigInit()
    ModName = "NarrativeEngine"
    Pages   = new string[1]
    Pages[0] = ""    ; single unnamed page
EndEvent

Event OnPageReset(string page)
    SetCursorFillMode(TOP_TO_BOTTOM)

    ; Left column
    AddHeaderOption("About")
    AddTextOption("NarrativeEngine", "v0.1.0 (dev)",  OPTION_FLAG_DISABLED)
    AddTextOption("Author",          "Patrick VanDusen", OPTION_FLAG_DISABLED)
    AddTextOption("Description",     "AI Director for Skyrim (SkyrimNet)", OPTION_FLAG_DISABLED)

    ; Right column
    SetCursorPosition(1)
    AddHeaderOption("Controls")
    AddKeyMapOptionST("DashboardHotkey", "Dashboard Hotkey", \
                      DashboardHotkeyDXSC, DashboardHotkeyModifiers)
EndEvent

state DashboardHotkey
    Event OnKeyMapChangeST(int newKeyCode, string conflictControl, string conflictName)
        DashboardHotkeyDXSC = newKeyCode
        ; SkyUI packs held modifiers into a separate call; capture via Input
        int mods = 0
        if Input.IsKeyPressed(42)  || Input.IsKeyPressed(54) ; L/R Shift
            mods += 1
        endif
        if Input.IsKeyPressed(29)  || Input.IsKeyPressed(157) ; L/R Ctrl
            mods += 2
        endif
        if Input.IsKeyPressed(56)  || Input.IsKeyPressed(184) ; L/R Alt
            mods += 4
        endif
        DashboardHotkeyModifiers = mods
        SendHotkeyChangedEvent()
        SetKeyMapOptionValueST(newKeyCode)
    EndEvent

    Event OnDefaultST()
        DashboardHotkeyDXSC = 65
        DashboardHotkeyModifiers = 0
        SendHotkeyChangedEvent()
        SetKeyMapOptionValueST(65)
    EndEvent
endState

Event OnGameReload()
    Parent.OnGameReload()
    SendHotkeyChangedEvent()
EndEvent

Function SendHotkeyChangedEvent()
    int handle = ModEvent.Create("_ne_DashboardHotkeyChanged")
    if handle
        ModEvent.PushInt(handle,   DashboardHotkeyDXSC)
        ModEvent.PushInt(handle,   DashboardHotkeyModifiers)
        ModEvent.Send(handle)
    endif
EndFunction
```

Two things worth flagging in this sketch:

- `AddKeyMapOptionST`'s modifier support is spotty across
  SkyUI versions. The modifier-capture logic above reads
  the held state at rebind time via `Input.IsKeyPressed`
  as a portable fallback; if the version of SkyUI in the
  build load order supports a `newModifiers` parameter on
  `OnKeyMapChangeST`, we'll use that instead and drop the
  manual capture. Verify at Step 2 authoring time.
- `Input.IsKeyPressed(scanCode)` requires SKSE (which is
  a hard dependency anyway). If it turns out the exact
  script name is different in the current SkyUI SDK
  (`Input` vs `SKSE_Input` vs similar), the import
  statement gets adjusted; the logic doesn't change.

### The C++ ModEvent sink

New helper module (~50 lines total) with two entry points:

- `MCMEventSink::Initialize()` — called from `Plugin.cpp`'s
  `kDataLoaded` handler, after `Settings::Load()`.
  Registers a `RE::BSTEventSink<RE::TESModEvent>` filtered
  on `eventName == "_ne_DashboardHotkeyChanged"`.
- Sink's `ProcessEvent` — reads the two ints packed into
  the ModEvent's arg0 (`strArg` — but Papyrus's
  `ModEvent.PushInt` places ints in `numArg` slots; check
  the actual struct layout in `RE::TESModEvent`), updates
  `Settings::Get().dashboardHotkeyDXSC` and
  `dashboardHotkeyModifiers` in place, logs the change.

The two Settings fields are read from a non-main thread
(`HotkeySink::ProcessEvent`, which fires on the input
thread) and written from the main thread (ModEvent
handlers run on Papyrus's VM thread which marshals to the
main thread). To keep this correct without introducing a
mutex, promote the two fields to `std::atomic<int>` and
`std::atomic<std::uint8_t>` — the memory-order requirements
are trivial (relaxed loads and stores) and the reads in
`HotkeySink` are already single-value.

Actually, on reflection, keep them as plain `int` /
`std::uint8_t` for now. A stale read from the input thread
during a rebind is a single mis-interpreted keypress and
no crash; adding atomics is over-engineering for that
risk. Revisit if it ever bites.

### SkyUI SDK as a build-time prerequisite

The Papyrus script needs `SKI_ConfigBase.psc` visible to
the CK's Papyrus compiler at authoring time (and to
`build.ps1`'s Papyrus batch compile). SkyUI's SDK download
provides this. The build environment already has SkyUI
present at runtime for its MCM framework; the SDK is
strictly a build-time addition.

Two placement options:

1. **Install SkyUI SDK as an MO2 mod.** Enable it in the
   modlist. VFS layers its `Source\Scripts\SKI_*.psc` into
   `Data\Source\Scripts\` for the CK and build.ps1 to
   find. Recommended — same discipline as any other
   authoring dependency.
2. **Copy `SKI_ConfigBase.psc` (and any `.psc` it imports)
   into `esp/Source/Scripts/` in this repo.** Standalone
   but couples us to a specific SkyUI SDK snapshot.

The plan assumes option 1. If SkyUI SDK isn't available
when Step 2 lands, fall back to option 2 with a note in
this doc.

---

## Settings

**Renamed keys (semantic realignment; landed in Step 1):**

- `dashboardHotkeyVK` → `dashboardHotkeyDXSC`
- INI: `[Dashboard] iHotkeyVK` → `[Dashboard] iHotkeyDXSC`
- Default value: `118` (VK_F7) → `65` (DIK_F7)

**Semantic realignment on the modifier bitmask (landed in
Step 1; same field name, same INI key, reshuffled
constants to match SkyUI):**

- `kModShift = 1` (was `2`)
- `kModCtrl  = 2` (was `1`)
- `kModAlt   = 4` (unchanged)

**Removed at Step 2:**

- `Settings::ApplyMcmOverride` — the
  `Data/MCM/Settings/NarrativeEngine.ini` read path. Its
  original consumer (MCM Helper) is no longer part of the
  plan; the imperative SkyUI approach delivers rebinds via
  ModEvent, not via a second INI. The file header comment
  in `Settings.h` updates accordingly.

**Unchanged:**

- Every other Settings field. This phase touches nothing
  outside `[Dashboard]`.

---

## Persistence

Nothing new in the SKSE cosave. The rebound hotkey lives
in the vanilla save via Papyrus's auto-property
serialization on the `_ne_MCM` script attached to
`_ne_MCMQuest`. On save load, `OnGameReload` fires the
`_ne_DashboardHotkeyChanged` ModEvent and C++ picks up the
persisted values.

The `_ne_MCMQuest` quest form is start-game-enabled with no
stages; its Papyrus save footprint is the two integer
properties plus the one Player ReferenceAlias.

---

## File map

New:

- `esp/Source/Scripts/_ne_MCM.psc` — the MCM script. See
  **The Papyrus script shape** above for the sketch;
  final source lands in Step 2.
- `include/MCMEventSink.h` — public API for the ModEvent
  sink (`Initialize`, `Shutdown` if needed for lifecycle
  parity — likely just `Initialize`).
- `src/MCMEventSink.cpp` — implementation.

Reshaped:

- `include/Settings.h` — update file header comment to
  reflect removal of the MCM INI override path.
- `src/Settings.cpp` — remove `ApplyMcmOverride`, remove
  its call site in `Load`, remove the
  `kMcmIniPath` constant. Leave the "resolved hotkey" log
  line intact.
- `src/Plugin.cpp` — call `MCMEventSink::Initialize()`
  from `kDataLoaded` after `Settings::Load()`.

CK / ESP work:

- `_ne_MCMQuest` — new start-game-enabled quest form,
  priority 0, no stages, one Player ReferenceAlias with
  `SKI_PlayerLoadGameAlias` attached, and the `_ne_MCM`
  script on the quest itself.

Preserved unchanged:

- The Step 1 C++ input-space refactor (settings rename,
  `HotkeySink` scan-code compare, INI comments) — already
  complete and independent of the MCM approach.
- Every other C++ file, Papyrus script, and dashboard
  source.

---

## Implementation plan

Sequential. Step 1 is complete. Step 2 is entirely
Claude's work (Papyrus + C++). Step 3 is entirely user
work (CK). Step 4 is joint verification.

Step order:

1. ✅ **C++ input-space normalization** — Settings /
   HotkeySink / INI refactor. Complete. Landed
   independently of the MCM approach and takes effect for
   the current INI-only configuration path.
2. **Author `_ne_MCM.psc` + MCM ModEvent sink** — write
   the Papyrus MCM script and the C++ sink that receives
   its rebind ModEvents. Also remove `ApplyMcmOverride`
   from `Settings.cpp`. Verifiable at the build level
   (both compilers succeed) but not user-visible yet — the
   quest form and attachment happen in Step 3.
3. **CK: quest form + attach script** — create
   `_ne_MCMQuest` in the CK, attach the compiled
   `_ne_MCM` script, save the ESP. First point at which
   the MCM page appears in SkyUI's list.
4. **Verify end-to-end** — walk the three verification
   scenarios below (page visible with correct layout,
   rebind persists across save/reload, fresh install
   falls back to defaults).

---

### Step 1 — C++ input-space normalization

- [x] Complete

**[CLAUDE]**

Landed 2026-07-18. See **The hotkey control and
input-space normalization** above for the design; the
concrete diff is captured in the commit that renamed
`dashboardHotkeyVK` → `dashboardHotkeyDXSC`, reshuffled
the `kMod*` constants, rewrote `HotkeySink` to compare
scan codes directly, and updated the plugin INI's
`[Dashboard]` block.

**Verify:** all four sub-tasks in the prior Step 1 draft
passed (default F7 opens dashboard; INI-configured scan
code overrides work; modifier combos work; no old
symbols remain in `src/`, `include/`, `statics/`).

---

### Step 2 — Author `_ne_MCM.psc` + C++ ModEvent sink

- [x] Complete

**[CLAUDE]**

**Goal:** Ship the Papyrus MCM script and the C++ side
that consumes its rebind ModEvents. Also drop the
now-defunct `Settings::ApplyMcmOverride` path. After this
step, the code exists for a working MCM page but no page
appears in-game yet — that requires the quest form and
attachment from Step 3.

**Files:**

- `esp/Source/Scripts/_ne_MCM.psc` — new. Full imperative
  MCM script per the shape in **The Papyrus script
  shape** above. Precise SkyUI-version-dependent details
  (`OnKeyMapChangeST` signature, `Input` script name)
  determined at authoring time against the SDK on disk.
- `include/MCMEventSink.h` — new. `Initialize()` entry
  point.
- `src/MCMEventSink.cpp` — new. `RE::BSTEventSink<RE::TESModEvent>`
  implementation.
- `src/Plugin.cpp` — call `MCMEventSink::Initialize()`
  from `kDataLoaded` after `Settings::Load()`.
- `include/Settings.h` — update file header comment: the
  `Data/MCM/Settings/NarrativeEngine.ini` line goes; the
  runtime override note becomes "rebinds via the MCM page
  land in-process via `_ne_DashboardHotkeyChanged`
  ModEvent, updating the two `[Dashboard]` fields in
  place."
- `src/Settings.cpp` — remove `ApplyMcmOverride`, its
  call site in `Load`, and the `kMcmIniPath` constant.
  Keep the resolved-hotkey log line.
- CMake — add `src/MCMEventSink.cpp` to the target file
  list.

**Sub-tasks:**

1. **Papyrus script.** Verify SkyUI SDK is available to
   the build environment (see **SkyUI SDK as a build-time
   prerequisite**). If missing, halt the step and prompt
   the user to install it before continuing.
2. Author `_ne_MCM.psc` per the sketch above. Confirm
   against the SDK's `SKI_ConfigBase.psc` on disk:
   - `OnKeyMapChangeST`'s exact signature in this SDK
     version. If it delivers modifiers directly (via a
     `newModifiers` int parameter), use that and drop the
     `Input.IsKeyPressed`-based manual capture. If not,
     keep the manual capture and note the SDK version in
     the script's file-header comment.
   - The `Input` script's fully-qualified name (some SDKs
     have it under a namespace prefix).
   - Whether `AddTextOption` accepts a
     `OPTION_FLAG_DISABLED` flag or requires a
     different constant name. Adjust as needed.
3. **C++ ModEvent sink.** In `MCMEventSink.cpp`:
   - Register a `RE::BSTEventSink<RE::TESModEvent>` on
     `RE::ScriptEventSourceHolder::GetSingleton()`.
   - `ProcessEvent` filters on
     `event->eventName == "_ne_DashboardHotkeyChanged"`.
     Papyrus's `ModEvent.PushInt` populates the
     `numArg` slot as a float — read
     `event->numArg` and cast to int for the DXSC (first
     PushInt) — but Papyrus only supports a single
     `numArg` per ModEvent, so the two ints need packing
     into one float or one goes via `strArg`. Simpler:
     use `SendModEvent(name, string strArg, float
     numArg)` and pack DXSC into `numArg`, modifiers into
     `strArg` as `"3"` / `"5"` etc. Revisit at
     authoring; the point is that the two 8-bit integers
     fit trivially into one ModEvent regardless of the
     specific packing shape.
   - Update `Settings::Get()`'s fields in place. Since
     `Settings::Get` returns `const Config&`, add a small
     internal setter (`Settings::UpdateDashboardHotkey(int
     dxsc, std::uint8_t mods)`) that touches the
     singleton without exposing broader mutation.
   - Log the change:
     `logger::info("MCMEventSink: dashboard hotkey rebound
     DXSC={} mods={}", dxsc, mods)`.
4. **Wire the sink** into `Plugin.cpp`'s `kDataLoaded`
   handler.
5. **Remove `Settings::ApplyMcmOverride`.** Delete the
   function, its call site in `Load`, the `kMcmIniPath`
   constant, and the "applied MCM override" log line.
   Update the file header comment in `Settings.h` per
   the file map above.
6. Add `src/MCMEventSink.cpp` to CMake.
7. `pwsh -File format.ps1` — resolve every finding.
8. `pwsh -File build.ps1 build` — verify both compilers
   succeed. Papyrus compile will fail if SkyUI SDK isn't
   in the source path; that's the signal to install it
   (see sub-task 1).

**Specifics:**

- The Papyrus script's default property values (`= 65
  Auto`, `= 0 Auto`) match `Settings.h`'s baked-in
  defaults, so a first-time page open shows the same
  binding the plugin INI has been enforcing. No
  divergence.
- `OnGameReload()` calling `Parent.OnGameReload()` first
  is important — `SKI_ConfigBase` uses that hook for its
  own registration bookkeeping. Skipping the super call
  breaks SkyUI's page registration.
- The ModEvent sink doesn't need a shutdown — SKSE tears
  it down at plugin unload via the singleton. If parity
  with the other sinks in the codebase warrants an
  explicit `Shutdown()`, add one; otherwise, `Initialize`
  is enough.

**Verify:**

- `pwsh -File build.ps1 build` succeeds. Papyrus compile
  reports `_ne_MCM.psc` succeeded, 0 errors, 0 warnings.
- `pwsh -File format.ps1` succeeds clean.
- Boot Skyrim without the Step 3 CK work yet in place
  (script compiled but not attached to any form). Log
  shows `MCMEventSink: initialized`. No MCM entry appears
  in SkyUI's list (the script exists but no quest form
  hosts it). Log shows no
  `_ne_DashboardHotkeyChanged` traffic — nothing to send
  the event because the script isn't running.
- Grep sweep: `ApplyMcmOverride`, `kMcmIniPath`, and any
  reference to `Data/MCM/Settings/NarrativeEngine.ini`
  return no matches under `src/` or `include/`.

---

### Step 3 — CK: quest form + Player alias + attach scripts

- [x] Complete

**[USER]**

**Goal:** Add the `.esp`-side machinery SkyUI needs to
find our MCM script and to fire its `OnGameReload` hook
on save-load. This step makes the MCM page visible
in-game and ensures the C++ side re-syncs to the persisted
Papyrus binding whenever a save loads.

**Files:**

- `_ne_MCMQuest` — new quest form in the CK.
- One ReferenceAlias on that quest — "Player", pointing at
  PlayerRef, with `SKI_PlayerLoadGameAlias` attached.

**Sub-tasks:**

1. In the Creation Kit, open `NarrativeEngine.esp`.
2. Create a new Quest form. EditorID: `_ne_MCMQuest`.
   Settings:
   - **Start Game Enabled** — checked.
   - **Priority** — 0.
   - **Quest Data** flags — leave all unchecked (no
     "Run Once", no journal, no objectives).
   - No stages, no objectives, no dialogue, no scene
     edits.
3. On the Quest's "Scripts" tab, attach the `_ne_MCM`
   script (compiled by Step 2's build). If the compiled
   `_ne_MCM.pex` isn't visible to the CK's Scripts panel,
   run `build.ps1 build` again to make sure the deployed
   mod folder has the current `.pex`.
4. On the Quest's "Quest Aliases" tab, create a new
   ReferenceAlias:
   - **Alias Name** — `Player`.
   - **Fill Type** — Specific Reference. Cell:
     `(any)`, Ref: `PlayerRef`.
   - On the alias's "Scripts" panel, attach
     `SKI_PlayerLoadGameAlias`. The `.pex` ships with
     SkyUI's SDK; it's not one we author. The alias's
     job is to receive `OnPlayerLoadGame`, which
     internally calls `_ne_MCM::OnGameReload` on the
     parent quest — the mechanism that lets us re-sync
     C++'s Settings to the persisted Papyrus binding on
     every save load.
5. Save the ESP.

**Verify:**

- ESP saves cleanly in the CK, no warnings.
- Boot Skyrim with the updated ESP loaded. Wait ~10
  seconds after main menu; open MCM (via SkyUI). A
  "NarrativeEngine" entry appears in the mod list.
- Open the NarrativeEngine page. Layout:
  - Left column: "About" header, "NarrativeEngine v0.1.0
    (dev)", "Author Patrick VanDusen", "Description AI
    Director for Skyrim (SkyrimNet)" — all read-only.
  - Right column: "Controls" header, "Dashboard Hotkey"
    row showing `F7`.
- Log shows `MCMEventSink: dashboard hotkey rebound
  DXSC=65 mods=0` shortly after save load (that's the
  `OnGameReload`-driven initial sync). Same values as
  the INI default, so no observable change to the
  hotkey.

---

### Step 4 — End-to-end verification

- [ ] Complete

**[CLAUDE + USER]**

**Goal:** Walk the three scenarios that together prove
the MCM page works and the fallback is intact.

**Sub-tasks:**

1. **Page visible + rebind persists across save/reload.**
   - Boot Skyrim with the mod loaded. Open MCM →
     NarrativeEngine. Verify layout (per Step 3 verify).
   - Rebind the dashboard hotkey to Ctrl+`8`.
     - Click the "Dashboard Hotkey" row. Press Ctrl+`8`.
     - Log shows `MCMEventSink: dashboard hotkey rebound
       DXSC=9 mods=2` (DIK_8=9, SkyUI-Ctrl=2).
   - Press Ctrl+`8` in-game. Dashboard toggles. Press F7
     — nothing happens (old default no longer bound).
   - Save the game. Exit to main menu. Reload the save.
   - Log shows a second
     `MCMEventSink: dashboard hotkey rebound DXSC=9
     mods=2` shortly after load (from `OnGameReload`).
   - Press Ctrl+`8` in-game. Dashboard toggles.

2. **Fresh install, MCM never opened.**
   - Start a new game.
   - MCM page appears in SkyUI's list. Don't open it.
   - Press F7 (plugin INI default). Dashboard toggles.
     No ModEvent traffic yet because the MCM script's
     `OnPageReset` hasn't been called (page not opened),
     but `OnGameReload` still fires on save load, so
     initial-sync ModEvent traffic will be present with
     the default binding.

3. **Grep sweep.** ✅ Complete.
   - `grep -RIn "ApplyMcmOverride\|kMcmIniPath\|Data/MCM/Settings"
     src include statics` returns no matches. Historical
     mentions in `docs/implementation/PHASE_01_MVP.md`
     and prior versions of this doc are expected.
   - `grep -RIn "MCM Helper\|MCMHelper" src include statics`
     returns no matches (the phase pivoted away from MCM
     Helper before landing any references). One README.md
     match remains and is preserved — it describes
     IntelEngine's (prior-art reference project)
     deployment layout, not ours.

**Verify:** all three sub-tasks pass; the Step 2 and
Step 3 checkboxes flip to complete alongside this one.

---

## Done condition

- All four steps above have their checkboxes marked
  complete.
- The MCM page appears in SkyUI when the mod is loaded;
  its layout is two columns as designed; the keymap
  control accepts rebinds including modifier combos.
- A rebind persists across save/reload via Papyrus
  auto-property serialization; the `_ne_DashboardHotkeyChanged`
  ModEvent syncs C++ to the persisted value on
  `OnGameReload`.
- `pwsh -File build.ps1 build` succeeds cleanly.
- `pwsh -File format.ps1` succeeds cleanly.
- No source or config file references `dashboardHotkeyVK`,
  `iHotkeyVK`, `MapVirtualKeyW`, `ApplyMcmOverride`,
  `kMcmIniPath`, `Data/MCM/Settings/`, or `MCM Helper`
  (matches in `docs/implementation/PHASE_01_MVP.md` and
  prior revisions of this doc are expected and preserved
  as historical).
- The `_ne_MCMQuest` quest is present in the ESP with its
  attached `_ne_MCM` script; the script compiles cleanly
  against SkyUI's `SKI_ConfigBase`.
