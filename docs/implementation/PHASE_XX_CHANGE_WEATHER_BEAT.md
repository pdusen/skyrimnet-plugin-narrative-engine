# Phase XX — Change Weather Beat

The first beat that uses Phase 9's observation infrastructure as its downstream signal loop. `ChangeWeatherBeat`
is the Director's cheapest lever: pick a region-appropriate vanilla weather form, kick off the engine's own
gradual weather transition (the C++ equivalent of the `sw` console command), post a one-line framing
notification, done. Everything else — the sky actually shifting over the next ~30 seconds, the change landing
in `recent_events` for the next Director cycle, downstream beats reacting to "the storm is rolling in" — is
already-shipped machinery this phase just triggers.

This is deliberately the smallest possible beat. No ESP-side addition. No cosave record. No `CLEANUP` state.
No dwell timer. No restore-on-completion apparatus. The whole beat runs synchronously inside `OnStart`;
the very first `Tick` returns `NOT_RUNNING` and the top-level slot releases one poll interval later. Its role
in the design is precisely to be an "always available" `Either`-polarity fallback the Director can reach for
when nothing more ambitious is on the table.

---

## Why this phase exists

Three shipped beats (`ambush`, `npc_letter`, `npc_visit`) all involve moving objects, aliased NPCs, and multi-
tick quest state machines. That's expensive to build and expensive to run. The Director's `Either`-polarity
surface is thin — `npc_letter` and `npc_visit` can bend either direction but both need viable engagement pools,
and `ambush` is `Raise`-only and interior-blocked. There are stretches where the phase-dwell gate says "pick
something" and the candidate set is empty.

