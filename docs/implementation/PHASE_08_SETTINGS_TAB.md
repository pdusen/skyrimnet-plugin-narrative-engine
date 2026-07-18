# Phase 08 — Dashboard Settings Tab

Phase 07 shipped an MCM page whose sole job is rebinding the
dashboard hotkey. That page uses MCM Helper as its vehicle,
which brought with it a nice side effect the plugin now
depends on: **cross-save persistence via
`Data/MCM/Settings/NarrativeEngine.ini`**. Any value written
to that INI survives save/load, character switches, and even
New Game — exactly the expectation players have for
"settings I set once."

Phase 08 leverages that same file as the single-source-of-truth
override store for a wider set of user-facing settings, but
puts the input UI for those settings **in the PrismaUI
dashboard** rather than the MCM. A new **Settings** tab is
added to the dashboard (fifth tab, after Dispatch), containing
sliders and toggles for the settings players are most likely
to want to tweak: whether the Director ticks at all, how often
it ticks, and how long each of the five narrative-cycle phases
should ideally last before the beat system starts considering
its exit. The MCM page keeps doing what it does — hotkey
rebind + mod-identity display — and the new tab writes to the
same INI file MCM Helper writes to, so the two surfaces share
one persistence layer.

The plugin INI (`Data/SKSE/Plugins/NarrativeEngine.ini`) does not disappear. Its role clarifies: it's the
**author-defaults + on-disk INI-editing surface** — the file a modder editing the mod's source tree hand-tunes. The
MCM INI (`Data/MCM/Settings/NarrativeEngine.ini`) becomes a **universal override layer**: any Config field named in
it overrides the plugin-INI value. Present keys override; absent keys fall through to the plugin INI, which in turn
falls through to the baked-in `Config` defaults. This applies across the board — including for the fifty-odd knobs
Phase 08 leaves out of every UI. A modder or advanced player who hand-edits an MCM-INI entry for a knob that isn't
in either UI surface gets an override that survives reboot; the plugin INI stays intact as the authored default.

The C++ refactor supporting this cascade is a small structural change: the current per-key `Get*Value` calls in
`LoadPluginIni` get lifted into a helper that takes a `CSimpleIniA&` and populates every `Config` field. `Load` then
calls the helper twice — once against the plugin INI (populates all defaults), once against the MCM INI (overrides
where keys are present). No sprawling parallel key list; one enumeration, invoked in two contexts.

---

## Why this phase exists

Concrete pain points, in order:

1. **The Director's most-visible knobs are all buried in the
   plugin INI.** How often it ticks, how long each phase
   should feel, whether the tick is even running — all of it
   requires the player to hand-edit `Data/SKSE/Plugins/NarrativeEngine.ini`
   and restart the game. Phase 01 accepted that; Phase 06's
   beat-system refactor made it worse (more knobs); Phase 07
   didn't fix it (it fixed the *hotkey*, which was one of the
   few knobs a player would actually want but not the most
   impactful).
2. **The tick killswitch is session-only.** The Dispatch tab
   already has an "Enable Tick" checkbox. It flips
   `Tick::SetEnabled` in-process, but nothing persists. Reboot
   the game and the tick is on again — no matter what the
   player wanted. For a debug knob that's fine; for a "I don't
   want the Director doing anything right now" affordance
   (running an intense combat gauntlet, testing another mod)
   it's a footgun.
3. **The MCM page is the wrong place for most settings.** It's
   a Skyrim-menu screen buried under Escape → Mod
   Configuration. The dashboard is what the player actually
   looks at — the settings the player edits should be next to
   the state those settings shape. Ideal phase durations live
   in the same window as the phase-progress panel; the tick
   toggle lives in the same window as the tick's decision log.
4. **`Data/MCM/Settings/NarrativeEngine.ini` is already the right shape for player overrides.** MCM Helper
   introduced it in Phase 07 for the hotkey; extending its use to other settings costs one C++ writer plus a
   restructured reader. Two systems reading and writing one file is simpler than two files with different scopes.
5. **The current reader is bifurcated.** `LoadPluginIni` reads every Config field from the plugin INI;
   `ApplyMcmOverride` reads a hand-picked four keys from the MCM INI. The two lists have to be kept in sync by
   inspection, and every new UI-facing knob would require adding a line to `ApplyMcmOverride` that duplicates a line
   already in `LoadPluginIni`. Lifting both into a single per-key enumeration invoked against two `CSimpleIniA`
   instances collapses the duplication and — as a first-order benefit — makes every plugin-INI key MCM-overridable
   automatically, whether it has a UI surface or not.

---

## Scope

### In scope

- One new **Settings tab** on the dashboard, added to the
  tab bar between Dispatch and (implicitly) the tab list
  end. Layout is a single scrollable column of grouped
  sections, not the two-column MCM layout.
