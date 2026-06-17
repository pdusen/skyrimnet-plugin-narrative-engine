# IntelEngine — Patterns and Lessons

Patterns IntelEngine used that **may be worth considering** if NarrativeEngine ends up facing the same problem, plus
a handful of "this is a real problem in this space" lessons. The right way to read this file:

- **Open it when** you've already decided NarrativeEngine needs to solve a specific problem (an async LLM call, a
  fuzzy NPC-name lookup, a recovery cascade for stuck NPCs, …) and you want to see one author's working answer.
- **Adopt** the pattern if the problem matches and the trade-offs fit our context.
- **Adapt** if it almost fits.
- **Ignore** if our problem is a different shape.
- **Don't** pull a pattern in just because it's listed here. None of these is a default. Several solve problems
  NarrativeEngine may never have (faction-political simulation, in-game React dashboard with two-way JS interop,
  multi-NPC battle spawning) — those patterns are listed for completeness, not as something to reach for.

A pattern showing up below is evidence it solved a real problem for one similar project; it isn't evidence that
NarrativeEngine should adopt it.

And even when a pattern *does* apply to a problem we have, ask whether a better design exists before adopting —
along the axes documented in
[CLAUDE.md](../../CLAUDE.md#even-when-intelengine-fits-ask-if-theres-a-better-design): simplicity, efficiency,
encapsulation (especially the C++ / Papyrus boundary cost), maintainability. IntelEngine's scope was much
broader than NarrativeEngine's, and several of its patterns are sized to that broader scope; lighter alternatives
may serve us better.

Finally: **don't trust IntelEngine's specifics by default.** It was abandoned mid-development with subtle quirks
and unresolved edge cases — magic numbers, threshold tunings, condition forms, and field names should all be
verified against authoritative sources (CK Wiki, CommonLibSSE-NG, in-game testing) before being adopted into
NarrativeEngine. See the full treatment in
[CLAUDE.md](../../CLAUDE.md#treat-intelengines-code-with-skepticism-not-deference).

---

## Threading & async

### Three-phase async pattern

Used by every DM tick (Story DM, NPC Social DM, Political DM) to keep SQL and context-building off the game thread.

- **Phase A (main thread)** — Snapshot all engine state into plain value-only structs. No pointers to game objects
  survive into the worker thread. *(Why: the game thread is the only one allowed to touch most engine APIs.)*
- **Phase B (worker thread)** — SQL queries, markdown formatting, prompt context assembly. Pure data work.
- **Phase C (main thread)** — Marshal the built result back to Papyrus via
  `DispatchMethodCall("OnXxxContextReady", ...)`. Papyrus then sends the prompt to the LLM via SkyrimNet.

IntelEngine's implementation: `AsyncDispatch.cpp` runs one dedicated worker thread; callbacks marshal to the main
thread via SKSE's `TaskInterface`.

**Lesson:** if a feature needs SQL, file IO, network IO, or expensive formatting, this is the template. Anything other
than "snapshot + work + dispatch" risks touching engine APIs off-thread and crashing.

### Marshaling callbacks to the main thread

Worker threads must never call Papyrus or engine APIs directly. Pattern: pass a completion lambda; the lambda enqueues
`SKSE::GetTaskInterface()->AddTask(...)` which runs on the main thread.

### Polling rates: 150ms vs Papyrus

IntelEngine replaced Papyrus's 3-second `OnUpdate` arrival poll with a 150ms C++ worker (`ProximityMonitor`). ~20×
faster arrival detection, no Papyrus VM cost. Worth reaching for whenever Papyrus's tick granularity is the
bottleneck — but be deliberate about how many such workers you spin up.

---

## State ownership

### Single source of truth, owned by C++

IntelEngine made the C++ `SlotTracker` the authoritative store for task state, **not** Papyrus's StorageUtil. Papyrus
arrays are a *view* synced from C++ on load (`SyncArraysFromSlotTracker()`).

Why: StorageUtil has measurable deserialization latency on load and is fragile across script-property changes. SKSE
co-save serialization is faster and more controllable. Papyrus still reads from arrays for fast access during gameplay;
the arrays just aren't where the truth lives.

Same pattern applied to: `BuildBusyReason()` is the single source for SkyrimNet's busy-status string; `PoliticalDB` is
authoritative over any in-memory caches.

**Lesson:** decide for each piece of state which layer owns it, and make every other layer a read-through view. Don't
let two layers both think they're authoritative.

### StorageUtil as the bridge to bio templates

But Papyrus's StorageUtil **is** the only thing SkyrimNet's submodule prompts can read (via `papyrus_util(...)`). So
even C++-authoritative state has to push a mirror into per-actor StorageUtil keys for the LLM to see it. IntelEngine's
`Intel_TaskHistory`, `Intel_TaskType`, etc. keys exist precisely for this.

**Implication for NarrativeEngine:** if it owns state in C++, design a StorageUtil mirror namespace early. The bio
templates can only read from there.

---

## Save safety

### Per-save SQLite isolation + save-scum recovery

IntelEngine creates one SQLite DB per save (`IntelEngine-{saveID}.db`, where `saveID = timestamp_randomSuffix`). The
`saveID` is generated at `kNewGame`, stored in the save, and recovered at `kPostLoadGame`.

When the player loads an earlier save:

1. `CleanupFutureEvents()` deletes any DB rows with timestamps beyond the loaded save's game time.
2. `RecalculatePlayerStandings()` replays remaining history chronologically.

This lets save-scumming work without polluting political history with events from an abandoned timeline. Each save has
a coherent past.

**Lesson:** if any feature persists outside the Skyrim save (SQLite, JSON, etc.), plan for timeline divergence on load.
Either bind storage to the save ID, or filter on game time.

### Multiple persistence layers, each used for what it's good at

| Layer                        | What it's good for                                              |
| ---------------------------- | --------------------------------------------------------------- |
| **SKSE co-save**             | Small, fast-deserializing canonical C++ state (SlotTracker)     |
| **SQLite (per-save)**        | Large/queryable data with history (politics)                    |
| **PapyrusUtil StorageUtil**  | Per-actor key-value state readable by SkyrimNet bio templates   |
| **Papyrus quest properties** | State that wants to ride the save automatically                 |
| **GlobalVariables**          | MCM-tuned values that condition functions need                  |
| **YAML configs on disk**     | User-editable plugin configuration (factions, settings)         |
| **JSON state files on disk** | Out-of-band data needing immediate availability across launches |

Each is paired with the matching lifecycle handler. IntelEngine's `Plugin.cpp` lifecycle table is the reference
(kPreLoadGame resets singletons, LoadCallback deserializes co-save, kPostLoadGame initializes DB + dispatches
`Maintenance()` to Papyrus).

### Script property fixup on load

IntelEngine patches stale Papyrus script cross-references on load via direct Papyrus VM property manipulation
(`FixupScriptProperties()`). Solves the "Quest A holds a reference to Quest B's old script object" problem that bites
mods after script refactors.

**Lesson:** if scripts have cross-references and the plugin is going to evolve, build the fixup harness up front.
Retrofitting after a refactor is painful.

---

## Search and resolution

### Fuzzy search cascade

NPCIndex, ItemIndex, and LocationResolver all use the same fallback cascade:

1. Exact case-insensitive match
2. Article-stripped match (`"the Bannered Mare"` → `"Bannered Mare"`)
3. Token-overlap scoring (word-by-word)
4. Levenshtein distance (character-level typo tolerance)

Cheap and effective for "the LLM gave us a name to find." Don't reach for embeddings or anything heavier without first
trying this.

### Pre-validation as anti-hallucination

`ActionValidator` runs before dispatch and returns specific failure codes (`"no_destination"`, `"npc_not_found"`,
`"npc_dead"`, `"npc_is_self"`). These codes feed into the `intel_get_failure_reason` decorator so the NPC can voice why
they can't do what was asked.

This is the moat against "the LLM picked an action whose parameters don't make sense." Always validate before allocating
a slot.

### Dynamic indexing at load, not hardcoded lists

IntelEngine builds its NPC, item, and location indexes by iterating all four process list tiers + all cells at
`kDataLoaded`. No hardcoded names — automatically picks up mod-added content.

Cost: a multi-second initial scan. Worth it.

---

## LLM-facing decision making

### Dispatch history ring buffer

IntelEngine keeps the last 6 dispatches (type, NPC, narration, outcome: dispatched/rejected) and injects them into the
DM prompt. The LLM sees what it just decided, what got rejected, and can avoid repetition or retry loops.

**Lesson:** any time an LLM makes a sequence of similar decisions, give it the recent history. Cheap, dramatically
improves variety.

### Candidate pool isolation

Each DM tick rebuilds its candidate pool from scratch and resolves names against the pool snapshot from prompt-send
time, not against the pool at response-arrival time. Prevents a save-load mid-tick from dispatching to the wrong NPC.

### Schedule safety net

Even after extensive prompt engineering, the LLM sometimes agrees to a meeting in dialogue without firing the schedule
action. IntelEngine has a separate `intel_schedule_safety_net.prompt` that runs after dialogue and detects commitments
that weren't scheduled.

**Lesson:** for high-importance actions, design a post-hoc safety net. Don't rely on the LLM to be perfectly disciplined
about firing the right tool.

---

## Soft dependencies

### Soft-link external DLLs via GetProcAddress

Both SkyrimNet and PrismaUI are loaded with `LoadLibrary` + `GetProcAddress` rather than linked at build time. If
absent, function pointers stay null; calling code checks before invocation.

**Result:** PrismaUI is optional with no code branches duplicated. SkyrimNet absence makes the plugin do nothing — but
doesn't crash.

**Lesson:** for any optional integration, use this pattern. Hard linkage forces users to install dependencies they don't
want.

---

## Recovery & robustness

### Escalating recovery for stuck NPCs

Three-level escalation in `StuckDetector`:

1. **Soft recovery** — re-evaluate AI packages (give the engine another chance)
2. **Waypoint navigation** — try an intermediate destination
3. **Progressive teleport** — closer to target each escalation: 2000 → 1000 → 500 → 250 units

Plus a final force-complete timeout to prevent permanent soft-locks in unloaded cells.

**Lesson:** Skyrim's pathfinding *will* fail. Have a graceful escalation path that ends in force-completion before
forever.

### Departure verification

Before treating a travel task as "in flight," `DepartureDetector` verifies the NPC actually moved from their starting
position. If they're trapped (locked door, blocked path), escalates before deadline tracking kicks in.

**Lesson:** "told them to leave" ≠ "they left." Verify.

### Off-screen tracking for unloaded cells

NPCs in unloaded cells can't have position polled. `OffScreenTracker` uses a two-phase approach: pre-arrival, advance
baseline by game time; post-arrival, check if they've stopped.

**Lesson:** cross-cell systems need a separate observation model from same-cell systems.

---

## Anti-patterns IntelEngine deliberately avoids

These show up in IntelEngine's design notes (`docs/ANTI_HALLUCINATION.md` in NativePlugin) and are worth internalizing:

- **Static location lists / hardcoded names.** Won't survive load-order changes. Index at runtime.
- **Trusting the LLM's parameters without validation.** It will name dead NPCs, nonexistent cells, itself. Always
  validate.
- **Letting StorageUtil be authoritative.** Save-load latency, fragility.
- **Polling everything in Papyrus at 3s.** Too slow; spawn C++ workers for fast paths.
- **Hard-linking optional DLLs.** Wrecks compatibility. Use soft links.
- **No dispatch history.** The LLM repeats itself, gets stuck in loops.

---

## Open issues IntelEngine acknowledges

Useful as known-difficult areas to design around or avoid:

- **Upstairs/downstairs Z-axis door scanning** is unreliable for interiors that don't follow the convention (named
  locations work; semantic vertical doesn't always).
- **Action conflicts between SkyrimNet plugins** — the LLM sees all actions from all installed plugins, so two plugins
  providing `travel` will produce mixed-mod behavior. Mitigation = `enabled: false` per action YAML; no architectural
  fix.
- **Locked doors blocking fetches at night** — common stuck case; mitigated by the stuck-recovery escalation path + an
  explicit narrate-failure fallback.