Weather change is the natural fill for that gap. It's polarity-flexible (a stormfront raises tension; a
sunbreak lowers it), it's available essentially everywhere outdoors, it's genuinely narratively load-bearing
(Skyrim's sky is a mood layer the vanilla game already leans on), and — critically — the whole thing is a
single API call plus a notification. No spawning, no aliases, no combat, no dialogue trees. Zero blast radius
on the rest of the systems.

Phase 9 shipped exactly the pieces this beat needs: `Region::ForPlayer()` for climate-appropriate weather
filtering, and `WeatherEventLog` for the observation-side feedback loop. When `ChangeWeatherBeat` fires,
`WeatherEventLog` picks up the change on its next poll and it lands in the LLM's `recent_events` tail one
cycle later — downstream beats react to "the sky just darkened" without knowing whether the change was
authored or ambient. No special-case pathway required; no observation suppression.

---

## Scope

### In scope

- A new **`ChangeWeatherBeat`** implementing `IBeat`, registered in `BeatRegistry` alongside the existing
  three beats.
- A new small **`WeatherManifest`** module owning the curated table of ~10 vanilla weather forms, each with
  a polarity mask (`Raise` / `Lower` / `Either`) and a climate mask (which of the ten `Region::Climate`
  values it's authored for). Exposes a `WeathersPermittedFor(climate, desiredDirection)` filter and a name
  → entry lookup.
- The beat's whole lifecycle runs synchronously inside `OnStart` on the main thread: validate the LLM's
  chosen weather name against the manifest + current climate, call
  `RE::Sky::GetSingleton()->SetWeather(w, /*override=*/false, /*accelerate=*/false)` (the `sw`-equivalent
  gradual fade), post the LLM's framing line via `RE::DebugNotification`, stamp the per-beat cooldown. First
  `Tick` returns `{BeatState::NOT_RUNNING}` unconditionally.
- A new INI section `[ChangeWeatherEvents]` with two tunables: the per-beat cooldown in in-game hours and
  a debug toggle for logging the manifest filter's per-tick result.
- Author the LLM-facing beat description (long-form, listed alongside `npc_letter` / `npc_visit` / `ambush`
  in the `narrative_engine_action_select` prompt) enumerating the manifest surface, the parameter schema,
  and the "the beat only chooses from these weathers" contract.
- `IBeat::Abort()` implementation — essentially a no-op logging line, since by the time an abort can be
  issued the `SetWeather` call has already completed synchronously. Present because `Abort` is
  pure-virtual on `IBeat`.

### Deferred (explicitly out)

- **Restore-on-cleanup / weather-ownership tracking.** The beat is fire-and-forget: whatever weather it
  sets stays set until the region system organically reasserts (which happens when the player crosses a
  cell boundary anyway). No `ReleaseWeatherOverride` call, no captured prior-FormID, no cosave record,
  no owner-verification check.
- **A dwell timer or "success measured by weather still holding at time T" logic.** Success is defined as
  the initial `SetWeather` call returning + the notification posting.
- **Per-weather cooldowns or per-climate cooldowns.** The two active gates are the global
  `iBeatCooldownSeconds` (shared across all beats) and this beat's own per-beat cooldown in in-game hours.
- **User-facing manifest tuning.** The ~10-entry manifest ships hardcoded in C++. Player customisation via
  MCM or INI is a future item if it turns out anyone wants it.
- **Interior weather awareness.** The beat is exterior-only; interior cells fail `IsAvailable`.
- **Cross-mod weather compatibility passes.** The manifest names vanilla + DLC weather EditorIDs only.
  Third-party weather overhauls (Vivid Weathers, Obsidian Weathers, etc.) will still see their variants
  chosen when the vanilla weather system organically decides to fire them; the beat itself doesn't reach
  into them.
- **Any changes to `WeatherEventLog`, `Region`, or `HoldGrid`.** All three are consumed as-is.
- **Any ESP-side addition.** No new quest, no new record, no CK work whatsoever. Papyrus `.psc` unchanged.

---

## Design overview

### Beat shape

`ChangeWeatherBeat` is the first beat to make full use of the fact that `IBeat::OnStart` runs on the main
thread. The existing three beats all defer real work into `Tick` via `AsyncDispatch::MarshalToMainThread`
because they need to marshal multiple round-trips (LLM compose callbacks, quest alias-fill polls, courier-
container checks). This beat needs none of that: it validates parameters, makes one engine call, posts one
notification, and is done. All of it fits inside `OnStart`.

The `IBeat` state machine still applies formally — the beat enters `BEAT_RUNNING` with sub-state `COMPOSE`
when `OnStart` returns — but the beat treats every `Tick` call as an immediate signal to release the slot:

```cpp
TickResult ChangeWeatherBeat::Tick(TickMode /*mode*/, BeatState /*state*/)
{
    return {BeatState::NOT_RUNNING};
}
```

The top-level slot flips out of `BEAT_RUNNING` one master-poll interval later (~250 ms). During that window
the beat holds the single-beat lock the same way any other beat does; that's the entire reason we don't
skip `OnStart → Tick` and just fire-and-return from `StartBeat`. Consistency with the rest of the beat
system is worth 250 ms of dead air.

### Weather manifest

The manifest ships as a hardcoded array in C++, with a mirror in `docs/vanilla/weathers/manifest.csv` for
auditability (same pattern `Region` uses for `holds.csv`).

```cpp
namespace NarrativeEngine::WeatherManifest {
    enum class Polarity : std::uint8_t { Raise, Lower, Either };

    struct WeatherEntry {
        const char*      name;                 // stable identifier for the LLM
        const char*      editorID;             // vanilla EditorID for resolution
        Polarity         polarity;
        std::uint32_t    climateMask;          // bitmask of Region::Climate values
        const char*      shortDescription;     // one-line for the LLM
    };

    // Resolve the entry's TESWeather* at first use; cache. Returns nullptr
    // on unknown name or unresolved EditorID (log-once).
    RE::TESWeather* Resolve(std::string_view name);

    // Filter the manifest to entries permitted for climate + direction.
    // Called by IsAvailable to check the pool is non-empty, and by
    // BuildBeatSelectPromptContext to build the LLM's candidate list.
    std::vector<const WeatherEntry*> WeathersPermittedFor(
        Region::Climate climate,
        PhaseTracker::Direction desiredDirection);

    // Direct lookup by name (used by OnStart's validation pass).
    const WeatherEntry* Find(std::string_view name);
}
```

Manifest contents (starting set — final list authored in Step 1):

| name                 | EditorID              | polarity | climates                                              |
| -------------------- | --------------------- | -------- | ----------------------------------------------------- |
| `fog_ambient`        | `SkyrimFog`           | Either   | all                                                   |
| `cloudy_calm`        | `SkyrimCloudy`        | Lower    | all                                                   |
| `clear_sky`          | `SkyrimClearSky`      | Lower    | all                                                   |
| `overcast_still`     | `SkyrimOvercast`      | Either   | all                                                   |
| `storm_rain`         | `SkyrimStormRain`     | Raise    | Tundra, Pine, Marsh, Reach, Rift, Coast, Volcanic     |
| `storm_snow`         | `SkyrimStormSnow`     | Raise    | Snow, Reach                                           |
| `overcast_rain`      | `SkyrimOvercastRain`  | Raise    | Tundra, Pine, Marsh, Reach, Rift, Coast, Volcanic     |
| `overcast_snow`      | `SkyrimOvercastSnow`  | Raise    | Snow, Reach                                           |
| `ash_storm`          | `DLC2AshStorm`        | Raise    | Solstheim                                             |
| `sunbreak`           | (per-climate variant) | Lower    | region-specific — one row per applicable climate      |

The `sunbreak` row expands out to multiple manifest entries in the final table so each climate can select its
best-looking clear-weather variant (`SkyrimClearMarsh`, `SkyrimClearReach`, etc.). The exact EditorIDs land
in Step 1 after xEdit cross-check.

**Cinematic / scripted weathers are excluded entirely** from the manifest: `HelgenAttackWeather`,
`SkyrimMQ206Weather`, `SkyrimDA02Weather`, Sovngarde and Apocrypha weathers, civil-war variants,
location-scripted fogs (`KarthspireRedoubtFog`, `BloatedMansGrottoFog`, `RiftenOvercastFog`). Forcing any of
these mid-play risks breaking quest state or being cell-scoped. This exclusion is deliberate; it is
**not** a global observation blacklist. `WeatherEventLog` continues to observe scripted weathers when the
vanilla game triggers them naturally.

### `OnStart` — the whole beat

```cpp
void ChangeWeatherBeat::OnStart(const BeatContext& ctx, const nlohmann::json& parameters)
{
    // 1. Parse and sanitize inputs.
    const auto weatherName = LLMTextSanitizer::Sanitize(
        parameters.value("weather_name", std::string{}));
    const auto framingLine = LLMTextSanitizer::Sanitize(
        parameters.value("framing_line", std::string{}));

    // 2. Validate weather name against manifest.
    const auto* entry = WeatherManifest::Find(weatherName);
    if (!entry) { /* log failure, return — cooldown NOT stamped */ }

    // 3. Validate climate mask against current player region.
    const auto climate = Region::ForPlayer().climate;
    if (!WeatherManifest::PermittedFor(*entry, climate, ctx.desiredDirection)) {
        /* log failure, return — cooldown NOT stamped */
    }

    // 4. Resolve the vanilla TESWeather*.
    auto* weather = WeatherManifest::Resolve(weatherName);
    if (!weather) { /* log failure, return */ }

    // 5. Kick off the gradual weather change.
    auto* sky = RE::Sky::GetSingleton();
    if (!sky) { /* log failure, return */ }
    sky->SetWeather(weather, /*a_override=*/false, /*a_accelerate=*/false);

    // 6. Post the framing notification.
    if (!framingLine.empty()) {
        RE::DebugNotification(framingLine.c_str(), /*a_soundToPlay=*/nullptr,
                              /*a_cancelIfAlreadyQueued=*/false);
    }

    // 7. Stamp the per-beat cooldown.
    g_lastFiredGameHours = EngineUtils::GetCurrentGameHours();
}
```

Notes on the flow:

- Every LLM-returned string passes through `LLMTextSanitizer::Sanitize` at the point of extraction, per the
  project's response-handling discipline in `CLAUDE.md`.
- `SetWeather` with `override=false` means the engine's own region-scheduled weather system can still take
  back control naturally when it wants to. That's the intended behaviour: the beat authors a nudge, the
  region system reasserts organically. If we passed `override=true`, we'd own the sky until an explicit
  `ReleaseWeatherOverride` — but we're not tracking ownership, so we don't want that responsibility.
- `accelerate=false` means the engine uses its normal fade duration (a smooth ~30 s transition, not a
  cinematic snap). This matches the `sw` console command's behaviour.
- The per-beat cooldown is a plain `double` in session memory. No cosave record, no ring buffer, no LLM
  round-trip. If the game reloads, the cooldown resets — acceptable at this scope.
- The `desiredDirection` field is populated by the dispatcher from the current phase's target direction;
  the manifest filter uses it to reject e.g. a `Lower`-polarity `clear_sky` when the Director asked for
  `Raise`.

### `IsAvailable` gates

```cpp
bool ChangeWeatherBeat::IsAvailable(const BeatContext& ctx) const
{
    if (ctx.playerInInterior) return false;

    // Per-beat cooldown, session-only.
    const double cooldownHours = Settings::Get().changeWeatherBeatCooldownGameHours;
    if (cooldownHours > 0.0
        && (EngineUtils::GetCurrentGameHours() - g_lastFiredGameHours) < cooldownHours) {
        return false;
    }

    // Region must resolve.
    const auto climate = Region::ForPlayer().climate;
    if (climate == Region::Climate::Unknown) return false;

    // At least one manifest weather must be permitted for this
    // climate + desired direction combination.
    if (WeatherManifest::WeathersPermittedFor(climate, ctx.desiredDirection).empty()) {
        return false;
    }
    return true;
}
```

The `RemainingCooldownGameHours` override is trivial — it returns
`max(0, cooldownHours - (now - g_lastFiredGameHours))` so the dashboard cooldown column populates the same
way the other beats' do.

### Feedback loop with `WeatherEventLog`

Deliberate and intentional: when `ChangeWeatherBeat` fires and the sky begins its fade, `WeatherEventLog`'s
poll (running on the same main thread, 30 s cadence by default) will observe the category change like any
other weather shift. It emits a `weather_event` into its ring buffer with no distinguishing marker; the
LLM sees "the sky is darkening" and doesn't know or care that we authored it. This is the entire point:
downstream beats can react to the authored change without any special-case pathway, and the Director's
prompt context stays uniform.

The one edge case worth naming: if the beat fires and then the beat-cooldown gate lets it fire again 60
seconds later on a different weather, `WeatherEventLog`'s debounce (20 s default) will still emit both
transitions. That's correct — two distinct authored weather changes should read as two events.

### `Abort` semantics

`Abort` for this beat is essentially a no-op:

```cpp
void ChangeWeatherBeat::Abort()
{
    logger::info("ChangeWeatherBeat: Abort() invoked (no-op; SetWeather "
                 "already synchronous, no state to unwind)");
}
```

By the time an abort can be issued via `BeatSystem::AbortRunningBeat`, `OnStart` has already returned. The
weather change is either mid-fade or complete; either way we don't own the override (we passed
`override=false`) and there's no cosave record, no aliases, no spawned refs, no per-beat quest state to
tear down. The `IBeat::Abort` contract is satisfied trivially.

The dashboard's Abort button will still function correctly on this beat: `BeatSystem::AbortRunningBeat`
forces the top-level state back to `NO_BEAT_RUNNING`, which for a beat that would have released the slot
250 ms later anyway is functionally indistinguishable from letting it complete.

---

## Steps

### Step 1 — `WeatherManifest` module

- [ ] Complete

**[CLAUDE]**

**Goal:** Author the manifest data structure, table, and lookup / filter helpers. No runtime callers yet;
build-verified only. Deliberately split from the beat itself so the manifest can be audited independently.

**Files:**

- `include/WeatherManifest.h` (new).
- `src/WeatherManifest.cpp` (new).
- `docs/vanilla/weathers/manifest.csv` (new) — human-authored mirror of the C++ table.
- `CMakeLists.txt` — add the new source file.

**Sub-tasks:**

1. Author the `Polarity` enum, `WeatherEntry` struct, and the three public functions (`Resolve`,
   `WeathersPermittedFor`, `Find`) per the design overview.
2. Author the internal hardcoded table. Confirm every EditorID against the CK / xEdit before shipping —
   do not guess. `SkyrimStormRain` and `SkyrimStormSnow` are the two known-good stormy references from
   Phase 9's investigation; use those as anchor points and cross-check the rest.
3. `Resolve`: lazy `TESForm::LookupByEditorID` on first call per name; cache the `TESWeather*` result.
   Log one warning per missing EditorID and fail open (return `nullptr`).
4. `WeathersPermittedFor(climate, direction)`: walk the table, filter by climate-mask bit AND polarity
   compatibility (`Either` matches both `Raise` and `Lower`), return pointers to matching entries.
5. `Find(name)`: linear scan, return `nullptr` on miss.
6. Mirror the table in `manifest.csv` — columns
   `name,editor_id,polarity,climate_mask,short_description`. The climate mask column lists the enum names
   separated by `|` for readability (e.g. `Tundra|Pine|Marsh|Reach|Rift|Coast|Volcanic`).
7. Run `pwsh -File format.ps1`.

**Specifics:**

- The climate-mask storage in C++ is a `std::uint32_t` bitfield keyed off `Region::Climate` enum values,
  built up in-place in the table via `(1u << static_cast<int>(Climate::Tundra)) | ...`. Consider a small
  constexpr helper (`ClimateBits(Climate...)` variadic) to keep the table readable.
- Manifest size is ~10–15 entries; a linear scan is fine everywhere.
- `LookupByEditorID` requires powerofthree's Tweaks (already a hard dep for `LocationKeywords` and
  `Region`), so no new dependency.
