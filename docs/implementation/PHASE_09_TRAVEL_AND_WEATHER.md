# Phase 09 — Travel and Weather Events

Two new internally-tracked event sources modelled after `CombatEventLog`. Both fill gaps SkyrimNet's event stream
leaves around ambient world state — the player moving between locations, holds, and biomes; and the weather
shifting overhead — and merge into the same `recent_events` array the Director's prompt and the dashboard already
consume.

No gameplay features. No LLM calls. No new beats. This phase is scoped to observation and event surfacing only.

---

## Why this phase exists

`CombatEventLog` established that a NarrativeEngine-owned event stream — hook the engine, keep a small in-process
log, merge into the tail SkyrimNet already renders — closes gaps the LLM would otherwise be blind to. Two other
gaps are similarly high-signal but currently unobserved:

- **Travel.** SkyrimNet has no signal for "the player entered Riverwood" / "the player crossed from Whiterun Hold
  into Falkreath Hold" / "the player fast-travelled from Whiterun to Solitude." The LLM sees dialogue and combat
  but has no sense of the player's geographic trajectory unless an NPC volunteers a location line in dialogue.
- **Weather.** The sky changing overhead — sun breaking through after a storm, snow starting to fall, a
  thunderstorm rolling in — is dramatically load-bearing and free ambient signal, but presently invisible to the
  Director. A future portent beat will *cause* weather changes; this phase makes those changes observable
  regardless of source.

Both are prerequisites for a robust future portent beat (Phase 10+): the beat authors the weather change, and the
same observation infrastructure this phase builds records that change into the tail. No special-case pathway
required.

A supporting piece falls out of the travel design: reliable **region detection**. The `LocSet*` keyword surface on
`BGSLocation` is inconsistently applied on vanilla locations and unusable as a hold/biome signal. Vanilla
`TESRegion` records, exposed via `RE::TESObjectCELL::GetRegionList`, are authored on cells and are what the
vanilla weather system itself consults. This phase introduces a small `Region` module that resolves the player's
current hold and climate from that surface; travel consumes it directly, and future biome-gated beats can consume
it too.

---

## Scope

### In scope

- A new **`Region`** module resolving the player's current hold and climate from `TESRegion` records.
- A new **`WeatherEventLog`** module observing `RE::Sky` state and emitting events on weather-category shifts.
- A new **`TravelEventLog`** module observing player location/region/parent-chain transitions and emitting
  travel events. Includes follower rendering, fast-travel specialization, and a run-collapsing condensation
  pass in `GetRenderedTail` modelled after combat's `hit`-condensation.
- A small shared-utility extraction (`EventLogUtil.h/.cpp`) so the three event-log modules
  (`CombatEventLog` + the two new ones) don't triplicate identical timestamp / string-serialization helpers.
- Wire-in of both new tails into `EvaluationPipeline::BuildPromptContext` and
  `DashboardUIManager::ComposeFullStateJSON` via the existing `BuildMergedTimeline` merger.
- New cosave records (`'NEWE'`, `'NETR'`) with the same on-save-prune-then-write discipline `CombatEventLog`
  uses.
- New INI section `[TravelEvents]` and `[WeatherEvents]` with a small handful of tunables (ring-buffer caps,
  condensation window, follower radius, debounce interval).

### Deferred (explicitly out)

- **The portent beat itself.** This phase makes weather changes *observable*; it does not make them
  *directable*. The `weather_portent` beat lands in a later phase.
- **Any new SkyrimNet prompt authoring** that consumes the new events. The events land in `recent_events`; the
  existing Director prompt reads that array unchanged.
- **Interior-to-interior travel events.** Transitions between two interior cells (walking through a loading
  door from one building to another, moving between dungeon zones) are suppressed for Phase 9. Interior↔exterior
  transitions (entering / leaving a building) still fire. Revisit if the LLM asks for more granularity.
- **Follower detection refinements** beyond `IsPlayerTeammate` + high-process + radius. Summon-based
  companions and mount-dismissed followers may not be captured on every transition; acceptable at this scope.

---

## Design overview

### Shared shape

Every event-log module in the codebase (`CombatEventLog` today; `WeatherEventLog` and `TravelEventLog` after this
phase) follows the same pattern:

- Registered at `SKSE::MessagingInterface::kDataLoaded` from `Plugin.cpp`.
- Poll driven by `Tick::Poll` on the main thread at ~500 ms cadence, plus optionally SKSE event sinks for
  discrete engine notifications.
- Ring buffer of internal events under a per-module `std::mutex`, capped at a settings-tunable size.
- Cosave record (`OnSave` / `OnLoad` / `OnRevert`) that prunes before writing so on-disk state matches the
  in-memory pruning rules.
- `OnPostLoadGame` hook to seed baseline state from the freshly-loaded world.
- `OnPhaseAdvanced` hook to drop events older than the current phase entry.
- `GetRenderedTail(currentGameTimeSeconds)` returning a JSON array shaped identically to SkyrimNet events, with
  `text` pre-rendered including the `[N ago]` prefix produced by `SkyrimNetEvents::FormatRelativeGameTime`.

The merge into the Director's `recent_events` and the dashboard state happens once, in the existing
`BuildMergedTimeline` helper — it accepts an additional array argument per new source rather than being called
multiple times.

### `Region` — hold and biome resolution

Small module with two entry points:

```cpp
namespace NarrativeEngine::Region {
    enum class Climate : std::uint8_t {
        Unknown,     // interior, unrecognised region, or no region — callers should treat as "no signal"
        Tundra,      // Whiterun Hold plains
        Pine,        // Falkreath Hold forest
        Marsh,       // Hjaalmarch
        Snow,        // Winterhold, The Pale — permanent snow
        Reach,       // The Reach — mountainous / rocky
        Rift,        // The Rift — autumnal forest
        Coast,       // Haafingar coast
        Volcanic,    // Eastmarch geothermal
        Solstheim,   // DLC02 ashland
    };

    struct Resolution {
        Climate climate = Climate::Unknown;
        RE::FormID holdRegionFormID = 0;   // the TESRegion FormID that resolved, if any — 0 when Unknown
        std::string holdDisplayName;       // "Whiterun Hold", "Falkreath Hold", ... derived from the region
                                           // EditorID via a small lookup; empty when Unknown
    };

    Resolution ForPlayer();
    Resolution ForCell(RE::TESObjectCELL* cell);
}
```