- **Section 1 — General.**
  - **Debug Mode** — bool toggle. Mirrors `Config::debugMode`.
  - **Dashboard Hotkey** — current-binding display (e.g. `F7` or `Ctrl+Shift+F9`) followed by a **Rebind** button.
    Clicking Rebind opens a modal ("Press any key to bind; hold modifiers while pressing to include them. Esc
    cancels."). The next non-modifier keypress is captured in native DirectX-scan-code space via the existing
    `HotkeySink` (put into a temporary "capture mode" by a new `ne_beginHotkeyRebind` listener), modifiers are read
    live via `GetAsyncKeyState`, and the four MCM-INI keys (`iHotkeyDXSC` + three `bHotkey*` bools) are written via
    `WriteMcmOverride`. The modal is a pure function of `state.settings.dashboard_hotkey_capture_active` — it opens
    when the flag becomes true and closes when the flag becomes false, driven entirely by PushFullState. Rebinds
    from this surface share the same MCM-INI persistence layer as MCM Helper's own hotkey control, so a rebind
    survives reboot and either surface reads the same value on next boot.
- **Section 2 — Narrative Director.**
  - **Enable Narrative Tick** — bool toggle. Mirrors the
    Dispatch tab's existing "Tick Enabled" checkbox. The
    Dispatch checkbox stays where it is (Phase 06 verified it
    there and the tab's whole purpose is dispatch-time
    debugging); the new toggle in Settings tab is a second
    mount point onto the same underlying state. Persistence
    is added on the C++ side so the setting survives reboot
    — a change from either surface writes to the MCM INI, and
    `Tick::Start` seeds itself from `Settings::Get()` at boot
    rather than defaulting to `true` unconditionally.
  - **Tick Interval (seconds)** — integer slider, 10..600.
    Mirrors `Config::tickIntervalSeconds`. This is the wall-
    clock interval between Director evaluations. Higher =
    fewer LLM calls per real minute = cheaper; lower = the
    Director notices state changes sooner. Ship default 90;
    plugin INI currently sets 30 as a dev override, which the
    Settings tab exposes but the "reset to default" gesture
    resets to 90.
- **Section 3 — Narrative Cycle Phase Durations.**
  - **Exposition (seconds)** — integer slider, 60..1200.
    Default 330.
  - **Rising Action (seconds)** — integer slider, 60..1200.
    Default 225.
  - **Climax (seconds)** — integer slider, 30..600. Default
    90.
  - **Falling Action (seconds)** — integer slider, 60..1200.
    Default 225.
  - **Resolution (seconds)** — integer slider, 60..1200.
    Default 330.
  - Total at defaults: 1200s (20 minutes) per cycle — same
    as today's INI defaults. See
    `Config::idealDuration*` and PHASE_06 for the beat-
    system semantics that read these values.
- A **`Settings::WriteMcmOverride(...)` helper** on the C++ side that: opens
  `Data/MCM/Settings/NarrativeEngine.ini`, mutates one or more keys, saves back, and updates the in-memory `Config`
  singleton to match. Called by every new JS→C++ listener that backs a Settings-tab control. Write surface is narrow
  by design — only the keys the UI can change.
- **A restructured reader.** Both `LoadPluginIni` and `ApplyMcmOverride` collapse into a shared file-local helper
  `ReadIniInto(CSimpleIniA& ini, Config& dst)` that enumerates every Config field once. `Settings::Load` invokes it
  twice: first against the plugin INI (populates from author defaults), then against the MCM INI (overrides where
  keys are present). Every Config field named in the MCM INI is honored — including fields that no UI surface exposes.
- **A boot-time seed for `Tick::g_enabled`.** `Tick::Start` reads `Settings::Get().tickEnabled` (a new field,
  default `true`) instead of hardcoding `true`, so a player who disabled the tick and rebooted stays disabled.
- **Six new JS→C++ listeners** on the dashboard: `ne_setDebugMode`, `ne_setTickInterval`, `ne_setPhaseIdealDuration`,
  `ne_beginHotkeyRebind`, `ne_cancelHotkeyRebind`, plus an extension to `ne_setTickEnabled` (already exists) so it
  also writes the MCM INI.
- **`HotkeySink` gains a temporary capture mode.** When a `ne_beginHotkeyRebind` arrives, an atomic flag in
  `DashboardUIManager` flips true. The next button-down event whose scan code isn't one of the six modifier keys is
  consumed by the capture path (writes the four hotkey MCM-INI keys, flips the flag back), rather than being
  compared against the current binding. Modifier scan codes seen while the flag is set are ignored — they contribute
  to the assembled bitmask via a `GetAsyncKeyState` probe at capture time, they don't stand as the primary key.
  Modifier-only bindings ("just Left Alt") are not supported, and the modal's help text says so.
- **PushFullState carries the new fields** so the Settings
  tab renders live values. A new `settings` sub-object on
  `DirectorState` collects them; the tab reads from
  `state.settings` rather than reaching into the top-level
  status.
- **Verification** that: (a) each new control's on-screen value matches Settings after change; (b) each change
  lands in the MCM INI on disk; (c) rebooting the game preserves every change; (d) the plugin INI's values still
  function as first-install defaults when the MCM INI is absent or a key is missing; (e) removing the MCM INI
  restores plugin-INI behavior; (f) the Dispatch tab's Tick checkbox and the Settings tab's Tick toggle stay in
  sync live and after reboot; (g) an MCM-INI key added by hand for a knob that has no UI surface (e.g.
  `iBeatCooldownSeconds`) is honored on the next boot; (h) a rebind performed from the Settings tab writes the same
  MCM-INI keys MCM Helper writes to, is honored by the runtime `HotkeySink` immediately, and shows up correctly on
  the next boot of MCM's page.

### Deferred (explicitly out)

- **Per-beat enable toggles.** The Dispatch tab's per-row
  enable checkboxes stay session-only; the plugin INI's
  `bEnable*` keys stay the boot-time source. Not part of
  the Phase 08 scope the user set (which is: dashboard
  hotkey, the global tick toggle, and the five phase-
  duration sliders, plus author-chosen adjacent settings).
- **Per-beat cooldown / tuning knobs.** Bandit counts,
  spawn distances, letter word bounds, visit poll cadences
  — author-facing shape knobs where a wrong value silently
  degrades the LLM output. Stay in the plugin INI.
- **Alpha-canon "do not disturb" cell editor.** The
  `sDoNotDisturbCellEDIDsCSV` setting is a comma-separated
  list of cell editor IDs. Editing a textual list in-game is
  a bigger UI than a slider — a search-and-add flow, a
  "current list" chip cloud — and cell EDIDs aren't
  discoverable from the game without console. Leave to
  plugin-INI for authors; a future phase can add a
  "Restricted Cells" browser that reads live cell EDIDs from
  `RE::TESForm` state.
- **Threshold sliders (phase-advancement tension).** The
  `iAdvanceThresholdExposition`..`Resolution` values are
  narrative-shape tuning that individual players are
  unlikely to want to touch — get them wrong and phase
  advancement goes sideways in ways that aren't obviously
  the fault of the setting. Keep them in the plugin INI.
- **Multiple pages / sub-tabs on the Settings surface.** One
  scrollable page with grouped sections. If the setting list
  outgrows a comfortable scroll, a follow-up phase can add a
  side-nav; not now.
- **A "reset to defaults" gesture.** Each control has an
  implicit default, but no dedicated reset button. Players
  who want defaults can delete `Data/MCM/Settings/NarrativeEngine.ini`.
- **Any settings that aren't listed above.** Phase 08 stops
  at the seven-ish sliders/toggles specified. No creeping in
  of "one more useful knob."

---

## Core concepts

### Storage architecture: MCM INI as the universal override layer

The two-file cascade Phase 07 sketched hardens into an explicit contract that applies to **every** Config field, not
just those with a UI surface:

- **Plugin INI (`Data/SKSE/Plugins/NarrativeEngine.ini`).** Author-facing. Populates every field of `Config` at boot
  from its baked-in defaults. Contains fifty-plus tuning knobs — beat cooldowns, composer word bounds, poll cadences
  — most of which no Phase 08 UI surface exposes. The plugin INI is what a modder editing the mod's source hand-tunes.
- **MCM INI (`Data/MCM/Settings/NarrativeEngine.ini`).** Universal override layer. Any Config field named here
  overrides the plugin-INI value on the same key. Absent keys fall through — the plugin-INI value stands. Absent
  file entirely means "fresh install" and every field reads from the plugin INI. There is no restricted subset of
  "MCM-override-capable" keys — the reader iterates every field regardless of source.

`Settings::Load` runs this cascade in one place at `kDataLoaded`: reset to defaults, apply plugin INI via the shared
reader, apply MCM INI via the same shared reader. Order matters (MCM after plugin so its values win) and doesn't
change from Phase 07 — MCM Helper's INI has always been the last thing to run.

The Settings tab writes only to the MCM INI, and only to the keys it can edit. The plugin INI is treated as
read-only from the plugin's own perspective (it always has been; the plugin has never written to
`Data/SKSE/Plugins/NarrativeEngine.ini`). Keys in the MCM INI that lack a UI editor still work — they're populated
by the shared reader and take effect from the next boot.

### Read cascade + write single-target

The reader restructures around a shared helper. Today `LoadPluginIni` enumerates every field with a series of
`Get*Value` calls against `kPluginIniPath`, and `ApplyMcmOverride` re-enumerates four keys against `kMcmIniPath`.
Phase 08 lifts the enumeration into one file-local function:

```cpp
// Populates `dst` from every recognized key in `ini`. Each Get*Value call
// passes the current `dst.<field>` as its default — so on first invocation
// (against the plugin INI, with `dst` fresh-defaulted) missing keys land
// on the Config defaults, and on the second invocation (against the MCM
// INI, with `dst` already populated from the plugin INI) missing keys land
// on the plugin-INI values. That single default-passing convention gives
// the cascade its fall-through semantics without any conditional logic.
void ReadIniInto(CSimpleIniA& ini, Config& dst);
```

`Load` becomes:

```cpp
void Load() {
    g_config = Config{};

    CSimpleIniA plugin; plugin.SetUnicode();
    if (plugin.LoadFile(kPluginIniPath) >= 0) {
        ReadIniInto(plugin, g_config);
        logger::info("Settings: loaded from {}", kPluginIniPath);
    } else {
        logger::info("Settings: no plugin INI at {}; using defaults", kPluginIniPath);
    }

    CSimpleIniA mcm; mcm.SetUnicode();
    if (mcm.LoadFile(kMcmIniPath) >= 0) {
        ReadIniInto(mcm, g_config);
        logger::info("Settings: MCM overrides applied from {}", kMcmIniPath);
    }
    // ...standard log lines...
}
```

`ApplyMcmOverride` (still called by `MCMEventSink` on the `_ne_DashboardHotkeyChanged` event) now delegates to the
same helper — it opens `kMcmIniPath`, calls `ReadIniInto`, and logs. No hand-picked key list. Every Config field
named in the MCM INI is honored on that re-read.

The section names on the MCM INI mirror the plugin INI for every key. `ReadIniInto` doesn't know which INI it's
reading from; it just needs each key in the same `[Section] iKey` shape in both files.

**Existing `[Dashboard]` section stays put.** MCM Helper's `config.json` currently binds the hotkey settings to
`[Dashboard]` via its `id: "iHotkeyDXSC:Dashboard"` syntax, and Phase 07 wired the C++ reader to that section. Phase 08
does not rename this — cost is high (MCM config change + seed file change + INI-migration story for players who
already have a rebound key persisted under `[Dashboard]`), value is zero. `ReadIniInto` reads the hotkey keys from
`[Dashboard]` and every other key from its plugin-INI section; the two sets don't collide.

`Settings::WriteMcmOverride(...)` is the single writer:

```cpp
// Writes a subset of Config fields to the MCM INI. Reads the
// current file, mutates the specified keys, writes back,
// atomically. Also updates the in-memory Settings singleton
// so the same call site can trust that Settings::Get()
// reflects the write immediately (no wait for the next
// Load()). Never touches the plugin INI.
void WriteMcmOverride(const McmOverride& mutations);
```

`McmOverride` is a struct-of-`std::optional`s covering every key the Settings tab can write. A listener that changes
just one control fills one optional and leaves the rest empty; the writer only touches keys whose optional is engaged.
The write surface is deliberately narrower than the read surface — the reader honors every Config field, but the
writer only knows how to author the specific keys the UI edits.

The writer is call-site-cheap because SimpleIni handles the "preserve unknown keys" behavior for us — reading the
whole file into a `CSimpleIniA`, calling `Set*Value` on the mutations, and calling `SaveFile` preserves every other
key (including comments, MCM Helper's own bookkeeping keys if any, and hand-authored overrides for knobs the UI
doesn't edit). No append/replace parsing; SimpleIni does it.

### In-dashboard hotkey rebinding

Rebinding a hotkey from the browser side sounds fraught — PrismaUI has input focus while the dashboard is shown, so
a raw JS `keydown` event carries browser-space key identifiers (`e.code`, `e.keyCode`) rather than the DirectX scan
codes the runtime `HotkeySink` compares against. Building a JS→DIK mapping table in TypeScript would be ugly and
fragile (function keys behave oddly across browsers, dead keys and IME states leak through, and Chromium's
`e.code` values change over the Blink versions PrismaUI has bundled).

The cleaner path avoids the browser entirely: capture at the C++ input sink, in native DIK space, using the sink
that's already registered against `BSInputDeviceManager`. The dashboard's `HotkeySink` already receives button-down
events while PrismaUI has focus — that's how ESC-to-close works today. Adding a temporary "capture mode" to that
sink lets the same code path serve two purposes: normal-time binding matching, and rebind-time key capture.

The flow:

1. Player clicks **Rebind** in Settings. React invokes `window.ne_beginHotkeyRebind()` (no argument).
2. C++ listener flips `DashboardUIManager::g_hotkeyCaptureMode = true` (atomic) and calls `PushFullState`. The state
   blob's new `settings.dashboard_hotkey_capture_active` boolean turns true; the React modal opens as a pure
   function of that.
3. `HotkeySink::ProcessEvent` sees `g_hotkeyCaptureMode == true`. On the next non-modifier button-down:
   - The DIK scan code is captured directly from `btn->GetIDCode()`.
   - Held modifiers are probed via `GetAsyncKeyState(VK_SHIFT/CONTROL/MENU)` and packed into the three bool fields
     MCM Helper's INI schema expects (`bHotkeyShift`, `bHotkeyCtrl`, `bHotkeyAlt`).
   - `Settings::WriteMcmOverride` writes all four keys under `[Dashboard]`.
   - `g_hotkeyCaptureMode` flips back to false; `PushFullState` fires.
4. `settings.dashboard_hotkey_capture_active` turns false; the modal closes. The Dashboard Hotkey display shows
   the new binding.

Modifier scan codes seen while `g_hotkeyCaptureMode == true` are filtered — treating them as the primary key
would mean the modal closes the instant the player presses Shift on the way to pressing Shift+F7. The six DIK codes
to filter are Left/Right Shift (42, 54), Left/Right Ctrl (29, 157), Left/Right Alt (56, 184). This means "just a
modifier key with no primary" isn't a bindable combination; the modal's help text calls that out.

**Cancel paths.** ESC during capture cancels (does not bind — even though ESC is technically a scan code). React
"Cancel" button in the modal invokes `window.ne_cancelHotkeyRebind()`, which flips the flag off and pushes state.
`ToggleVisibility(hide)` also auto-cancels — if the player hides the dashboard mid-rebind, the flag clears so a
random keypress later doesn't land as an unintended binding.

**MCM Helper cache staleness (known limitation).** If the player rebinds via the dashboard, then opens MCM's page
in the same session, MCM Helper shows the *pre-rebind* value from its in-session cache. The runtime binding is
still correct; only MCM's display is stale. Rebinding a second time via either surface fixes it. Full live sync
would require poking MCM Helper's cache from Papyrus (via `SetModSettingInt`/`SetModSettingBool` around the
existing `_ne_MCM` script) — a legitimate follow-up but not required for the rebind path to work correctly.

### The tick-enabled bootstrap

`Tick::g_enabled` today defaults to `true` at process
launch and gets set by `Tick::SetEnabled` when the Dispatch
tab's checkbox flips. To make the setting persist across
reboot, `Tick::Start` (or `Plugin.cpp` immediately after
`Settings::Load`) reads `Settings::Get().tickEnabled` and
calls `Tick::SetEnabled(cfg.tickEnabled)` before the tick
thread's first sleep. The default in `Config::tickEnabled`
is `true` — a fresh install ticks; a player who disabled
the tick and rebooted stays disabled.