- The `Either`-polarity entries (`SkyrimFog`, `SkyrimOvercast`) always survive the polarity filter — this
  is what makes the beat "always available" in the design sense.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- No runtime callers yet; correctness is validated indirectly via Steps 2–4.

---

### Step 2 — `ChangeWeatherBeat` skeleton + Settings

- [ ] Complete

**[CLAUDE]**

**Goal:** Land the beat class, its `IBeat` overrides (excluding real `OnStart` / `Tick` bodies), the per-
beat cooldown session state, and the INI section. The beat compiles and can be constructed, but
registration and dispatch land in Step 3.

**Files:**

- `include/ChangeWeatherBeat.h` (new).
- `src/ChangeWeatherBeat.cpp` (new).
- `include/Settings.h`, `src/Settings.cpp` — add `[ChangeWeatherEvents]` section.
- `statics/SKSE/Plugins/NarrativeEngine.ini` — add `[ChangeWeatherEvents]` defaults.
- `CMakeLists.txt` — add the new source file.

**Sub-tasks:**

1. Author `ChangeWeatherBeat` per the shape sketched in the design overview. All `IBeat` overrides
   present:
   - `Name` returns `"change_weather"`.
   - `Description` returns the long-form LLM-facing description (drafted in Step 4 — for now, a short
     stub is fine).
   - `Polarity` returns `BeatPolarity::Either`.
   - `IsAvailable` implements the four gates from the design overview.
   - `OnStart` — stub that logs but does nothing yet.
   - `Tick` — final body: `return {BeatState::NOT_RUNNING};`. This is the whole implementation.
   - `RemainingCooldownGameHours` per design overview.
   - `Abort` — the trivial log-only implementation from design overview.