Internally: walks `cell->GetRegionList(false)`, matches each `TESRegion` by EditorID against a curated table,
returns the most specific hit (per-hold beats the Tamriel root region). EditorIDs are resolved lazily via
`RE::TESForm::LookupByEditorID` on first call (requires powerofthree's Tweaks, same as `LocationKeywords`) and
cached. Source-of-truth CSV lives at `docs/vanilla/regions/holds.csv` so a maintainer can audit/extend without
touching code.

`Sky::region` is available as an optimisation path (the engine has already picked a region for weather purposes)
but the cell-walk fallback is authoritative — during fast travel and cell-load transitions `Sky::region` briefly
lags. Start with the cell walk; add the `Sky` shortcut only if profiling shows the walk matters, which it won't.

### `WeatherEventLog` — observation-only

Poll-based, no sinks. `Poll()` is called from `Tick::Poll` on the 500 ms cadence but internally throttles —
weather transitions happen on the order of tens of seconds and don't warrant frequent re-sampling. An
accumulator checks `NowUnixSeconds() - g_lastPolledAt` against `iWeatherEventPollIntervalSeconds` (default 30)
and early-returns cheaply on the poll cycles in between. When the interval elapses, it reads
`RE::Sky::GetSingleton()`, derives a small `WeatherCategory { primary, stormy }` tuple from
`currentWeather->data.flags` plus lightning frequency, and diffs against the last-observed tuple. On mismatch,
emits a `weather_event` with `ne_kind` drawn from a small hardcoded transition vocabulary.

`primary` is a priority-ordered enum: `Snowy` > `Rainy` > `Pleasant` > `Cloudy` > `Other`. `stormy` is a
derived bool set when a rainy/snowy weather has non-zero `Data::thunderLightningFrequency` OR high wind speed.
Rendering vocabulary lives entirely in `WeatherEventLog.cpp` as a static transition table (from-primary,
to-primary, from-stormy, to-stormy) → sentence. Transitions with no table entry render as a generic
"The weather has changed."

Explicitly *not* suppressed:

- Weather overrides from other mods, from console `sw`/`fw`, or from a future portent beat. When the portent
  beat ships, it authors a weather change and the poll observes it through the exact same mechanism — the
  narrative fact of the sky darkening lands in the tail without special-case plumbing.
- Scripted weathers (HelgenAttackWeather, MQ206Weather, DA02Weather). These *are* narratively significant
  moments and belong in the tail; we don't second-guess the game's story cues.

Suppressed:

- `Sky::mode != kFull` (interior). No weather narrative when the player can't see the sky.
- The first `Poll()` after `OnPostLoadGame` — used to seed baseline, no event emission.
- A short inter-event debounce (`iWeatherEventDebounceSeconds`, default 20 real seconds) applied on top of the
  poll interval to prevent a pathological rapid-flip inside a single sampling window from spamming.

### `TravelEventLog` — observation with condensation

Poll-based comparison of a per-tick `TravelSnapshot`; a single `TESFastTravelEndEvent` sink flags the next
observed transition as a fast-travel arrival.

**Snapshot:**

```cpp
struct TravelSnapshot {
    RE::FormID currentLocationID = 0;              // player->GetCurrentLocation()->GetFormID()
    std::vector<RE::FormID> parentChain;           // BGSLocation::parentLoc walk, root-first
    RE::FormID holdRegionFormID = 0;               // via Region::ForPlayer()
    Region::Climate climate = Climate::Unknown;
    bool interior = false;
    double sampledAt = 0.0;                        // Unix-epoch seconds
};
```

**Emitted events** (all `type: "travel_event"`, discriminated by `ne_kind`):

- `entered_location` — a new location joined the parent chain (most-specific child took precedence).
- `left_location` — a location dropped out of the parent chain, with no replacement child.
- `crossed_holds` — hold region changed (`holdRegionFormID` diff), regardless of location chain state.
- `entered_wilderness` — location went null AND the hold region also flipped.
- `fast_travel_arrived` — sink-flagged. Includes an origin field captured from the previous snapshot.

Each event bakes the follower list at emission time — walking `RE::ProcessLists::ForEachHighActor`, filtering
alive + `IsPlayerTeammate()` + within `iTravelFollowerRadiusUnits` (default 4000) of the player, capturing
display names. Rendering as "Varian and Jenassa entered Riverwood." / "Varian, Jenassa, and Marcurio arrived in
Solitude, having journeyed from Whiterun." Oxford-comma formatting.

Each event *also* stores both endpoints (`fromLocationID`/`fromHoldRegion` + `toLocationID`/`toHoldRegion`) —
not just the destination — so the condensation pass can identify start-and-end-at-same-place runs without walking
back through prior events.

**Suppression:**

- Interior-to-interior transitions only (e.g. loading-door hop from one building to another, or between
  dungeon zones) are suppressed. Interior↔exterior transitions still emit — entering a house fires
  `entered_location: "Varian entered the Bannered Mare."`, exiting fires `left_location`. The gate is on the
  *pair* of snapshots: skip emission when both `g_lastSnapshot.interior` and the new snapshot's `interior`
  are true.
- While `interior == true`, hold-region diffs are suppressed (interior cells frequently have no assigned
  region, which would otherwise look like a spurious `crossed_holds`). The snapshot preserves the last-known
  hold from the exterior side; when the player exits back to exterior, the fresh region query may then
  legitimately fire a `crossed_holds` if they left through a different hold's door.
- Transitions where nothing changed except the cell (same location, same hold, same interior flag) are
  dropped silently.
- The first `Poll()` after `OnPostLoadGame` seeds baseline, no event emission.

**Condensation** (inside `GetRenderedTail`, per the pattern combat's `hit` events use):

1. Walk the ring buffer oldest-to-newest.
2. Group consecutive travel events into a *run* if each pair's inter-event gap is
   ≤ `iTravelCondensationWindowSeconds` (default 60).
3. `fast_travel_arrived` events are discrete narrative moments and always break a run — they render standalone
   even when adjacent events fall within the window.
4. For each run of 2+ events, first check whether the run is "net zero" — the first event's `from` state
   equals the last event's `to` state. Two sub-cases based on whether the run touched any interior:
   - **Net zero, no interiors involved** (pure exterior back-and-forth — e.g. crossing a hold boundary and
     coming back): **drop the whole run.** The player is where they started and never entered a named
     interior; the churn was noise.
   - **Net zero with one or more interiors visited** (e.g. walked into the Bannered Mare, walked out, walked
     back in, walked back out): emit one `travel_summary` reading "Varian visited the Bannered Mare." The
     player is where they started, but they did *go somewhere*, and the fact that they visited that
     interior is narratively load-bearing. Collect the distinct `toLocationName` values from events in the
     run where `toInterior == true` — if there are multiple, render with Oxford-comma joining
     ("Varian visited the Bannered Mare, Belethor's, and Warmaiden's").
5. For non-net-zero runs of 2+ events: emit one `travel_summary` entry reading "Varian travelled from X to
   Y" (or "…across Whiterun Hold into Falkreath Hold" for hold-only movement, or "…from wilderness to
   Riverwood" for wilderness-to-named). Follower list from the *last* event in the run.
6. Single-event runs render as their original prose.

Raw events remain in the ring buffer either way — the dashboard debug view sees the same condensed output the
LLM does, but detailed inspection is available via logs.

---

## Steps

### Step 1 — Extract shared event-log utilities

- [ ] Complete

**[CLAUDE]**

**Goal:** Pull the small helpers currently private in `CombatEventLog.cpp` (`NowUnixSeconds`,
`NowGameTimeOfDaySeconds`, `WriteString`, `ReadString`) into a shared `EventLogUtil` module so the two new
modules don't duplicate them. Preparatory; no behaviour change.

**Files:**

- `include/EventLogUtil.h` (new).
- `src/EventLogUtil.cpp` (new).
- `src/CombatEventLog.cpp` — remove the four private definitions, replace call sites with the new module.
- `CMakeLists.txt` — add the new source file.

**Sub-tasks:**

1. Author `EventLogUtil.h` exposing four free functions in a `NarrativeEngine::EventLogUtil` namespace:
   `NowUnixSeconds()`, `NowGameTimeOfDaySeconds()`, `WriteString(SerializationInterface*, const std::string&)`,
   `ReadString(SerializationInterface*, std::string&)`.
2. Move the bodies verbatim from `CombatEventLog.cpp` into `EventLogUtil.cpp`.
3. Replace the four call sites in `CombatEventLog.cpp` with `EventLogUtil::` qualified calls.
4. Add the new source to CMake.
5. Run `pwsh -File format.ps1`.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- No functional change in `CombatEventLog` — behaviour is identical.

---

### Step 2 — `Region` module

- [ ] Complete

**[CLAUDE]**

**Goal:** Author the hold / climate resolver that `TravelEventLog` will consume in Step 6 and future biome-
gated beats can consume later.

**Files:**

- `include/Region.h` (new).
- `src/Region.cpp` (new).
- `docs/vanilla/regions/holds.csv` (new) — source-of-truth mapping of vanilla `TESRegion` EditorIDs to hold /
  climate values. Human-authored and version-controlled; the C++ side embeds a mirror.
- `CMakeLists.txt` — add the new source file.

**Sub-tasks:**

1. Author the `Climate` enum, `Resolution` struct, and `ForPlayer` / `ForCell` signatures per the design
   overview. Namespace is `NarrativeEngine::Region`.
2. Author the internal EditorID → `Climate` + display-name table. Includes at minimum: `HoldWhiterunRegion`
   (Tundra), `HoldFalkreathRegion` (Pine), `HoldHjaalmarchRegion` (Marsh), `HoldWinterholdRegion` (Snow),
   `HoldThePaleRegion` (Snow), `HoldTheReachRegion` (Reach), `HoldTheRiftRegion` (Rift),
   `HoldHaafingarRegion` (Coast), `HoldEastmarchRegion` (Volcanic), and `DLC2SolstheimRegion` (Solstheim).
   Confirm exact EditorIDs against the CK / xEdit before shipping the table — do not guess.
3. Author `ForCell(cell)`: null-guard, call `cell->GetRegionList(false)`, walk the returned
   `BSSimpleList<TESRegion*>`, match each region's EditorID against the table, return the first hit at
   hold-level. If nothing matches, return a `Resolution` with `climate = Climate::Unknown`.
4. Author `ForPlayer()` as a convenience: null-guards, calls `ForCell(player->GetParentCell())`.
5. Cache the resolved `RE::TESRegion*` pointers on first call, keyed by EditorID. Log one warning per missed
   EditorID (like `LocationKeywords` does) and fail open (return `Unknown`).
6. Mirror the CSV in `holds.csv` — one row per entry, columns
   `region_editor_id,climate_enum,hold_display_name`.
7. Run `pwsh -File format.ps1`.

**Specifics:**

- The Tamriel root region (`TamrielRegion` or similar) is deliberately unmapped — it's the parent of all outdoor
  cells and matching it would swallow every hold. Only per-hold regions live in the table.
- `LookupByEditorID` requires powerofthree's Tweaks; the plugin already declares this as a hard dep for
  `LocationKeywords`, so no new dependency.
- The resolution table has ~10 entries; a linear scan is fine, no need for a hash map.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- No runtime callers yet; correctness is validated indirectly via Step 9's in-game travel test.

---

### Step 3 — `WeatherEventLog` module (infrastructure)

- [ ] Complete

**[CLAUDE]**

**Goal:** Author the weather event log — poll, ring buffer, cosave, category diff — but *not* the rendering
vocabulary or the tail-merge wire-in yet. Splitting keeps this step tightly bounded.

**Files:**

- `include/WeatherEventLog.h` (new).
- `src/WeatherEventLog.cpp` (new).
- `src/Tick.cpp` — add `WeatherEventLog::Poll()` alongside the existing `CombatEventLog::Poll()`.
- `src/Plugin.cpp` — register `WeatherEventLog::Initialize()` at `kDataLoaded`,
  `WeatherEventLog::OnPostLoadGame()` at `kPostLoadGame`, cosave record `'NEWE'`.
- `include/Settings.h`, `src/Settings.cpp` — add `[WeatherEvents]` section.
- `statics/SKSE/Plugins/NarrativeEngine.ini` — add `[WeatherEvents]` defaults.
- `CMakeLists.txt` — add the new source file.

**Sub-tasks:**

1. Public surface mirroring `CombatEventLog`: `Initialize`, `Shutdown`, `OnPhaseAdvanced`, `Poll`,
   `GetRenderedTail`, `OnPostLoadGame`, `OnSave`, `OnLoad`, `OnRevert`. Cosave type ID `'NEWE'` = "NEt WEather".
2. Internal `WeatherCategory` struct with the priority-ordered `primary` enum (`Snowy` > `Rainy` > `Pleasant`
   > `Cloudy` > `Other`) and derived `stormy` bool. Author a `DeriveCategory(TESWeather*)` free function that
   reads `data.flags` and `data.thunderLightningFrequency`.
3. Internal `InternalEvent { fromCategory; toCategory; localTime; gameTime; }` — no actor fields; weather
   events have no participants. Rendering happens at `GetRenderedTail` time using the categories.
4. `Poll()`: main-thread, mutex-locked. First gate — early-return cheaply if
   `NowUnixSeconds() - g_lastPolledAt < iWeatherEventPollIntervalSeconds`. Weather changes take tens of
   seconds; the Tick's 500ms cadence is far too aggressive for this signal. When the interval elapses,
   update `g_lastPolledAt` and continue: read `Sky::GetSingleton()`, suppress if
   `sky->mode != Sky::Mode::kFull`, derive category from `sky->currentWeather`, compare to `g_lastCategory`;
   on mismatch AND if `NowUnixSeconds() - g_lastEmittedAt >= iWeatherEventDebounceSeconds`, push an event and
   update `g_lastCategory` + `g_lastEmittedAt`.
5. `OnPostLoadGame()`: seed `g_lastCategory` from the current sky, set `g_lastEmittedAt = NowUnixSeconds()`
   and `g_lastPolledAt = NowUnixSeconds()` so no event fires on the first post-load poll and the interval
   gate starts fresh.
6. `OnPhaseAdvanced()`: drop events older than `PhaseTracker::PhaseStartRealTime()`. (Same shape as combat's
   phase-prune but simpler — no encounter concept.)
7. Cosave payload: `uint32_t count`, then per-event
   `{ uint8_t fromPrimary; uint8_t fromStormyByte; uint8_t toPrimary; uint8_t toStormyByte; double localTime;
   double gameTime; }`. Version `1`.
8. Wire `Poll()` into `Tick::Poll` after `CombatEventLog::Poll()`. Wire `Initialize()`, `OnPostLoadGame()`,
   and the cosave record into `Plugin.cpp` alongside the existing `CombatEventLog` registrations.
9. Settings additions:
   - `int weatherEventsMaxStored = 128;` (`[WeatherEvents] iWeatherEventsMaxStored`)
   - `int weatherEventPollIntervalSeconds = 30;` (`[WeatherEvents] iWeatherEventPollIntervalSeconds`)
   - `int weatherEventsDebounceSeconds = 20;` (`[WeatherEvents] iWeatherEventDebounceSeconds`)
10. Mirror the defaults in `statics/SKSE/Plugins/NarrativeEngine.ini`.
11. Author `GetRenderedTail` as a stub that returns an empty array — Step 4 fills in the rendering.
12. Run `pwsh -File format.ps1`.

**Specifics:**

- `Sky::mode == kFull` is the "we're outdoors with a real sky" gate. `kSkyDomeOnly` covers small dungeon
  entrances that keep a sky visible but not the full weather system — treat those as interior too.
- `sky->currentWeather` can be null briefly during weather transitions or immediately post-load. Null derives
  to `Other` primary + `stormy=false`; a null-to-null diff is a no-op.
- The category diff must consider `stormy` — a same-primary transition where stormy flipped
  (e.g. rainy → stormy rainy) is a meaningful event.
- Do NOT suppress emission when `sky->overrideWeather != nullptr`. See the design overview — we explicitly
  want to observe overrides.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- Rendering not yet wired; end-to-end validation deferred to Step 5.

---

### Step 4 — `WeatherEventLog` rendering + tail merge

- [ ] Complete

**[CLAUDE]**

**Goal:** Fill in the transition-to-sentence vocabulary and thread the weather tail through
`BuildMergedTimeline` so weather events land in `recent_events` alongside SkyrimNet and combat events.

**Files:**

- `src/WeatherEventLog.cpp` — implement `GetRenderedTail`.
- `include/SkyrimNetEvents.h`, `src/SkyrimNetEvents.cpp` — extend `BuildMergedTimeline` to accept a weather
  array parameter.
- `src/EvaluationPipeline.cpp` — pass `WeatherEventLog::GetRenderedTail(...)` into `BuildMergedTimeline`.
- `src/DashboardUIManager.cpp` — same, mirrored so the dashboard sees the identical stream.

**Sub-tasks:**

1. Author the static transition-to-sentence table inside `WeatherEventLog.cpp`. Suggested vocabulary:
   - `(!Pleasant → Pleasant, any stormy)` → "The clouds have parted and the sun is coming out."
   - `(Pleasant → Cloudy)` → "The sky has clouded over."
   - `(Cloudy → Rainy)` / `(Pleasant → Rainy)` → "Rain has started to fall."
   - `(Rainy → !Rainy && !Snowy)` → "The rain has stopped."
   - `(any → Snowy, !stormy)` → "Snow is beginning to fall."
   - `(Snowy → !Snowy && !Rainy)` → "The snow is letting up."
   - `(Rainy, !stormy → Rainy, stormy)` → "A thunderstorm is picking up."
   - `(Snowy, !stormy → Snowy, stormy)` → "A blizzard has struck."
   - `(any stormy → same primary, !stormy)` → "The storm is subsiding."
   - Fallback for any unmapped transition: "The weather has shifted."
2. `GetRenderedTail` walks the event ring, renders each event via the table, prefixes with
   `[N ago]` from `SkyrimNetEvents::FormatRelativeGameTime`, emits JSON matching the SkyrimNet event shape:
   `{ type: "weather_event", ne_kind: "<transition_slug>", localTime, gameTime, originatingActorName: "",
   targetActorName: "", text: "..." }`. `ne_kind` is a stable slug per transition
   (`"clear_start"`, `"rain_start"`, `"storm_start"`, etc.) so future callers can branch on kind.
3. Extend `BuildMergedTimeline`'s signature to take a third array (weather events). Weather events are never
   condensed — they surface as-is like combat's `combat_start` / `collapse`. The merge is now a three-way
   stable sort by `localTime`; the combat-hit condensation logic is untouched.
4. Update the two call sites (`EvaluationPipeline.cpp`, `DashboardUIManager.cpp`) to pass the new argument.
5. Run `pwsh -File format.ps1`.

**Specifics:**

- The `type: "weather_event"` label lets a future prompt-side handler style weather differently from combat
  and travel. The LLM only reads `text`, so this is a dashboard / debug affordance.
- Transition slug naming: `<primary_lowercase>_<direction>` where direction is `start` / `stop` / `intensify` /
  `subside`. Keep the slug list open — extension is one table row, not a code change.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- End-to-end verification of actual weather events landing deferred to Step 5.

---

### Step 5 — In-game verification of `WeatherEventLog`

- [ ] Complete

**[USER]**

**Goal:** Confirm the weather event log correctly observes weather transitions in-game, renders sensible
sentences, respects the interior gate, and persists across save/load.

**Files:** None. In-game only.

**Sub-tasks:**

1. Launch Skyrim with the newly-built plugin. Load a save with the player outdoors somewhere calm.
2. Open the dashboard (default `F7`) and confirm the baseline `recent_events` tail contains no weather
   entries.
3. Force a series of weather changes via console:
   - `fw 81a` (SkyrimClear) — expect a "clouds have parted" or "weather shifted" entry, depending on the
     starting state.
   - `fw c8220` (SkyrimStormRain) — expect a "thunderstorm picking up" entry.
   - `fw c8221` (SkyrimStormSnow) — expect a "blizzard has struck" entry.
   - `fw 81a` again — expect a "storm is subsiding" or "clear" entry.
4. Confirm each transition produces exactly one entry in the dashboard's `recent_events`, with the correct
   `ne_kind` slug and rendered text.
5. Enter an interior (any building). Force another weather change via console. Confirm no event fires while
   indoors.
6. Exit the interior. Confirm the current weather state is observed on the next poll (there may be a
   transition event if it differs from the last-observed state pre-interior; that's expected).
7. Save the game mid-storm. Reload. Confirm no bogus "weather started" event fires on the first post-load
   poll — the baseline should be seeded silently.
8. Trigger a Director phase advance (wait, or force via console). Confirm weather events from before the
   phase advance are pruned from the next dashboard refresh.
9. Check the SKSE log for any `WeatherEventLog` warnings or errors.

**Failure modes to watch:**

- Duplicate events firing on a single transition — suggests the debounce isn't holding or the category
  derivation is oscillating mid-fade.
- Events firing indoors — suggests the `Sky::mode` gate isn't taking effect.
- Post-load ghost event — suggests `OnPostLoadGame` isn't seeding baseline correctly.
- "The weather has shifted" fallback firing for a transition that should have a specific sentence — flag the
  from/to categories in the report so the table can be extended.

---

### Step 6 — `TravelEventLog` module (snapshot + poll + cosave)

- [ ] Complete

**[CLAUDE]**

**Goal:** Author the travel event log's core observation loop — snapshot, poll-diff, event emission with
baked follower names, cosave. No fast-travel specialization yet, no rendering vocabulary yet, no condensation
yet. Purely the "diff and emit" spine.

**Files:**

- `include/TravelEventLog.h` (new).
- `src/TravelEventLog.cpp` (new).
- `src/Tick.cpp` — add `TravelEventLog::Poll()` after `WeatherEventLog::Poll()`.
- `src/Plugin.cpp` — register `TravelEventLog::Initialize()` at `kDataLoaded`,
  `TravelEventLog::OnPostLoadGame()` at `kPostLoadGame`, cosave record `'NETR'`.
- `include/Settings.h`, `src/Settings.cpp` — add `[TravelEvents]` section.
- `statics/SKSE/Plugins/NarrativeEngine.ini` — mirror the defaults.
- `CMakeLists.txt` — add the new source file.

**Sub-tasks:**

1. Public surface mirroring `WeatherEventLog`. Cosave type ID `'NETR'` = "NEt TRavel".
2. Internal `TravelSnapshot` per the design overview. `parentChain` is populated by walking
   `BGSLocation::parentLoc` starting from `player->GetCurrentLocation()`, root-first, bounded to prevent
   pathological loops (cap at 8 hops; a real Skyrim chain is typically 2–3 deep). `climate` and
   `holdRegionFormID` are populated via `Region::ForPlayer()` when the player is on an exterior cell, and
   preserved from `g_lastSnapshot` when the player is on an interior cell (interior cells frequently have no
   assigned region).
3. Internal `InternalEvent`:

   ```cpp
   enum class Kind : uint8_t {
       EnteredLocation, LeftLocation, CrossedHolds, EnteredWilderness, FastTravelArrived,
   };
   struct InternalEvent {
       Kind kind;
       double localTime;
       double gameTime;
       // Endpoint fields — both sides captured for condensation
       RE::FormID fromLocationID = 0;
       std::string fromLocationName;
       bool fromInterior = false;
       RE::FormID fromHoldRegionID = 0;
       std::string fromHoldName;
       RE::FormID toLocationID = 0;
       std::string toLocationName;
       bool toInterior = false;
       RE::FormID toHoldRegionID = 0;
       std::string toHoldName;
       // Baked at emission
       std::vector<std::string> partyNames; // player + followers, in intended render order
   };
   ```

4. `CollectFollowers()` helper: walk `RE::ProcessLists::ForEachHighActor`, filter `!IsDead()` +
   `IsPlayerTeammate()` + within `iTravelFollowerRadiusUnits` (default 4000) of the player, sort by display
   name for stability, return names. Player display name is always the first entry in `partyNames`.
5. `Poll()`: main-thread, mutex-locked. Build a `TravelSnapshot` with `interior` set from
   `player->GetParentCell()->IsInteriorCell()`. If BOTH `g_lastSnapshot.interior` and the new snapshot's
   `interior` are true, just update `g_lastSnapshot` and return — interior-to-interior transitions produce
   no events. Otherwise diff against `g_lastSnapshot` and emit events per the design
   (`entered_location` / `left_location` / `crossed_holds` / `entered_wilderness`). When either side is
   interior, suppress `crossed_holds` / `entered_wilderness` — only location-chain events fire on
   interior↔exterior transitions. Skip if the only diff is `sampledAt` or a cell change with no
   location/hold/interior change.
6. Emission helper bakes `partyNames` from `CollectFollowers()` plus the player. `fromX` fields populated
   from `g_lastSnapshot`; `toX` from the new snapshot.
7. `OnPostLoadGame()`: seed `g_lastSnapshot` from the current world state, no event emission.
8. `OnPhaseAdvanced()`: drop events older than `PhaseTracker::PhaseStartRealTime()`.
9. Cosave payload: standard `count` prefix, per-event
   `{ uint8_t kind; double localTime; double gameTime; uint32_t fromLocID; str fromLocName;
   uint32_t fromHoldID; str fromHoldName; uint32_t toLocID; str toLocName; uint32_t toHoldID;
   str toHoldName; uint16_t partyCount; str[partyCount] partyNames; }`. Version `1`.
10. Settings additions:
    - `int travelEventsMaxStored = 128;` (`[TravelEvents] iTravelEventsMaxStored`)
    - `int travelCondensationWindowSeconds = 60;`
      (`[TravelEvents] iTravelCondensationWindowSeconds` — read in Step 8)
    - `int travelFollowerRadiusUnits = 4000;` (`[TravelEvents] iTravelFollowerRadiusUnits`)
11. Wire `Poll()` into `Tick::Poll` and registrations into `Plugin.cpp`.
12. Stub `GetRenderedTail` returning an empty array — Step 8 fills it in.
13. Run `pwsh -File format.ps1`.

**Specifics:**

- `player->GetCurrentLocation()` returns null in cells with no assigned location — that's the "wilderness"
  state on exterior cells. Handle explicitly; don't treat null as an error.
- The parent-chain diff for `entered_location` should identify the most-specific location added (the deepest
  new node in the chain). Same for `left_location` — the most-specific location removed. Interior locations
  are typically their own most-specific node (e.g. `WhiterunBanneredMareLocation` is a child of
  `WhiterunLocation`), so entering a building fires `entered_location` for the building.
- `crossed_holds` fires whenever `holdRegionFormID` changes on an exterior-to-exterior transition, regardless
  of whether location also changed. When both fire on the same tick (e.g. crossing from Riverwood into
  Falkreath wilderness), emit both events — the condensation pass in Step 8 will collapse them if they're
  noise, and they carry different information otherwise.
- `entered_wilderness` is emitted (exterior→exterior only) when location goes null AND hold *did not*
  change. When location goes null AND hold also changed, that's `left_location` + `crossed_holds` instead —
  carries the same information but more precisely.
- Interior↔exterior transitions emit only the location-chain event (`entered_location` on going indoors,
  `left_location` on coming back out). Hold-tracking is preserved across the interior visit via the
  snapshot's hold fields, so an exit-into-a-different-hold correctly fires `crossed_holds` on the
  next Poll after exiting.
- Follower detection is at emission time only. If followers dismount / recruit mid-travel, the summary in
  Step 8's condensation uses the final party — deliberate, matches the "who's with you now" narrative frame.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- Rendering / condensation / merge not yet wired; end-to-end validation deferred to Step 9.

---

### Step 7 — Fast-travel specialization sink

- [ ] Complete

**[CLAUDE]**

**Goal:** Hook `TESFastTravelEndEvent` so travel events resulting from fast travel (map or carriage) render
with the "having journeyed from X" flourish. Isolated as its own step so the base observation loop can ship
without depending on this.

**Files:**

- `src/TravelEventLog.cpp` — add the sink, wire the flag through the emission path.

**Sub-tasks:**

1. Add a `FastTravelEndSink : public BSTEventSink<TESFastTravelEndEvent>` inside the anonymous namespace,
   registered from `Initialize()` against `ScriptEventSourceHolder::GetSingleton()`.
2. On event fire (arriving on the SKSE thread), grab the mutex and set a `g_fastTravelPending` flag along with
   `g_fastTravelSampledAt = NowUnixSeconds()`. Do NOT emit an event from the sink itself — the next `Poll()`
   will see the transition and consult the flag.
3. In `Poll()`, when preparing to emit what would be an `EnteredLocation` or `CrossedHolds` event, check
   `g_fastTravelPending`. If set AND `NowUnixSeconds() - g_fastTravelSampledAt < 5.0` (short window; the
   engine transitions fast), upgrade the event kind to `FastTravelArrived` and clear the flag.
4. `FastTravelArrived` events use the same endpoint capture (fromLocationID = pre-fast-travel snapshot,
   toLocationID = post) — the poll-diff already computes this correctly.
5. `Shutdown()` deregisters the sink.
6. Run `pwsh -File format.ps1`.

**Specifics:**

- Carriage travel: verify `TESFastTravelEndEvent` fires — CommonLibSSE-NG's event source reference suggests it
  does. If in-game verification (Step 9) shows it doesn't, add a targeted quest-script hook in a follow-up;
  don't block Step 7 on it.
- The 5-second flag window prevents a stale flag from mislabelling an unrelated later transition as fast
  travel. Tunable if the observed poll latency is longer; log a warning if the flag ever expires without
  being consumed, which would indicate the poll missed the intended transition.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- Confirmation of behaviour is part of Step 9.

---

### Step 8 — `TravelEventLog` rendering + condensation + tail merge

- [ ] Complete

**[CLAUDE]**

**Goal:** Fill in the rendering vocabulary, implement the run-collapsing condensation pass in
`GetRenderedTail`, and thread the travel tail through `BuildMergedTimeline` so travel events land in
`recent_events`.

**Files:**

- `src/TravelEventLog.cpp` — implement `GetRenderedTail` with rendering + condensation.
- `include/SkyrimNetEvents.h`, `src/SkyrimNetEvents.cpp` — extend `BuildMergedTimeline` to accept a travel
  array parameter.
- `src/EvaluationPipeline.cpp` — pass `TravelEventLog::GetRenderedTail(...)`.
- `src/DashboardUIManager.cpp` — mirror.

**Sub-tasks:**

1. Author a `RenderParty(const std::vector<std::string>& names)` helper: 1 name → `"Varian"`; 2 → `"Varian
   and Jenassa"`; 3+ → Oxford-comma `"Varian, Jenassa, and Marcurio"`.
2. Author `RenderEvent(const InternalEvent& e)`:
   - `EnteredLocation` → `"<Party> entered <toLocationName>."`
   - `LeftLocation` → `"<Party> left <fromLocationName>."`
   - `CrossedHolds` → `"<Party> crossed from <fromHoldName> into <toHoldName>."`
   - `EnteredWilderness` → `"<Party> left <fromHoldName> and is now in the wilderness."` (When `partyCount`
     > 1, "are" instead of "is"; keep the grammar-agreement helper small.)
   - `FastTravelArrived` →
     `"<Party> arrived in <toLocationName || toHoldName>, having journeyed from <fromLocationName || fromHoldName>."`
   - Prefix each with `[N ago]` from `FormatRelativeGameTime`.
3. Author two summary renderers for condensed runs, both taking the full run span:
   - `RenderJourneySummary(const std::vector<InternalEvent>& run)` for non-net-zero runs — reads
     `run.front()`'s `from` endpoint and `run.back()`'s `to` endpoint: "`<Party> travelled from
     <run.front().from{Location||Hold}Name> to <run.back().to{Location||Hold}Name>.`" Uses
     `run.back().partyNames`.
   - `RenderVisitSummary(const std::vector<InternalEvent>& run)` for net-zero runs that touched interiors —
     collects distinct `toLocationName` values from events in the run where `toInterior == true`, then
     renders "`<Party> visited <interior_1>.`" (single), "`<Party> visited <interior_1> and <interior_2>.`"
     (two), or Oxford-comma joined for 3+. Uses `run.back().partyNames`.
4. Implement the condensation pass per the design:
   - Group consecutive events into runs where each pair's `localTime` gap is ≤
     `iTravelCondensationWindowSeconds`.
   - `FastTravelArrived` always breaks a run — it stays standalone regardless of adjacency.
   - Runs of length 1 render as-is via `RenderEvent`.
   - Runs of length 2+: compute `netZero = (run.front().from<Location||Hold>ID == run.back().to<Location||
     Hold>ID)` and `visitedInteriors = distinct toLocationName from events where toInterior == true`. Three
     outcomes:
     - `netZero && visitedInteriors.empty()` → drop the whole run.
     - `netZero && !visitedInteriors.empty()` → emit `RenderVisitSummary(run)`.
     - `!netZero` → emit `RenderJourneySummary(run)`.
5. Emitted JSON for both standalone and summary entries:
   `{ type: "travel_event", ne_kind: "<slug>", localTime, gameTime, originatingActorName: <player_name>,
   targetActorName: "", text: "..." }`. Slug is the event's `Kind` for standalone, `"travel_summary"` for
   condensed.
6. Extend `BuildMergedTimeline` to take a fourth array (travel events). Same three-way merge shape as after
   Step 4 became four-way — stable sort by `localTime`, no cross-source condensation.
7. Update the two call sites.
8. Run `pwsh -File format.ps1`.

**Specifics:**

- The condensation-window setting is read from `Settings::Get().travelCondensationWindowSeconds` inside
  `GetRenderedTail`; keep it a settings-read per call, not a cached copy — matches how CombatEventLog treats
  its analogous knobs.
- The from/to state comparison for "netted zero" runs uses whichever endpoint field is non-zero (location
  takes precedence over hold). A run that crosses a hold boundary and comes back with no interior visits
  nets zero and drops entirely; a run that walks into a building and back out nets zero but collapses to a
  "visited" summary instead — the presence of interior transitions distinguishes the two cases.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- End-to-end verification deferred to Step 9.

---

### Step 9 — In-game verification of `TravelEventLog`

- [ ] Complete

**[USER]**

**Goal:** Confirm travel events correctly land in `recent_events` across the full set of transition shapes,
that condensation collapses back-and-forth noise, that fast-travel gets the special phrasing, and that
followers appear correctly.

**Files:** None. In-game only.

**Sub-tasks:**

1. Launch Skyrim with the newly-built plugin. Load a save with the player standing in Whiterun (city
   interior or exterior — start outside).
2. Open the dashboard and confirm the baseline `recent_events` tail contains no travel entries.
3. **Enter / leave a town:** walk out through the Whiterun main gate to the plains, then back through the
   gate. Confirm `entered_wilderness` (or `left_location`, depending on how Whiterun's location chain
   resolves) → `entered_location` sequence, then confirm they condense if the round-trip completes within
   the condensation window.
4. **Cross a hold boundary:** travel by foot from Whiterun Hold plains into Falkreath Hold. Confirm a
   `crossed_holds` event lands with the correct from/to hold names — this validates `Region`
   end-to-end.
5. **Enter a named settlement:** walk into Riverwood. Confirm `entered_location` fires with "Varian entered
   Riverwood." Walk back out. Confirm `left_location` fires.
6. **Test condensation:** walk in and out of Riverwood three times in quick succession. Confirm the
   dashboard's rendered tail shows the run collapsed (either dropped entirely if you end up outside where
   you started, or a single `travel_summary` reflecting net displacement).
7. **Fast travel:** open the map, fast travel from Whiterun to Solitude. Confirm a `fast_travel_arrived`
   event fires with "Varian arrived in Solitude, having journeyed from Whiterun." (Or similar — the
   exact from/to names depend on which locations resolved at each end.)
8. **Carriage travel:** hire a carriage from a hold capital. Confirm a `fast_travel_arrived` event fires on
   arrival. If it doesn't, note it in the report — a targeted quest-script hook can be added in a follow-up.
9. **Followers:** recruit any follower (Lydia, Jenassa, etc.). Repeat the "enter Riverwood" test. Confirm
   the rendered text is "Varian and `<Follower>` entered Riverwood." Recruit a second follower; confirm
   Oxford-comma rendering.
10. **Interior gate:** enter a house / shop (e.g. the Bannered Mare). Confirm an `entered_location` event
    fires ("Varian entered the Bannered Mare"). Walk to a different room inside the same interior — confirm
    no event fires. Exit back to the street — confirm `left_location` fires. If the interior has a loading
    door to another interior (e.g. Dragonsreach's Great Hall to its Porch), pass through and confirm no
    event fires on that interior→interior transition.
11. **Interior round-trip condensation:** enter and exit the Bannered Mare three times in quick succession
    (within the condensation window). Confirm the run collapses to a single "Varian visited the Bannered
    Mare." summary — NOT dropped entirely (interior visits are load-bearing even when the player ends up
    where they started).
    - Then walk into and out of two different buildings in the same short window (e.g. Bannered Mare
      *and* Belethor's). Confirm the summary combines them: "Varian visited the Bannered Mare and
      Belethor's General Goods."
    - Contrast: cross the Whiterun→Falkreath hold boundary on foot and immediately turn around and cross
      back. Since no interior was involved, confirm this exterior-only round trip is dropped entirely
      (nothing appears for it in the tail).
12. **Persistence:** save mid-travel (e.g. mid-way between two holds). Reload. Confirm no bogus initial
    event fires on the first post-load poll.
13. **Phase advance:** trigger a Director phase advance. Confirm travel events from before the advance
    are pruned.
14. Check the SKSE log for any `TravelEventLog` warnings, missing region editor IDs, or emit-path errors.

**Failure modes to watch:**

- Missing region resolution — a `Region` warning about an unrecognised `TESRegion` EditorID suggests
  the Step 2 table needs an addition. Report the EditorID.
- Fast-travel event tagged as `entered_location` instead of `fast_travel_arrived` — suggests the sink flag
  isn't firing or the poll consumed it too late. Log lines around the transition will show the flag state.
- Condensation dropping events it shouldn't (or failing to drop events it should) — flag the specific
  sequence and the observed vs. expected output.
- Follower list wrong — Serana / summoned creatures / mount-dismissed followers are known edge cases;
  document what's missing and defer the fix.

---

## Persistence summary

Two new cosave records, both following `CombatEventLog`'s discipline:

| Record | Owner              | Payload                                                                    |
|--------|--------------------|----------------------------------------------------------------------------|
| `NEWE` | `WeatherEventLog`  | Weather event ring (from/to category tuples + timestamps).                 |
| `NETR` | `TravelEventLog`   | Travel event ring (from/to endpoints + baked party names + timestamps).    |

Neither record persists any state that the module can rebuild from live world state on load. The last-observed
`WeatherCategory` and the last-observed `TravelSnapshot` are seeded fresh in each module's `OnPostLoadGame`.

Both `OnSave` implementations run their `OnPhaseAdvanced` prune step immediately before writing, so on-disk
content matches in-memory pruning rules.

---

## Settings summary

New `[WeatherEvents]` section:

- `iWeatherEventsMaxStored` — ring-buffer cap. Default `128`.
- `iWeatherEventPollIntervalSeconds` — minimum real seconds between weather-state samples. Weather changes
  on the order of tens of seconds, so the module throttles internally rather than sampling every Tick.
  Default `30`.
- `iWeatherEventDebounceSeconds` — minimum inter-event gap, applied on top of the poll interval. Default `20`.

New `[TravelEvents]` section:

- `iTravelEventsMaxStored` — ring-buffer cap. Default `128`.
- `iTravelCondensationWindowSeconds` — max inter-event gap within a condensable run. Default `60`.
- `iTravelFollowerRadiusUnits` — follower-inclusion distance gate. Default `4000`.

All four go through the same INI cascade every other setting uses (plugin INI → MCM override → in-code
default). No dashboard UI surface — modder-tunables only.

---

## Out-of-scope smoke tests

Failure modes not covered by the numbered verification steps but worth watching for once the phase lands:

- LLM prompt bloat from too many weather / travel entries surviving merge. If observed, lower the ring-buffer
  caps or narrow the debounce / condensation windows.
- `TESRegion` mis-detection in the DLC02 Solstheim worldspace — verify with a Solstheim save if available;
  fall through to `Unknown` is acceptable if the DLC isn't installed.
- Interior-vs-exterior race on cell load — a rapid door-transition might briefly show the wrong
  `IsInteriorCell` reading. Manifest as a spurious `entered_wilderness` / `crossed_holds` right after
  exiting a house. If seen, add a one-poll debounce on interior-exterior transitions.