`Config::tickEnabled` is a new field. It's the only place
where the "runtime killswitch" state has an INI reflection;
`Tick::g_enabled` remains the authoritative runtime store
(atomic, thread-safe reads from every Tick call site), but
its initial value comes from `Config::tickEnabled`.

### Cross-surface sync

Three surfaces can now change settings that share the same
MCM INI:

1. **PrismaUI Settings tab.** Writes via
   `WriteMcmOverride` and updates in-memory Config directly.
   Fires `PushFullState` so the dashboard re-renders with the
   new value.
2. **PrismaUI Dispatch tab (Tick checkbox only).** Same
   handler as before, extended to also write the MCM INI so
   the change persists. The per-beat enable checkboxes on
   this tab keep their existing session-only semantics —
   Phase 08 does not touch them.
3. **PrismaUI Settings tab, hotkey rebind path.** `HotkeySink`'s capture mode writes the four `[Dashboard]` keys
   via `WriteMcmOverride`. Runtime binding updates in-process immediately; MCM Helper reads the same keys on next
   MCM open (subject to the cache-staleness caveat noted above).
4. **MCM page (hotkey only).** Writes via MCM Helper's own path. `_ne_MCM.psc` fires
   `_ne_DashboardHotkeyChanged`, `MCMEventSink` re-runs `Settings::ApplyMcmOverride` so Config reflects the write.
   This path is unchanged from Phase 07.

Two windows can't be open at once — the dashboard's
`ToggleVisibility` calls `Focus(pauseGame=true)` which
prevents MCM from opening, and `HotkeySink` gates the
dashboard open on `!GameIsPaused()` which stops it opening
while MCM is up. So concurrent writers to the MCM INI can't
happen; the read-mutate-save pattern is safe without a
lock.

Live sync between the Dispatch and Settings tabs (both mount
points on the tick toggle) is handled by `PushFullState`
firing at the end of each listener — the state blob carries
the new `tickEnabled` value, both tabs re-render, both
checkboxes agree.

### Dashboard state schema addition

`DirectorState` gains a `settings` object:

```ts
settings: {
    debug_mode: boolean;
    dashboard_hotkey_display: string;              // e.g. "F7" or "Ctrl+F7"
    dashboard_hotkey_capture_active: boolean;      // rebind modal drives off this
    tick_enabled: boolean;                         // duplicates status.tick_enabled
    tick_interval_seconds: number;
    ideal_duration_seconds: {
        exposition: number;
        rising_action: number;
        climax: number;
        falling_action: number;
        resolution: number;
    };
};
```

`status.tick_enabled` is retained (Dispatch tab reads it),
so the two mount points read the same underlying value from
two different shapes on the payload. That's mildly
redundant on the wire (one boolean copied) but keeps the
Dispatch tab code untouched.

`dashboard_hotkey_display` is composed on the C++ side by
`DashboardUIManager::ComposeFullStateJSON` from the current
`dashboardHotkeyDXSC` + `dashboardHotkeyModifiers`. A small
scan-code → friendly-name helper handles the common keys
(letters A-Z, function keys F1-F12, digits 0-9, arrows,
Space, Enter, Escape). Uncommon scan codes fall back to the
literal `"DIK #NN"`. Modifier prefix is `"Ctrl+"`,
`"Shift+"`, `"Alt+"` in that fixed order.

### Slider control shape (in React)

The `int slider` controls use a plain `<input type="range">`
with `min`, `max`, and a numeric label showing the current
value. On `change` (fires on release), the listener callback
fires. `mousemove`-frequency updates are noisy on the JS→C++
bridge and don't help the player, so we deliberately don't
use `oninput`.

Modest debouncing is unnecessary because `change` already
fires only on release, and the write path is one INI
write + one PushFullState. If profiling ever shows a
problem, the writer can be batched behind a 200ms debounce
in JS — not part of Phase 08.

---

## Settings

**New Config fields:**

```cpp
struct Config
{
    // ... existing fields ...

    // [Director]
    bool tickEnabled = true;   // NEW — seeded by ApplyMcmOverride
                               //       and read by Tick::Start.
                               //       The runtime killswitch state
                               //       stored in Tick::g_enabled
                               //       initializes from this.
    // ... other existing [Director] fields unchanged ...
};
```

**Reader coverage (`ReadIniInto` / the new `ApplyMcmOverride`):**

Every field in `Config`. The shared helper's enumeration is the same list `LoadPluginIni` uses today, moved into the
helper and extended with `Config::tickEnabled`. The MCM INI can override any of them; a boot-time cascade of
plugin-INI-first, MCM-INI-second produces the final `Config` singleton.