2. Per-beat session state (private namespace inside `ChangeWeatherBeat.cpp`):
   - `std::mutex g_cooldownMutex;`
   - `double g_lastFiredGameHours = 0.0;`
3. Settings additions:
   - `double changeWeatherBeatCooldownGameHours = 2.0;` (`[ChangeWeatherEvents] fChangeWeatherBeatCooldownGameHours`)
   - `bool debugManifestFilter = false;` (`[ChangeWeatherEvents] bDebugManifestFilter`) — when true,
     `IsAvailable` logs the manifest filter's per-tick result at `logger::debug` level.
4. Mirror the defaults in `statics/SKSE/Plugins/NarrativeEngine.ini`. Include a leading comment block per
   the existing section style.
5. Run `pwsh -File format.ps1`.

**Specifics:**

- No cosave record — this beat has zero persistent state. `Plugin.cpp` gets no `OnSave`/`OnLoad`/`OnRevert`
  entries for it.
- The 2-hour default cooldown is a starting guess; user tuning will follow in Step 5's validation.
- `Abort` compiles even though the pure-virtual is declared on `IBeat`; the trivial body satisfies it.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- Beat is constructible but not yet registered; the dashboard's Dispatch tab does not list it yet.

---

### Step 3 — `OnStart` implementation + `BeatRegistry` registration

- [ ] Complete

**[CLAUDE]**

**Goal:** Fill in the real `OnStart` body per the design overview, register the beat in `BeatRegistry` at
`kDataLoaded` so the Director can consider it. After this step the beat is fully functional from the
plugin side; only the LLM prompt authoring in Step 4 is missing to make it fire organically.

**Files:**

- `src/ChangeWeatherBeat.cpp` — replace the `OnStart` stub with the real body.
- `src/Plugin.cpp` — register `ChangeWeatherBeat` in `BeatRegistry` alongside the existing three beats
  (`AmbushBeat`, `NPCLetterBeat`, `NPCVisitBeat`).

**Sub-tasks:**

1. Implement `OnStart` per the design overview's seven-step sketch. Every LLM-returned string routes
   through `LLMTextSanitizer::Sanitize` at the extraction site, per `docs/LLM_RESPONSE_HANDLING.md`.
2. Failure paths (unknown weather name, unresolved `TESWeather*`, climate mismatch, null `Sky` singleton)
   all log at `logger::warn` and return without stamping the cooldown. The Director's LLM will see the
   beat as still available next tick and can pick something else; this is the intended failure mode.
3. Success path (through step 6 of the design sketch) stamps `g_lastFiredGameHours` under the cooldown
   mutex. Log at `logger::info` with the resolved weather EditorID and the current climate for
   traceability.