`ApplyMcmOverride` becomes a thin wrapper: load `kMcmIniPath`, call `ReadIniInto`, log. The four hand-picked
`[Dashboard]` reads it does today are absorbed into the shared enumeration (which also reads them for the plugin-INI
pass; both files can carry the same keys, and MCM's value wins). `MCMEventSink`'s call site is unchanged.

**`WriteMcmOverride` writes:**

Called from the JS→C++ listeners the Settings tab needs (see **File map** below) plus the rebind capture path in
`HotkeySink`. The writer is `void WriteMcmOverride(const McmOverride&)`. `McmOverride` is a struct of optionals
covering the keys the UI can edit: `debugMode`, `tickEnabled`, `tickIntervalSeconds`, five `idealDuration*` fields,
and four hotkey fields (`dashboardHotkeyDXSC`, `hotkeyShift`, `hotkeyCtrl`, `hotkeyAlt`). Each listener/capture-site
writes only its key(s); the rebind path fills all four hotkey optionals in one call so a rebind is atomic on disk.
MCM Helper's own rebind path continues to flow through `_ne_MCM.psc` + `MCMEventSink` and reads the same keys back
on the next `ApplyMcmOverride` — the two write paths converge on the same on-disk shape.

**Plugin INI additions:**

- `[Director] bTickEnabled=1` — new. Author default.
  Plugin-INI seed for `Config::tickEnabled`. Missing key
  falls back to `true`.

**Plugin INI unchanged:**

- Every other key. The plugin INI's role as author-tunable
  ground truth for every setting doesn't change; MCM INI
  just adds an override layer for the user-facing subset.

**MCM INI seed file (`statics/MCM/Config/NarrativeEngine/settings.ini`):**

Extended with the eight new UI-facing keys so MCM Helper's first-run seed matches the Config defaults. Not strictly
required (missing keys fall through to plugin INI, then Config defaults, via `ReadIniInto`), but makes the seed file
self-documenting for the UI's initial state. The seed file does **not** enumerate every Config field — only the ones
the UI can edit. A modder who wants to override a non-UI key adds it to the deployed MCM INI at runtime; they don't
need it seeded.

---

## Persistence

Nothing new in the SKSE cosave. All new persistence lives
in `Data/MCM/Settings/NarrativeEngine.ini`. That file's
lifetime is:

- Created by MCM Helper (Phase 07 seed).
- Extended by the Settings tab (Phase 08 writes).
- Read by C++ at `kDataLoaded` (Phase 07 reader, widened
  by this phase).
- Re-read by `MCMEventSink` on the
  `_ne_DashboardHotkeyChanged` event (Phase 07, unchanged).
- Never deleted by the plugin.

Boot ordering — a wrinkle worth calling out. `Tick::Start`
today is called from `kPostLoadGame` / `kNewGame`, both of
which fire after `kDataLoaded` (where `Settings::Load`
runs). So `Tick::Start` sees a `Config::tickEnabled`
that's already been reconciled with the MCM INI override.
No pre-boot race.

---

## File map

New:

- Nothing in `include/` — every new function
  (`Settings::WriteMcmOverride`, extended
  `Settings::ApplyMcmOverride`) lives in existing headers.
- `dashboard/src/components/tabs/SettingsTab.tsx` — the new
  tab. Renders three sections (General, Narrative Director,
  Phase Durations) with controls per **Scope → In scope**.

Reshaped:

- `include/Settings.h` — add `tickEnabled` to `Config`; add `McmOverride` struct; declare `WriteMcmOverride`; update
  file header to describe the universal-override cascade and the fall-through defaulting convention.
- `src/Settings.cpp` — extract the current per-key `Get*Value` calls in `LoadPluginIni` into a shared file-local
  `ReadIniInto(CSimpleIniA&, Config&)` helper; add the `Config::tickEnabled` key to that helper (both the plugin-INI
  read path and the MCM-INI read path pick it up automatically). Rewrite `Load` to call `ReadIniInto` twice — once
  against the plugin INI, once against the MCM INI. Rewrite `ApplyMcmOverride` as a thin wrapper that reopens
  `kMcmIniPath` and re-invokes `ReadIniInto` on `g_config`. Implement `WriteMcmOverride` (narrow write surface — only
  the UI-editable keys).
- `include/Tick.h` / `src/Tick.cpp` — `Tick::Start` reads
  `Settings::Get().tickEnabled` before the first sleep.
- `src/DashboardUIManager.cpp` —
  - Register five new JS listeners: `ne_setDebugMode`, `ne_setTickInterval`, `ne_setPhaseIdealDuration`,
    `ne_beginHotkeyRebind`, `ne_cancelHotkeyRebind`.
  - Extend `OnSetTickEnabled` to also `Settings::WriteMcmOverride` — the toggle now persists from both mount
    points (Dispatch and Settings tabs).
  - Add file-local `std::atomic<bool> g_hotkeyCaptureMode` and extend `HotkeySink::ProcessEvent` with a
    capture-mode branch that writes the four hotkey MCM-INI keys via `WriteMcmOverride`.
  - Auto-cancel the capture in `ToggleVisibility` on the hide edge.
  - `ComposeFullStateJSON` emits the new `settings` object including the friendly-name hotkey display and the
    `dashboard_hotkey_capture_active` flag.
- `dashboard/src/types.ts` — add `settings` to
  `DirectorState`.
- `dashboard/src/App.tsx` — mount the SettingsTab under a
  new `'settings'` tab id.
- `dashboard/src/components/TabBar.tsx` — add `'settings'`
  to `TabId` and the `TABS` array.
- `statics/SKSE/Plugins/NarrativeEngine.ini` — add
  `bTickEnabled=1` under `[Director]`; expand the file
  header comment to describe the MCM override cascade in
  its Phase 08 shape.
- `statics/MCM/Config/NarrativeEngine/settings.ini` — seed
  the eight new keys with defaults matching the Config
  struct.
- `statics/MCM/Config/NarrativeEngine/config.json` — no
  change. New settings are not surfaced in MCM; the MCM
  menu keeps its hotkey-only footprint.

Preserved unchanged:

- `esp/Source/Scripts/_ne_MCM.psc` — the MCM script's job
  is unchanged (fire ModEvent on setting change).
- `include/MCMEventSink.h` / `src/MCMEventSink.cpp` — the
  sink only cares about `_ne_DashboardHotkeyChanged`; new
  settings are written from C++ and don't need the
  Papyrus→C++ round trip.

---

## Implementation plan

Sequential. Step 1 is C++ plumbing (Settings widening +
Tick bootstrap). Step 2 is the JS→C++ listeners and
`PushFullState` schema change. Step 3 is the React tab.
Step 4 is verification.

Every step is Claude's work except Step 4, which is
joint — the on-disk INI checks and cross-reboot behavior
need the user's live game.

Step order:

1. **C++ Settings widening + Tick bootstrap.** Widen
   `ApplyMcmOverride`, implement `WriteMcmOverride`, add
   `tickEnabled` to Config, wire `Tick::Start` to read it.
   Verifiable at the build level.
2. **C++ dashboard wiring — listeners + schema.** Register
   three new JS listeners; extend the tick listener to
   also persist; emit the `settings` sub-object from
   `ComposeFullStateJSON`. Verifiable at the build level
   plus a log-line check.
3. **React Settings tab.** New tab component, TabBar
   registration, App.tsx mount, types.ts extension. Rebuild
   the dashboard bundle. Verifiable in-game — page loads,
   controls render.
4. **End-to-end verification.** Change each control from
   both surfaces (where applicable); verify INI file,
   verify Settings singleton, verify reboot survival.

---

### Step 1 — C++ Settings widening + Tick bootstrap

- [x] Complete

**[CLAUDE]**

**Goal:** Restructure `Settings` so both INIs flow through one shared per-key enumeration; give the MCM INI
override coverage for every Config field; add the `Config::tickEnabled` bootstrap so a persisted "off" survives
reboot. After this step, the plumbing is in place but no UI writes to it yet.

**Files:**

- `include/Settings.h` — add `Config::tickEnabled = true`; add `McmOverride` struct; declare
  `void WriteMcmOverride(const McmOverride&)`; refresh the file header comment to describe the universal-override
  cascade (plugin INI supplies defaults, MCM INI overrides any named key, both files read via the same enumeration).
- `src/Settings.cpp` — extract the current `LoadPluginIni` per-key reads into `ReadIniInto(CSimpleIniA&, Config&)`;
  add `tickEnabled` to that enumeration; rewrite `Load` around two `ReadIniInto` calls; rewrite `ApplyMcmOverride`
  as a thin wrapper; implement `WriteMcmOverride`.
- `src/Tick.cpp` — in `Tick::Start`, before spawning the worker thread, call
  `g_enabled.store(Settings::Get().tickEnabled, memory_order_release)`.
- `statics/SKSE/Plugins/NarrativeEngine.ini` — add `[Director] bTickEnabled=1` with a comment describing the MCM
  override cascade.
- `statics/MCM/Config/NarrativeEngine/settings.ini` — seed the eight new UI-facing keys under their sections.

**Sub-tasks:**

1. **`Config::tickEnabled`.** Add the field with a comment noting its role: "runtime killswitch initial state;
   seeds `Tick::g_enabled` at `Tick::Start`. Runtime changes via the dashboard update this field in place *and*
   write to the MCM INI (so a subsequent boot's `ReadIniInto` re-seeds correctly)."
2. **`McmOverride` struct.** Field per writable key, each an `std::optional`. Order matches Config's field order
   for readability. Add a small doc comment noting the narrow write surface vs. the universal read coverage.
3. **Extract `ReadIniInto(CSimpleIniA& ini, Config& dst)`.** Move every `ini.Get*Value(...)` call currently in
   `LoadPluginIni` into this file-local helper. Each call passes the corresponding `dst.<field>` as its default —
   preserving the fall-through semantics regardless of which INI is being read. Add the new `tickEnabled` read to
   this helper (`bTickEnabled` under `[Director]`); do not add anything anywhere else. Keep section names identical
   to today's — the plugin INI and MCM INI use the same schema.
4. **Rewrite `Load`.** Reset `g_config = Config{}`; load the plugin INI into a `CSimpleIniA`; if the load succeeded,
   call `ReadIniInto(pluginIni, g_config)` and log `Settings: loaded from ...`; else log the "no plugin INI" branch.
   Then load the MCM INI; if it succeeded, call `ReadIniInto(mcmIni, g_config)` and log
   `Settings: MCM overrides applied from ...`. Keep the existing `Settings: dashboard hotkey ...` log at the end.
5. **Rewrite `ApplyMcmOverride`.** New body: load `kMcmIniPath` into a fresh `CSimpleIniA`; if that fails, return
   quietly (fresh install / user deleted the file); else call `ReadIniInto(mcmIni, g_config)` and log the mutation.
   `MCMEventSink`'s call site is unchanged. The four hand-picked `[Dashboard]` reads it did before are absorbed by
   the shared enumeration.
6. **Implement `WriteMcmOverride`.** Load the MCM INI (or start empty on file-not-found); for each engaged optional
   in `McmOverride`, call the appropriate `Set*Value`; save. Then apply the same mutations to `g_config` in memory
   so `Settings::Get()` reflects the write immediately. Log the changed keys as
   `logger::info("Settings: MCM override write: <key>=<value>")` — one line per key so the log reads as an audit
   trail.
7. **`Tick::Start`.** Before the worker thread starts, read `Settings::Get().tickEnabled` and call the atomic
   `store` on `g_enabled`. Existing `SetEnabled` / `IsEnabled` remain the runtime API — no change.
8. **Plugin INI update.** Add the `bTickEnabled=1` line under `[Director]` with an in-file comment explaining the
   MCM override. Also expand the file-header comment to describe the new universal cascade — any key in this file
   can be overridden by naming the same key in the MCM INI, whether or not that key has a UI editor.
9. **MCM seed file update.** Add the eight new UI-facing keys under `[General]` / `[Director]` sections with
   values matching the Config defaults. Do not enumerate every plugin-INI key; the seed file's purpose is to give
   MCM Helper's page and the Settings tab a coherent first-open state, not to mirror the plugin INI.
10. `pwsh -File format.ps1` — resolve every finding.
11. `pwsh -File build.ps1 build` — verify C++ compiles and Papyrus is unchanged.

**Specifics:**

- The plugin INI's `iTickIntervalSeconds=30` dev override
  is retained. Players who install the mod see 30 until
  they change it in the Settings tab (or delete the plugin
  INI). If we ship, the plugin INI's line changes to `90`;
  either way, the Settings tab's slider default is the
  Config-baked ship default (90), not the current plugin-
  INI value — reset behavior needs to be predictable.
- `Config::tickEnabled` is the initial state, not the runtime state. Do not update `Config::tickEnabled` in
  `Tick::SetEnabled` (that would create a cyclic dependency); update it only in `ReadIniInto` (via the two `Load`
  calls) and `WriteMcmOverride`. Runtime reads use `Tick::IsEnabled`.
- The "each `Get*Value` passes `dst.<field>` as its default" convention is load-bearing. On the plugin-INI pass,
  `dst` is fresh-defaulted, so missing keys land on the Config baked-in default. On the MCM-INI pass, `dst` already
  carries the plugin-INI values, so missing keys land on those. If a future refactor drops the convention (e.g.
  passing `0` as the default for an int read), the cascade silently breaks for that key — the reviewer should watch
  for that.

**Verify:**

- `pwsh -File build.ps1 build` succeeds. C++ and Papyrus
  compile cleanly.
- `pwsh -File format.ps1` succeeds clean.
- Boot Skyrim (no in-game verification yet — no dashboard
  UI changes). Log shows the existing
  `Settings: dashboard hotkey DXSC=... mods=...` line and,
  if `bDebugMode` is set, `Settings: debug mode ON`. No
  crash on load. No new visible behavior — the extra
  fields are read into Config but nothing reads them yet.

---

### Step 2 — C++ dashboard wiring: listeners + schema

- [x] Complete

**[CLAUDE]**

**Goal:** Register the JS→C++ listeners the Settings tab
will call and extend `ComposeFullStateJSON` to emit the
`settings` sub-object. After this step, C++ is fully
listening; the browser side just isn't calling yet.

**Files:**

- `src/DashboardUIManager.cpp` — register three new
  listeners (`ne_setDebugMode`, `ne_setTickInterval`,
  `ne_setPhaseIdealDuration`), extend `OnSetTickEnabled`
  to persist via `WriteMcmOverride`, extend
  `ComposeFullStateJSON` to emit `settings` with the
  friendly hotkey display string.
- Nothing else — Step 1 already added the Settings
  plumbing this step depends on.

**Sub-tasks:**

1. **Friendly hotkey display.** Add a small file-local helper `std::string FormatHotkeyBinding(int dxsc,
   uint8_t mods)` returning a string like `"F7"`, `"Ctrl+F7"`, `"Ctrl+Shift+Alt+G"`. Modifiers in fixed order
   (Ctrl/Shift/Alt). Body is a switch on DIK values for the common keys (function keys, letters, digits, arrows,
   Space/Enter/Escape/Tab); fallback is `"DIK #<N>"`.
2. **`ne_setDebugMode` listener.** Signature matches other bool listeners. Parse via `ParseBoolArg`. Marshal to
   main thread. Call `Settings::WriteMcmOverride({.debugMode = enabled})`. Log the change. Call `PushFullState`.
3. **`ne_setTickInterval` listener.** Payload is a bare integer string. Parse via `std::from_chars`. Clamp to
   `[10, 600]`. Call `Settings::WriteMcmOverride({.tickIntervalSeconds = v})`. Log. Push.
4. **`ne_setPhaseIdealDuration` listener.** Payload is a JSON object `{"phase":"exposition","seconds":420}`.
   Parse; validate phase name against the five known strings; clamp to the slider's range (`60..1200` for four
   phases, `30..600` for climax). Fill the appropriate optional and write. Log. Push.
5. **Extend `OnSetTickEnabled`.** After the existing `Tick::SetEnabled(enabled)` call, also
   `Settings::WriteMcmOverride({.tickEnabled = enabled})`. This makes the Dispatch tab's checkbox persist too —
   a free upgrade to what was previously session-only.
6. **`ne_beginHotkeyRebind` listener.** No argument. Marshal to main thread. Flip
   `g_hotkeyCaptureMode.store(true, memory_order_release)` (new atomic file-local in `DashboardUIManager.cpp`).
   Log `DashboardUIManager: hotkey rebind capture armed`. Call `PushFullState` so
   `settings.dashboard_hotkey_capture_active` flips on for the React modal.
7. **`ne_cancelHotkeyRebind` listener.** No argument. Marshal to main thread. Flip `g_hotkeyCaptureMode` back to
   false (idempotent — if already false, log at debug and no-op). Call `PushFullState`.
8. **Extend `HotkeySink::ProcessEvent` — capture mode branch.** At the top of the per-button-down loop, check
   `g_hotkeyCaptureMode.load(memory_order_acquire)`. When true:
   - If the DIK is one of the six modifier codes (42/54/29/157/56/184), `continue` — don't consume.
   - If the DIK is `kDIK_ESCAPE`, flip the flag off and marshal a `PushFullState`; treat as cancel; return.
   - Otherwise: probe `GetAsyncKeyState` for the three modifier flags, build the `McmOverride` with all four
     hotkey optionals set, marshal a main-thread call to `Settings::WriteMcmOverride(...)` followed by
     `PushFullState`, and flip the flag off. Log
     `DashboardUIManager: hotkey rebound DXSC={} shift={} ctrl={} alt={}`. Return.
   The existing binding-match branch below is unreachable while the flag is set, but leave it in place for the
   normal case.
9. **Auto-cancel on hide.** In `ToggleVisibility`, before/after the `Hide` call, unconditionally
   `g_hotkeyCaptureMode.store(false, memory_order_release)`. Ensures a mid-rebind hide doesn't leave a latent
   capture that snags a random keypress later.
10. **`ComposeFullStateJSON` — `settings` sub-object.** Populate:

    ```json
    {
      "settings": {
        "debug_mode": ...,
        "dashboard_hotkey_display": "Ctrl+F7",
        "dashboard_hotkey_capture_active": <g_hotkeyCaptureMode.load()>,
        "tick_enabled": ...,
        "tick_interval_seconds": ...,
        "ideal_duration_seconds": {
          "exposition": ..., "rising_action": ...,
          "climax": ..., "falling_action": ...,
          "resolution": ...
        }
      }
    }
    ```

11. Register the five new listeners in `Initialize` next to the existing four.
12. `pwsh -File format.ps1` — resolve findings.
13. `pwsh -File build.ps1 build` — verify build.

**Specifics:**

- Every listener's PushFullState is inside the marshaled
  main-thread lambda so the pushed state reflects the
  mutation the listener just applied — same discipline as
  the existing four listeners.
- `WriteMcmOverride` runs on the main thread inside these
  lambdas (safe — SimpleIni is single-threaded and no other
  thread writes the INI). Do not call it from the
  PrismaUI worker thread directly.
- The clamp in each listener is defensive; the slider's
  min/max in HTML already constrains the payload, but a
  browser-side glitch or malicious call from another mod's
  JS shouldn't be able to write a wild value.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- `pwsh -File format.ps1` clean.
- Boot Skyrim. Open the dashboard. In the browser dev tools
  console (if PrismaUI exposes one; else via the log after a
  manual test in Step 4), invoke
  `window.ne_setDebugMode('true')`. Log shows
  `DashboardUIManager: ne_setDebugMode(true) received` and
  `Settings: MCM override write: bDebugMode=1`. Verify
  `Data/MCM/Settings/NarrativeEngine.ini` on disk has
  `bDebugMode=1` under `[General]`. Reload the game;
  Config's `debugMode` reads `true` from the MCM INI.

---

### Step 3 — React Settings tab

- [x] Complete

**[CLAUDE]**

**Goal:** Add the Settings tab component, register it in
the tab bar, and mount it in App.tsx. Rebuild the dashboard
bundle. After this step, the page is visible in-game and
every control routes to a live C++ listener.

**Files:**

- `dashboard/src/components/tabs/SettingsTab.tsx` — new.
  Three `<section className="panel">` blocks with the
  controls per **Scope**. Uses the same panel/heading
  conventions the other tabs use.
- `dashboard/src/types.ts` — add `settings` sub-object to
  `DirectorState`.
- `dashboard/src/App.tsx` — mount SettingsTab under the
  `'settings'` tab id.
- `dashboard/src/components/TabBar.tsx` — add
  `{ id: 'settings', label: 'Settings' }` to `TABS`.
- `dashboard/src/index.css` (or the equivalent stylesheet) —
  add any new classes the Settings tab needs. Keep tight;
  reuse `.panel`, `.tab-content`, `.tick-toggle` where
  possible.

**Sub-tasks:**

1. **`SettingsTab.tsx`.** Declarations:

   ```ts
   declare global {
     interface Window {
       ne_setDebugMode?:            (arg: string) => void;
       ne_setTickEnabled?:          (arg: string) => void; // existing
       ne_setTickInterval?:         (arg: string) => void;
       ne_setPhaseIdealDuration?:   (arg: string) => void;
       ne_beginHotkeyRebind?:       (arg: string) => void; // arg unused
       ne_cancelHotkeyRebind?:      (arg: string) => void; // arg unused
     }
   }
   ```

2. **General section.** Debug Mode checkbox + Dashboard Hotkey row. The hotkey row renders
   `state.settings.dashboard_hotkey_display` with a **Rebind** button to its right. Clicking Rebind invokes
   `window.ne_beginHotkeyRebind?.('')`. No local state.
3. **Rebind modal (`HotkeyRebindModal`).** Renders iff `state.settings.dashboard_hotkey_capture_active === true`.
   Fixed-position overlay with a centered panel: "Press any key to bind." on the primary line, "Hold modifiers
   while pressing to include them. Esc to cancel." as secondary text, and a `Cancel` button that invokes
   `window.ne_cancelHotkeyRebind?.('')`. Because the modal is a pure function of server state, it disappears the
   moment C++ flips the flag off (whether from a captured keypress, an ESC-cancel from the input sink, or the
   Cancel button); no `useEffect`-based close logic is needed. Include a top-level `role="dialog"` and
   `aria-modal="true"` for basic accessibility hygiene.
4. **Narrative Director section.** Enable Narrative Tick checkbox (`onChange` calls `ne_setTickEnabled`), Tick
   Interval slider (10..600, step 5) with a numeric label showing the current value. Slider `onChange` (release-
   time) calls `ne_setTickInterval(String(newValue))`.
5. **Phase Durations section.** Five sliders, one per phase. Each uses a range-input in a
   `<label>{name}<input/><span>{value}s</span></label>` wrapper. `onChange` calls
   `ne_setPhaseIdealDuration(JSON.stringify({phase, seconds:v}))` where `phase` is the lowercase-with-underscores
   name matching the C++ side's expected values.
6. **TabBar update.** Append `'settings'` to `TabId` and to `TABS`. Order:
   `director, letters, visit, dispatch, settings`.
7. **App.tsx update.** Conditional render on `activeTab === 'settings'`.
8. **types.ts update.** Add the `settings` field per **Core concepts → Dashboard state schema addition**.
9. **Rebuild.** `pwsh -File build.ps1 build` runs the dashboard bundle build as part of the normal build pipeline;
   the deployed mod folder picks up the new bundle.
10. `pwsh -File format.ps1` — resolve findings (Prettier handles the TSX).

**Specifics:**

- Slider values render "live" as the player drags via
  `oninput`-based label update, but the C++ listener only
  fires on `onchange` (release). No debounce logic needed;
  we get one write per drag-release, which is what we want.
- The Debug Mode checkbox is intentionally simple. Toggling
  debug mode from the dashboard reflects immediately in
  Config; subsystems that log with
  `if (Settings::Get().debugMode)` gates start / stop
  emitting immediately.
- The tick-enabled control shares no React state with the
  Dispatch tab's version; both read from `state`, so a
  change from either surface fires PushFullState and both
  update on the same frame.
- Do not import `stateStore` in SettingsTab; take the
  state via props like the other tabs do. Keeps the
  component testable and matches the codebase's tab
  conventions.

**Verify:**

- `pwsh -File build.ps1 build` succeeds; the deployed
  `dashboard/index.html` bundle is refreshed under
  `Data/PrismaUI/views/NarrativeEngine/dashboard/`.
- Boot Skyrim; open the dashboard; the tab bar shows the fifth tab labeled "Settings". Click it. Three sections
  render, all controls populated from live values (Debug Mode reflects the current INI; Tick Interval slider
  starts at 30 given the current dev override; five phase sliders start at 330/225/90/225/330). Dashboard Hotkey
  row reads "F7" (or whatever the current binding is) with a **Rebind** button to its right. Clicking Rebind
  opens the modal; pressing Cancel closes it without changing the binding. (Full rebind survival is verified in
  Step 4.)

---

### Step 4 — End-to-end verification

- [x] Complete

**[CLAUDE + USER]**

**Goal:** Walk a representative minimum of scenarios that together prove persistence, cross-surface sync, and
fallback behavior. The five Config-field types the Settings tab exercises (bool, bare-int slider, JSON-shaped
phase-slider payload, hotkey capture, dispatch-tab mirror) collapse to two representative controls and three
architectural probes — testing each of the five phase sliders individually would only re-run the same JSON code
path with different keys.

**Sub-tasks:**

1. **Representative writes + reboot survival.** Change **Debug Mode** (represents bool writes) and
   **Exposition ideal duration** (represents the JSON-payload slider path — one phase slider exercises the same
   code path all five use). For each: log shows the `Settings: MCM override write: <key>=<value>` line;
   `Data/MCM/Settings/NarrativeEngine.ini` on disk shows the section+key+value; the Settings tab reflects the new
   value. Then quit → relaunch → load a save; both controls come back with the values you set, not the defaults.
2. **Plugin INI unchanged.** `git diff statics/SKSE/Plugins/NarrativeEngine.ini` shows only the `bTickEnabled=1`
   addition from Step 1 and the header-comment expansion — no other keys changed. The plugin INI is never
   authored by Settings-tab writes.
3. **Fresh-install fallback.** Delete `Data/MCM/Settings/NarrativeEngine.ini`. Boot Skyrim, load a save. Log shows
   `Settings: loaded from ...` but no `Settings: MCM overrides applied ...` line (the file's absent). Every
   setting reads its plugin-INI value (Tick Interval: 30, Debug Mode: 1, phase durations at plugin-INI defaults).
4. **Cross-surface tick sync + persistence.** On the Dispatch tab, toggle Tick Enabled off. Switch to the
   Settings tab; the Tick Enabled checkbox is unchecked (server state pushed via `PushFullState` drives both
   mount points). Toggle it on from the Settings tab; switch back to Dispatch; the checkbox is checked. Reboot;
   `Tick: driver thread started (... enabled=true)` matches the last-set value. This scenario is the sole test
   of two things: (a) the two-mount-point live sync unique to the tick toggle, and (b) the Phase 08 upgrade that
   made tick-enabled persist at all (pre-Phase 08 it was session-only).
5. **In-dashboard hotkey rebind + one cancel path.** Click **Rebind**; the modal appears (log:
   `DashboardUIManager: hotkey rebind capture armed`). Hold Ctrl+Shift and press `G`. The modal closes; MCM INI
   on disk shows `iHotkeyDXSC=34`, `bHotkeyShift=1`, `bHotkeyCtrl=1`, `bHotkeyAlt=0` under `[Dashboard]`; log
   shows the `hotkey rebound DXSC=34 shift=1 ctrl=1 alt=0` line. Display reads `Ctrl+Shift+G`. Close the
   dashboard; press Ctrl+Shift+G — dashboard opens; press old F7 — nothing. Reboot; binding survives. Then test
   **one** cancel path: click Rebind again, press ESC — modal closes, binding unchanged, log shows the ESC
   cancel line. (The Cancel-button path and dashboard-hide auto-cancel path share the same
   `g_hotkeyCaptureMode = false` code, so ESC's success covers them.) Finally rebind back to F7 no modifiers to
   leave the test env clean.
6. **Cross-surface hotkey persistence + universal override.** Two probes in one boot:
   - Rebind via MCM to Ctrl+`8`; verify the MCM ini keys change. Open the dashboard; the Settings tab's Dashboard
     Hotkey display shows `Ctrl+8` (proves `_ne_DashboardHotkeyChanged` → `ApplyMcmOverride` → `PushFullState`
     round trip). MCM Helper's own control may still show the old value from its in-session cache (documented
     limitation, not a bug).
   - Quit Skyrim; hand-edit `Data/MCM/Settings/NarrativeEngine.ini` to add `iBeatCooldownSeconds=45` under
     `[BeatSystem]` (plugin-INI default: 120; no UI edits this). Boot; log shows both `Settings: loaded ...` and
     `Settings: MCM overrides applied ...`; the beat-system's next dispatch honors the 45s cooldown. Remove the
     line; reboot; the value falls back to 120. This is the only test of the universal-override contract — any
     Config field, not just the twelve the UI writes, is honored when named in the MCM INI.

**Verify:** all six sub-tasks pass; the Step 1, 2, and 3 checkboxes flip to complete alongside this one.

---

## Done condition

- All four steps above have their checkboxes marked
  complete.
- The dashboard's tab bar contains a fifth tab labeled
  "Settings" that renders three sections (General,
  Narrative Director, Phase Durations) with the controls
  specified in **Scope → In scope**.
- Every control change routes through `WriteMcmOverride`
  and lands in `Data/MCM/Settings/NarrativeEngine.ini` on
  disk under the section+key mapping described in **Core
  concepts → Read cascade**.
- Changes survive quit → relaunch → load save.
- The Dispatch tab's Tick checkbox and the Settings tab's Tick checkbox stay in sync live and after reboot.
- The Settings tab's Rebind button captures the next non-modifier keypress in native DIK space via
  `HotkeySink`'s temporary capture mode, writes the four `[Dashboard]` MCM-INI keys via `WriteMcmOverride`, and
  the resulting binding is honored by the runtime hotkey path immediately and by MCM Helper on next MCM open
  (subject to the documented in-session cache-staleness caveat).
- Deleting the MCM INI restores plugin-INI behavior for every setting.
- The plugin INI has one new line (`bTickEnabled=1` under
  `[Director]`) and no other changes.
- The MCM Helper `config.json` is unchanged. The MCM
  page's contents and layout are unchanged.
- `pwsh -File build.ps1 build` succeeds cleanly.
- `pwsh -File format.ps1` succeeds cleanly.
- The C++ reader is restructured around a single shared `ReadIniInto` enumeration that populates every Config
  field. `Settings::Load` invokes it twice (plugin INI, then MCM INI); missing keys in either file fall through
  correctly. `ApplyMcmOverride` is a thin wrapper over the same helper.
- Every plugin-INI key can be overridden by naming the same key in the MCM INI — including the fifty-plus
  author-only knobs no UI exposes. Verified by scenario 7 in Step 4.
- The `WriteMcmOverride` write surface stays narrow — only the eight UI-editable keys. Hand-authored MCM-INI entries
  for non-UI keys are honored on read but never touched by the writer.