4. Register the beat in `Plugin.cpp` — one line added to the existing `BeatRegistry::Register` sequence.
   Follow the existing pattern (`std::make_unique<ChangeWeatherBeat>()`).
5. Run `pwsh -File format.ps1`.

**Specifics:**

- `RE::DebugNotification` is a free function in `RE/M/Misc.h` — no bridge required. Signature:
  `DebugNotification(const char* text, const char* sound = nullptr, bool cancelIfAlreadyQueued = true)`.
  Pass `cancelIfAlreadyQueued=false` so consecutive weather beats don't silently drop each other's
  notifications.
- `OnStart` runs on the main thread with the single-beat lock held (see `BeatSystem::StartBeatInternal`
  in `src/BeatSystem.cpp`). All engine reads and writes are safe here without marshaling.
- The `BeatContext` passed to `OnStart` carries `desiredDirection` — use it directly for the climate-mask
  permission check; do not re-derive it from `PhaseTracker`.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- The dashboard's Dispatch tab now lists `change_weather` alongside the other three beats, with its
  enabled toggle default-on and cooldown display populated.
- The dashboard's per-row "Dispatch" button on the `change_weather` row will trigger the full pathway
  once Step 4's prompt authoring lands — until then, force-dispatch will fail at the LLM validation
  step with "unknown beat" because the prompt hasn't been taught about it yet. This is expected between
  Steps 3 and 4.

---

### Step 4 — Beat-select prompt authoring

- [ ] Complete

**[CLAUDE]**

**Goal:** Teach the `narrative_engine_action_select` prompt about `change_weather`. The Director's LLM
must know the beat exists, understand its parameter schema, and be able to see the per-tick candidate
weather list filtered by current climate + desired direction.

**Files:**

- `statics/SKSE/Plugins/SkyrimNet/prompts/narrative_engine_action_select.prompt` — add the beat's
  long-form description and parameter schema.
- `src/BeatSystem.cpp` — extend `BuildBeatSelectPromptContext` to inject a
  `weather_candidates` array per-beat, populated from `WeatherManifest::WeathersPermittedFor` when
  `change_weather` is in the candidate list.
- `include/ChangeWeatherBeat.h`, `src/ChangeWeatherBeat.cpp` — update the `Description` return with the
  full long-form text (the same text the prompt embeds).

**Sub-tasks:**

1. Update `Description` in `ChangeWeatherBeat.cpp` per the pattern `NPCLetterBeat` uses — long-form prose
   describing the beat's purpose, the polarity flexibility, the parameter schema, and a strong statement
   that the LLM must pick `weather_name` from the `weather_candidates` list attached to the beat entry
   in the prompt context.
2. Extend `BuildBeatSelectPromptContext` in `src/BeatSystem.cpp`. Follow the existing pattern
   (`letter_sender_candidates`, `visit_sender_candidates`): populate `weather_candidates` on the beat's
   JSON entry when the beat is present in the candidate list, using
   `WeatherManifest::WeathersPermittedFor(currentClimate, direction)` to build the list. Each entry:
   `{name, short_description, polarity}`.
3. Add candidate collection to `ConsiderBeat` alongside the existing letter / visit collectors. Unlike
   letter and visit, there's no min-candidate gate — the `IsAvailable` gate already ensures at least one
   permitted weather exists.
4. Update the prompt template: add the beat's entry to whatever section describes the available beats,
   using the same pattern letter and visit beats use. The `parameters` schema in the prompt lists
   `weather_name` (REQUIRED, string, must be from `weather_candidates`) and `framing_line`
   (REQUIRED, string, one sentence of narrative framing under ~200 chars).
5. Run `pwsh -File format.ps1`.

**Specifics:**

- The `framing_line` character cap of 200 exists so a runaway LLM can't paste a paragraph into a
  `DebugNotification` (which will truncate mid-glyph in Skyrim's HUD). Enforce the cap in `OnStart`'s
  sanitize step by truncating rather than failing.
- If the LLM returns a `weather_name` outside the `weather_candidates` list for the current tick — even
  a valid manifest entry that just isn't permitted here — the beat's `OnStart` validation catches it
  and fails cleanly. This is defense in depth; the prompt should still be clear about the constraint.
- Save the `narrative_engine_action_select.prompt` change and rebuild — `statics/` edits only reach the
  runtime mod folder via `build.ps1` (per `feedback_statics_require_build.md`).

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- With `bDebugMode=true` in the INI, force-dispatch the beat from the dashboard's Dispatch tab. The LLM
  should return a valid `weather_name` + `framing_line`; the log should show the resolved EditorID and
  the notification should appear in the top-left corner of the HUD.
- The sky should visibly begin fading toward the new weather within a few seconds; full transition
  completes over ~30 s per vanilla engine behaviour.

---

### Step 5 — In-game validation

- [ ] Complete

**[USER]**

**Goal:** Confirm the beat fires organically (not just via force-dispatch), behaves correctly across the
climate spectrum, and that the feedback loop with `WeatherEventLog` produces the expected downstream
observation event.

**Test steps** (with a running game, `bDebugMode=true`, `bEventHistoryEnabled=true`):

1. Load a save in an exterior cell in Whiterun Hold (Tundra climate). Confirm the dashboard's Dispatch
   tab shows `change_weather` as enabled and with `remaining_cooldown_hours=0.0`.
2. Force-dispatch `change_weather` from the dashboard. Verify:
   - The notification text appears in the top-left HUD corner.
   - The sky begins fading toward the chosen weather within ~5 seconds.
   - Within ~1 minute, a `weather_event` entry appears in the dashboard's Recent Events tab reflecting
     the change (from `WeatherEventLog`'s next poll).
   - The event history log file at `<SKSE logs>/NarrativeEngine_EventHistory.log` contains both a
     `beat_dispatched change_weather` entry and the subsequent `weather_event` entry.
   - The Dispatch tab's `remaining_cooldown_hours` column now shows ~2.0 h counting down.
3. Wait for the per-beat cooldown to expire (advance time via `t` console command if needed). Confirm
   `change_weather` becomes available again.
4. Fast-travel to Winterhold (Snow climate). Force-dispatch and verify the LLM picks from the
   Snow-appropriate manifest subset (`storm_snow`, `overcast_snow`, `clear_sky`, ambient fog / cloudy /
   overcast / sunbreak-snow variant). Confirm no `storm_rain` / `overcast_rain` / `ash_storm` entries
   appear in the candidate list logged to the debug log.
5. Fast-travel to Solstheim (Solstheim climate). Force-dispatch and verify `ash_storm` appears in the
   candidate list and can be selected. Cross-check other climates' storms are excluded.
6. Enter an interior (e.g. Whiterun's Bannered Mare). Confirm the dashboard's Dispatch tab shows
   `change_weather` as unavailable (`IsAvailable` gate on `playerInInterior`). Exit and confirm
   availability returns.
7. Trigger the abort pathway: force-dispatch `change_weather`, then immediately click the dashboard's
   Abort button. Confirm the log records the no-op abort and the top-level state releases cleanly.
   The weather change already-in-flight is unaffected.
8. Let the game run for 30+ real minutes with normal play. Confirm `change_weather` fires organically
   (not force-dispatched) at least once, that the framing line reads sensibly for the tension direction
   the phase is in, and that the subsequent `WeatherEventLog` entry uses the same category the beat
   authored.

**Success criteria:**

- Beat fires cleanly on force-dispatch AND organically at least once during passive play.
- Climate filter correctly excludes region-inappropriate weathers.
- Interior gate blocks correctly.
- Per-beat cooldown behaves as configured.
- `WeatherEventLog` observes the authored change and it appears in the merged tail.
- No crashes, no stuck top-level state, no LetterPool / VisitState / AmbushBeat regressions.

If any climate's candidate list feels too thin (fewer than 3 entries after filter), adjust the manifest
in Step 1 and rerun this step. If the per-beat cooldown default (2 h) turns out to be too short or too
long in practice, tune `fChangeWeatherBeatCooldownGameHours` and note the final value here.

---

## Post-implementation additions

_This section is populated after implementation completes, mirroring Phase 09's practice. Any modules,
settings, or design shifts that landed beyond the numbered plan get retrospectively recorded here so
future readers can see the plan/reality gap at a glance._
