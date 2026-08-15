#pragma once

#include <array>

#include <cstdint>
#include <optional>
#include <string>

// Author-tunable plugin configuration.
//
// Read once at SKSE's kDataLoaded message via Load(). Sources, in order:
//   1. Data/SKSE/Plugins/NarrativeEngine.ini (the plugin INI — author
//      defaults for every setting; the file a modder editing the mod's
//      source tree hand-tunes).
//   2. Data/MCM/Settings/NarrativeEngine.ini (MCM Helper-managed;
//      universal override layer — any Config field named here overrides
//      the plugin-INI value on the same key).
//
// Both files are parsed through a single shared per-key enumeration
// (see ReadIniInto in Settings.cpp), so **every** Config field is
// MCM-overridable — including the fifty-plus knobs no UI surface
// exposes. Absent keys fall through: MCM INI absent → plugin-INI value
// stands; plugin INI absent → the baked-in default in Config below
// stands. The plugin is fully functional with no INI at all.
//
// The MCM override INI is re-read at runtime whenever the player
// changes a value in the MCM page: _ne_MCM.psc fires the
// "_ne_DashboardHotkeyChanged" ModEvent (via SKI_ConfigBase's
// OnSettingChange), and MCMEventSink calls ApplyMcmOverride() which
// re-runs the same shared enumeration against the MCM INI.
//
// The plugin also writes to the MCM INI directly (via
// WriteMcmOverride) from dashboard-authored setting changes. The write
// surface is deliberately narrower than the read surface — the reader
// honors every Config field, but the writer only knows how to author
// the specific keys the dashboard UI edits.
namespace NarrativeEngine::Settings
{
    // Number of Attacker0N reference aliases authored on _ne_AmbushQuest
    // (000831). This is a hard ceiling on how many attackers an ambush
    // can field: every spawned reference is held in an alias, so slot N
    // simply doesn't exist beyond this. ReadIniInto clamps
    // ambushMaxAttackerCount to it, and AmbushBeat indexes slots against
    // it, so the ESP and the code can't drift apart silently. Raising it
    // means authoring more aliases in the quest YAML first.
    inline constexpr int kAmbushAttackerSlotCount = 8;

    // Bitmask values for the dashboard hotkey's modifier keys. Bit
    // assignment matches SkyUI convention so the value _ne_MCM.psc packs
    // into the "_ne_DashboardHotkeyChanged" ModEvent can be consumed
    // directly without a remap.
    // Combinable: (kModShift | kModCtrl | kModAlt) == 7.
    inline constexpr std::uint8_t kModShift = 1;
    inline constexpr std::uint8_t kModCtrl = 2;
    inline constexpr std::uint8_t kModAlt = 4;

    struct Config
    {
        // [General]
        bool debugMode = false;
        // Enables per-tick / per-actor trace-level chatter that would
        // drown the log if surfaced by debugMode alone (VisitConclusion-
        // Poll's silence-accumulator tick, AliasWalkFilter's alias-by-
        // alias walk output, HoldGrid's per-worldspace pruning report).
        // Independent of debugMode — a run can have debug on and trace
        // off, or vice versa. Logged via logger::trace, which spdlog
        // is configured to pass through (see logger.h).
        bool traceMode = false;

        // [Director]
        // TEMP: 30 for development iteration; ship default is 90.
        int tickIntervalSeconds = 30;             // wall-clock seconds between evaluations
        int decisionLogMaxEntries = 200;          // ring buffer cap
        int decisionLogTailSizeForPrompt = 10;    // entries fed into BuildPromptContext
        int skyrimNetEventTailSizeForPrompt = 40; // maxCount passed to PublicGetRecentEvents

        // Runtime killswitch initial state. Seeds Tick::g_enabled at
        // Tick::Start. Runtime changes via the dashboard update this
        // field in place *and* write to the MCM INI (so a subsequent
        // boot's ReadIniInto re-seeds correctly). The authoritative
        // runtime store is Tick::g_enabled (atomic); this field only
        // ever supplies the initial value.
        bool tickEnabled = true;

        // Minimum dwell in the current phase (unpaused real-time
        // seconds) before PhaseTracker::EvaluateAdvance is allowed to
        // transition to the next phase. Applies uniformly to every
        // phase — a single floor, not per-phase. Distinct from the
        // per-phase idealDuration values, which gate whether the beat
        // system may fire an event to nudge the story forward. The
        // ideal durations are "when it's a good time for something to
        // happen"; this is "the earliest we'll let a phase change fire
        // at all". Default is deliberately low so tension-driven
        // advancement stays responsive; players who want a floor of
        // real narrative dwell before any phase can hand off can raise
        // it via the Settings tab.
        int minPhaseDurationSeconds = 30;

        // Per-current-phase tension thresholds that drive Freytag advancement.
        // The LLM returns only a tension score; the system decides advancement
        // by comparing that score against the threshold for the *current*
        // phase. Each threshold is either a "rises above" or "drops below"
        // gate depending on the dramatic shape of the transition out of that
        // phase — Exposition/RisingAction/Resolution rise into their successor,
        // Climax/FallingAction wind down into theirs. All values are
        // 0..100 to match the tension-score domain.
        int advanceThresholdExposition = 45;    // -> RisingAction when score >=
        int advanceThresholdRisingAction = 80;  // -> Climax when score >=
        int advanceThresholdClimax = 60;        // -> FallingAction when score <=
        int advanceThresholdFallingAction = 30; // -> Resolution when score <=
        int advanceThresholdResolution = 25;    // -> Exposition when score >=

        // Per-phase ideal durations in unpaused real-time seconds.
        // BeatSystem::ConsiderBeat gates on these — beats may only fire
        // after the current phase has overstayed its ideal duration.
        // Total ideal cycle at defaults: 1200s (20 min); proportions
        // follow the design narrative — Exposition and Resolution sit
        // longer; Climax is brief.
        int idealDurationExposition = 330;    // 5.5 min
        int idealDurationRisingAction = 225;  // 3.75 min
        int idealDurationClimax = 90;         // 1.5 min
        int idealDurationFallingAction = 225; // 3.75 min
        int idealDurationResolution = 330;    // 5.5 min

        // [BeatSystem]
        // Master poll cadence for the Narrative Beat System's worker
        // thread. See PHASE_06_BEAT_SYSTEM_REFACTOR.md.
        int beatSystemPollIntervalMs = 250;
        // Beat dispatch knobs.
        int beatCooldownSeconds = 120;         // wall-clock seconds after beat COMPLETION before next may fire
        int beatRepetitionWindowSeconds = 300; // window during which the same beat name is excluded from picks

        // NPCLetterBeat precondition: minimum number of recently-engaged
        // NPCs SkyrimNet must report before the beat becomes available.
        // Below this, the letter would either fail or fall back to a
        // generated persona; we'd rather skip the beat than ship that.
        int letterMinSenderCandidates = 3;

        // [AlphaCanon]
        // Comma-separated list of cell EditorIDs to treat as do-not-disturb.
        // Empty by default. Whitespace around commas is allowed.
        std::string doNotDisturbCellEDIDsCSV;

        // Comma-separated list of Location EditorIDs the Director will
        // not fire beats in. Matching is inclusive of descendants: the
        // player's current Location and every ancestor reached via
        // BGSLocation::parentLoc is tested, so naming a parent covers
        // its whole subtree. Whitespace around commas is allowed.
        std::string blacklistedLocationEDIDsCSV = "SovngardeLocation";

        // [Dashboard]
        int dashboardHotkeyDXSC = 65;              // DirectX scan code; 65 == DIK_F7; -1 disables
        std::uint8_t dashboardHotkeyModifiers = 0; // kModShift|kModCtrl|kModAlt bitmask; 0 = none

        // [CombatEvents]
        int combatEventsHitRadiusUnits = 6000; // ~90 ft; distance gate for hit / collapse capture
        int combatEventsMaxStored = 256;       // ring-buffer cap on retained internal combat events
        // Semicolon-separated list of spell / magic-item names whose
        // TESHitEvent captures are discarded on receipt. Matches the
        // source form's Full Name (case-insensitive; leading/trailing
        // whitespace on each entry is trimmed). Motivating case:
        // Requiem's "Aura of Courage" (edid REQ_Cloak_GuardOfficer)
        // is a cloak buff that hops between allied guards; the engine
        // fires TESHitEvent for each hop, and our capture reads it as
        // "Longstig attacks Brurid" because cause is the officer and
        // target is the ally. The parsed set lives in Settings.cpp;
        // membership is queried via Settings::IsSpellNameBlocked.
        std::string spellNameBlocklist;

        // [WeatherEvents]
        // Ring-buffer cap on retained internal weather events.
        int weatherEventsMaxStored = 128;
        // Minimum real seconds between weather-state samples. Weather
        // updates on the order of tens of seconds; sampling every Tick
        // (500ms) would waste work without producing new signal.
        int weatherEventPollIntervalSeconds = 30;
        // Minimum real seconds between emitted weather events, applied
        // on top of the poll interval. Debounces pathological rapid
        // category flips inside a single sampling window.
        int weatherEventsDebounceSeconds = 20;

        // [HoldGrid]
        // Debug: after HoldGrid::Initialize builds the partition,
        // dump one 24-bit BMP per worldspace to the SKSE log
        // directory visualizing the fill. Each hold gets a distinct
        // color, seeded cells are black, unassigned cells are white.
        // Overwrites existing files each session. Disabled by default
        // — flip to true only when auditing the fill quality.
        bool holdGridDebugBitmap = false;
        // Between the seed collection pass and the BFS fill, drop
        // small seed clusters that are geographically far from the
        // rest of their hold's seeds. Purpose: filter out cells whose
        // vanilla BGSLocation chain resolves to a hold they clearly
        // aren't part of — mis-authored strays that would otherwise
        // seed a bad fill front that propagates to the map edge.
        //
        //   iHoldGridPruneMaxClusterSize    — K. Only same-hold seed
        //                                     clusters of size <= K
        //                                     are candidates for
        //                                     pruning. Cluster is
        //                                     computed via 4-neighbor
        //                                     adjacency on the seed
        //                                     set. Default 3 catches
        //                                     vanilla strays; setting
        //                                     to 0 disables the pass.
        //   iHoldGridPruneIsolationRadius   — R. A candidate cluster
        //                                     is kept if any same-hold
        //                                     seed OUTSIDE the cluster
        //                                     lies within Manhattan
        //                                     distance R of any cell
        //                                     in the cluster. Dropped
        //                                     otherwise. Default 5
        //                                     — empirically tuned
        //                                     against vanilla Skyrim
        //                                     to catch mis-authored
        //                                     strays without harming
        //                                     legitimate outposts.
        //                                     Setting to 0 disables
        //                                     the pass.
        int holdGridPruneMaxClusterSize = 3;
        int holdGridPruneIsolationRadius = 5;

        // [TravelGraph]
        // EXPERIMENTAL. Builds a road graph at kDataLoaded from the NAVI
        // record's precomputed preferred-path chains — the same data the
        // engine uses to move actors travelling outside the loaded cell
        // grid. Nothing consumes the graph yet; this is a diagnostic to
        // establish whether that data is an accurate road network.
        bool travelGraphEnabled = false;
        // Debug: after the graph builds, dump one 24-bit BMP per
        // worldspace to the SKSE log directory. Nodes are black, edges
        // gray, empty space white. Worldspaces with no nodes are
        // skipped. Overwrites existing files each session.
        bool travelGraphDebugBitmap = true;
        // World units per bitmap pixel. Lower = larger, more detailed
        // image. 256 puts a 4096-unit cell at 16 pixels and renders
        // Tamriel at roughly 1330x820. Automatically raised if it would
        // produce an image over 4096px on a side.
        int travelGraphBitmapUnitsPerPixel = 256;
        // Diagnostic: log runner-up offsets from the BSNavmeshInfo
        // layout calibration. Calibration itself always runs — the graph
        // needs it, since only ~15% of navmeshes have a resident NavMesh
        // form and the rest can only be positioned by reading the info
        // struct. This just controls how much detail it reports.
        bool travelGraphLogCalibration = false;

        // [FineRoads]
        // EXPERIMENTAL. High-resolution road graph for the loaded cell
        // grid, extracted from kPreferred-flagged navmesh triangles.
        // Complements TravelGraph, which only carries the long-distance
        // skeleton and has no spurs. Nothing consumes it yet.
        bool fineRoadsEnabled = false;
        // Rescans are normally event-driven: cell-loaded, load-game, and
        // fast-travel-end sinks flag the graph and the next tick picks it
        // up. This is only a backstop, in unpaused seconds. It exists
        // because cell UNLOADS have no event of their own — an unload
        // unaccompanied by any load would otherwise leave stale cells in
        // the active set indefinitely.
        //
        // It may well be dead weight: the grid shifts as a unit, so an
        // unload almost always arrives with a load whose event triggers
        // the rescan anyway. Kept because the failure it guards against
        // is silent (a stale active set serves points in cells that are
        // no longer loaded) and the cost of keeping it is one grid scan
        // every few minutes. If the log never shows a backstop-triggered
        // rescan finding a changed set, it can be dropped outright.
        int fineRoadsBackstopSeconds = 180;
        // Debug: dump the active local graph to a BMP in the SKSE log
        // directory whenever it changes. Road nodes red, frontier nodes
        // (where the road leaves loaded cells) blue, edges gray.
        bool fineRoadsDebugBitmap = true;
        // World units per pixel for that bitmap. Much finer than
        // TravelGraph's, since this covers ~5x5 cells rather than a
        // whole worldspace.
        int fineRoadsBitmapUnitsPerPixel = 16;

        // [EventHistory]
        // Testing aid: writes every emitted internal event plus
        // SkyrimNet's own event stream to a rotating session-scoped
        // file at Data/../SKSE/NarrativeEngine_EventHistory.log,
        // each line prefixed with an absolute in-game timestamp.
        // Not the same as the ring-buffered tail the LLM sees. Session
        // rotation keeps the previous 5 files.
        //
        // Disabled by default. When disabled, the file is never
        // opened AND each event log skips its per-emit push to the
        // pending queue (so the queue can't grow unbounded on a
        // long-running session with the writer off). Flip to true
        // for Step 9 testing and back to false after.
        bool eventHistoryEnabled = false;
        // Flush cadence in unpaused real seconds (Tick-driven
        // accumulator). Weather / travel events emit slowly; 5s is a
        // comfortable trade between file-write frequency and how long
        // events sit in the pending queues before landing on disk.
        int eventHistoryFlushIntervalSeconds = 5;

        // [TravelEvents]
        // Ring-buffer cap on retained internal travel events.
        int travelEventsMaxStored = 128;
        // Maximum inter-event gap (in unpaused real seconds) within a
        // condensable run. Consecutive travel events whose localTime
        // gap is <= this window collapse together in GetRenderedTail:
        // net-zero runs drop or fold to a "visited X" summary; non-zero
        // runs fold to a "travelled from A to B" summary.
        int travelCondensationWindowSeconds = 60;
        // Follower inclusion distance (engine units). Any alive
        // PlayerTeammate within this radius of the player at emission
        // time gets baked into the event's partyNames list. ~4000
        // units ≈ 60 ft — visually plausible "with you" range.
        int travelFollowerRadiusUnits = 4000;

        // [Beats]
        // Per-beat enable flags, seeded into BeatRegistry at Register
        // time and toggled at runtime from the dashboard's Dispatch tab.
        // Toggles persist: they route through
        // WriteBeatEnabledOverride, which authors the MCM INI and
        // updates the field here.
        bool enableNpcLetter = true;

        // NPCLetterBeat / LetterPool content + dispatch knobs. See
        // PHASE_04_LETTER_POOL_AND_NPC_LETTER_ACTION.md.
        int letterContentMinWords = 60;  // lower bound on LLM body length
        int letterContentMaxWords = 180; // upper bound on LLM body length
        // Minimum `importance_score` (0.0–1.0) a SkyrimNet memory must
        // have to be included in the sender's memory tail passed to
        // the LLM. Filters out low-signal chatter so both the
        // beat-select "who should send this?" pick and the compose
        // "what should they say?" call see only memories that carried
        // real weight when they happened.
        float letterMemoryImportanceThreshold = 0.4f;
        // Compose-prompt content caps — bound how much sender context
        // renders into narrative_engine_letter_compose.prompt. Users
        // running local LLMs against a tight context window can dial
        // these down to shrink the rendered prompt. The candidate-pool
        // memory tail (which drives the beat-select prompt, not the
        // compose prompt) uses its own fixed cap and is not affected.
        int letterComposeMemoryRenderCap = 6;    // max sender.memories entries
        int letterComposeDialogueRenderCap = 25; // max sender.recent_dialogue entries

        // Action-select prompt content caps — bound how much rendered
        // context lands in narrative_engine_action_select.prompt (the
        // beat-select prompt that picks which action fires). Same
        // motivation as the compose caps above: shrink the rendered
        // prompt for tight-context local LLMs.
        int actionSelectEventRenderCap = 10;       // max recent_events entries
        int actionSelectLetterMemoryRenderCap = 6; // memories per letter sender candidate
        int actionSelectVisitMemoryRenderCap = 6;  // memories per visit sender candidate

        int letterPoolSize = 20;                       // informational; ESP defines the actual 20 forms
        int letterDispatchVerifyDelaySeconds = 5;      // grace window before RUNNING gives up on the courier handoff
        int letterPendingDeliveryTimeoutSeconds = 600; // load-time demotion gate for stuck PendingDelivery slots

        // Per-beat cooldown in *in-game hours* applied after the letter
        // successfully reaches the vanilla courier container.
        // Independent of the global iBeatCooldownSeconds real-time
        // cooldown, which still applies on top. 0 disables. Persists
        // via the beat's own co-save record.
        int letterBeatCooldownGameHours = 24;

        // Per-sender cooldown in *in-game hours* applied after the
        // vanilla courier hands the letter to the player (delivery
        // event). Prevents the same sender from being picked as a
        // candidate again for this many in-game hours, avoiding the
        // "three letters from Ancano in one session" pathology. 0
        // disables the filter. Persists per-sender-FormID in the
        // beat's co-save record.
        int letterSenderCooldownGameHours = 72;

        // [LetterPool]
        // 0 = silent, 1 = log evictions, 2 = log every state transition.
        int letterPoolEvictionLogVerbosity = 1;

        // --- NPCVisitBeat ---

        // [Director]
        // NPCVisitBeat precondition: minimum number of viable sender
        // candidates required for IsAvailable to return true. Below
        // this the beat declines and BeatSystem considers other picks.
        int visitMinSenderCandidates = 3;

        // [Actions] — dispatch / composition
        int visitBriefingMinWords = 40;
        int visitBriefingMaxWords = 120;
        int visitMarkerMinDistanceUnits = 800;  // closest spawn marker may be
        int visitMarkerMaxDistanceUnits = 2500; // farthest spawn marker may be

        // Compose-prompt content caps for narrative_engine_visit_compose.prompt.
        // Same shrink-for-local-LLMs motivation as the letter-compose
        // caps above. Only affects the visit compose prompt; the
        // action-select visit candidate memory tail has its own knob.
        int visitComposeMemoryRenderCap = 6;    // max sender_memories entries
        int visitComposeDialogueRenderCap = 25; // max sender_recent_dialogue entries

        // Cap on the `recent_lines` block in
        // narrative_engine_visit_conclusion_poll.prompt — how many
        // most-recent sender/player exchanges are shown to the LLM
        // when it judges whether the ongoing visit has concluded.
        // The oldest-newest slice is taken from the tail of the
        // filtered dialogue history, so lowering this narrows the
        // window without losing chronology.
        int visitConclusionPollRecentLinesRenderCap = 8;

        // Per-sender cooldown in *in-game hours* applied once a visit's
        // Salutation → Discuss transition fires (i.e., the sender actually
        // showed up and spoke to the player). Prevents the same NPC from
        // being picked as a visit sender again for this many in-game
        // hours, avoiding the "Ancano visits three times in one session"
        // pathology. 0 disables the filter. Persists per-sender-FormID
        // in the action's own co-save record.
        int visitSenderCooldownGameHours = 72;

        // [Actions] — state machine timing
        // Salutation timeout: seconds after Start before rollback if the sender
        // hasn't closed distance to speak the opening line.
        int visitApproachTimeoutSeconds = 60;
        // Distance at which the Salutation opening line fires and the machine
        // advances to Discuss. Kept generous (~900u) so the LLM + TTS pipeline
        // has time to generate the opening line while the sender is still
        // closing the last stretch — otherwise the sender arrives at the
        // player and stands there silently while the response streams in.
        int visitSalutationApproachDistanceUnits = 1000;
        // Distance for the ReEngage resumption line to fire. Defaults to
        // the same value as Salutation but tunable independently.
        int visitReEngageApproachDistanceUnits = 1000;
        // Wall-clock cadence (seconds) at which C++ evaluates the three
        // cheap-signal gates that decide whether to fire the natural-conclusion
        // LLM poll during Discuss.
        int visitPollGateTickSeconds = 1;
        // Speech turns observed since the last poll before a poll fires
        // (rough proxy for ~30 real sec of active exchange).
        int visitPollTurnCountThreshold = 4;
        // Real seconds of silence since the last observed speech turn before
        // a poll fires. Doubles as the "silence exceeded -> ContinueConversation"
        // threshold. Accumulation pauses while the game is paused (menus, load
        // screens) — see VisitConclusionPoll::GateTick. Real-time rather than
        // game-time so the threshold matches how long a real conversation
        // partner would wait, regardless of the user's iTimescale.
        int visitPollSilenceRealSeconds = 120;
        // In-game minutes since the last poll before a poll fires as a safety
        // ceiling. Guarantees the LLM verdict refreshes even during a
        // productive back-and-forth that never trips the other gates.
        int visitPollMaxIntervalGameMinutes = 10;
        // Consecutive poll failures (parse errors, LLM timeouts) before
        // hard-abort.
        int visitConclusionPollMaxConsecutiveFailures = 6;
        // Consecutive ContinueConversation fires without a poll ever returning
        // "concluded" in between; on this cap, force Valediction with an
        // elevated nudge_count so the closing line reads as a frustrated exit.
        int visitMaxIgnoreNudges = 3;
        // How long OnHold may persist while combat is the trigger before
        // hard-abort.
        int visitOnHoldCombatMaxSeconds = 60;
        // Wall-clock seconds between Valediction closing line and the
        // ReturnHome transition (dwell for the closing line to play out).
        int visitValedictionDwellSeconds = 10;
        // Sender-to-player distance during ReturnHome that triggers the final
        // teleport + shutdown. Also triggered by line-of-sight loss or cell
        // unload — whichever comes first.
        int visitReturnHomeExitDistanceUnits = 8000;
        // Outer wall-clock cap on ReturnHome — if the sender is still walking
        // after this many seconds, teleport anyway.
        int visitReturnHomeTimeoutSeconds = 300;

        // Enable toggle for the visit beat. See enableNpcLetter.
        bool enableNpcVisit = true;

        // --- AmbushBeat ---

        // [Beats]
        // Enable toggle for the ambush beat. See enableNpcLetter.
        bool enableAmbush = true;

        // Attacker count. The Director picks a count; these bound it.
        // The max is itself clamped on the read path to
        // kAmbushAttackerSlotCount, the number of Attacker0N aliases
        // actually authored on _ne_AmbushQuest — a hand-edited INI must
        // not be able to request a slot the ESP doesn't have.
        int ambushDefaultAttackerCount = 3;
        int ambushMinAttackerCount = 2;
        int ambushMaxAttackerCount = 6;

        // The band the spawn-point search works within. Not
        // Director-selectable: the search starts at the default and
        // widens toward the max looking for cover, clamped to this band
        // throughout. Below the min an ambush is visibly conjured;
        // above the max it is too far off to read as one.
        int ambushDefaultSpawnDistanceUnits = 2500;
        int ambushMinSpawnDistanceUnits = 2500;
        int ambushMaxSpawnDistanceUnits = 5500;

        // Approach-to-hostile handoff range. Attackers travel with
        // aggression 0 until they close to within this distance, then
        // flip to aggression 2 and start combat. Keeping the handoff
        // late means the approach package isn't fighting combat AI for
        // control the whole way in.
        int ambushEngageDistanceUnits = 1500;

        // Every surviving attacker beyond this distance ends the beat as
        // abandoned — the player outran it. Needs real clearance above
        // ambushMaxSpawnDistanceUnits, or an encounter that spawned at
        // the far end of the band abandons itself the moment the player
        // keeps walking.
        int ambushAbandonDistanceUnits = 8000;

        // Outer cap on RUNNING, in *unpaused real* seconds accumulated
        // from the tick argument (never a wall-clock timer of its own).
        int ambushMaxDurationSeconds = 600;

        // In-game-hour cooldown stamped after a completed or abandoned
        // ambush. Not stamped when COMPOSE fails, so a failed spawn
        // doesn't burn a day.
        int ambushPerBeatCooldownGameHours = 24;

        // StuckRecovery tuning. Read together — the pair is really one
        // setting, "slower than this counts as stopped". Terrain and
        // creature speeds vary enough between load orders that these
        // want to be tunable without a rebuild.
        int stuckRecoveryMovementThresholdUnits = 100;
        int stuckRecoveryCheckIntervalSeconds = 4;

        // [Gossip]
        // Background rumor propagation across the unique-actor
        // population. See docs/implementation/PHASE_13_GOSSIP_PROPAGATION.md.
        //
        // Ships DISABLED pending in-game validation. Every transmission
        // writes two real memories into SkyrimNet's database and does
        // not clean them up, so enable on a playthrough you are willing
        // to leave gossip in.
        bool gossipEnabled = false;
        // The dedicated rumor-trace log at
        // Data/../SKSE/NarrativeEngine_Gossip.log. Deliberately
        // independent of bDebugMode so a long validation session can run
        // with a quiet main log and a complete gossip trace.
        bool gossipLogEnabled = true;
        // Accumulator threshold for the simulation poll, in unpaused
        // real seconds (Tick-driven accumulator pattern).
        int gossipTickIntervalSeconds = 2;
        // Work caps per simulation firing, applied together. Neither
        // drops work — the remainder stays queued and drains on the next
        // firing, so load spreads across ticks by design.
        //
        // The MILLISECOND budget is the real governor and the count is a
        // backstop. Per-event cost is dominated by the AddMemory calls a
        // transmission makes, and that grows with the size of SkyrimNet's
        // memory database, so bounding time adapts where bounding a count
        // cannot.
        //
        // History worth keeping: the count was 25 for the first in-game
        // run and starved the queue outright — a game day generated far
        // more than 25 events, so the backlog grew every day and
        // propagation stalled at roughly one telling per rumor per day.
        // At 400 it merely throttled during time skips. It is now set high
        // enough that it should never bind; lower it only if profiling
        // shows the time budget is not doing its job.
        int gossipMaxEventsPerTick = 5000;

        // A carrier's finite daily budget of gossip-capable conversations,
        // divided among their contacts in proportion to the channel
        // weights below.
        //
        // This is the term that keeps propagation subcritical. Modelling
        // contact as a per-PAIR rate instead makes a person's total
        // social activity scale with the size of their settlement, and
        // the offline validation run showed that saturates the entire
        // province within one game day.

        // Relative channel weights. Ratios, NOT rates — only their
        // proportions matter, since the budget above sets the absolute
        // scale.
        //
        // The household:settlement ratio is what separates the three
        // tiers of the design target. Too low and settlements saturate
        // as readily as households; no value of the transmission
        // probability compensates, because the epidemic threshold is
        // sharp. See PHASE_13_SIR_VALIDATION_LOG.md, iterations 2-3.
        // The contact ladder. Seven rungs; a conversation draws a rung by
        // these relative weights and then a peer uniformly from within it.
        //
        // Rungs 1/3/5/7 are the geographic tiers. Rungs 2/4/6 have NO natural
        // membership -- they exist only to receive peers moved by a faction
        // or a relationship, which is what keeps a guild-mate three holds
        // away from being lost among a hundred hold-mates.
        //
        // MONOTONIC by contract: each rung must be <= the one closer than it.
        // A peer moved outward must end up spoken to less, not more, or the
        // ladder stops meaning anything. Nothing enforces this at load time
        // because a deliberate experiment is a legitimate thing to run.
        //
        // Defaults measured offline against the vanilla graph (857
        // participants) over a month of in-world time -- see
        // docs/implementation/tests/gossip-spread/simulate-tiered.py. They
        // give roughly 77% of rumors staying in one hold, 15% reaching two or
        // three, and 7% going wider.
        //
        // Rung 6 does not need to be loud. It carries only ~1% of tellings
        // and still produces the entire cross-hold band, because the point
        // was never its weight -- it was giving distant ties a pool of their
        // own instead of diluting them into the hold.
        std::array<float, 7> gossipTierWeights{0.33f, 0.20f, 0.18f, 0.12f, 0.09f, 0.06f, 0.02f};
        // Conversations each carrier holds per simulation step
        // (gossipStepDays). Expressed against the STEP, not the harvest tick:
        // those are separate clocks and coupling them would make the harvest
        // cadence silently change how fast rumors spread.
        float gossipConversationsPerStep = 5.0f;

        // Distance attenuation on the personal-edge channel. A guild-mate
        // in your own settlement is someone you see constantly; one three
        // holds away you see rarely, if ever.
        //
        // The channel originally had no distance term — deliberate, since
        // it is what carries a rumor between holds — which was harmless
        // at weight 4 but became the dominant cross-hold leak once the
        // weight rose to 40. Attenuating it is almost perfectly
        // selective: household, settlement and post-jump coverage are all
        // unchanged across the whole range, and only the frequency of
        // hold crossings moves.

        // --- SIR model ------------------------------------------------
        //
        // Susceptible -> Infectious (for a fixed period) -> Recovered
        // (immune permanently, never re-infectable).
        //
        // Transmissibility is CONSTANT for a rumor's whole life. A rumor
        // does not become less catching because it has changed hands; an
        // outbreak ends when conversations start landing on people who
        // already know — exhaustion of susceptible contacts, not decay.
        // Three earlier models all failed by making spread a function of
        // how far the rumor had already spread.

        // How long an NPC keeps bringing a rumor up after hearing it.
        float gossipInfectiousDays = 3.0f;
        // Per-conversation transmission probability is
        // `notability * this`. Lengthening the infectious period while
        // lowering this is what decouples the tiers: household
        // saturation needs only a few successes out of many attempts, so
        // extra conversations buy it cheaply, while outward spread is
        // governed by the probability.
        float gossipTransmissionScale = 0.055f;
        // Simulation step, in game days. Each step an infectious carrier
        // holds Poisson(conversationsPerDay * this) conversations.
        // Smaller is finer-grained and costs proportionally more events.
        float gossipStepDays = 0.25f;

        // Hard bounds on persistent state, so the co-save payload cannot
        // grow with playthrough length.
        //
        // Raised from 12 after the first SIR in-game run. Under SIR rumors
        // live long enough that a cap of 12 stayed saturated almost
        // permanently, and because SeedRumor refuses while the cap is full,
        // it silently throttled the harness to a third of its configured
        // seeding rate. Worth remembering if this is ever lowered again.
        //
        // At 40 x 80 carriers the co-save payload is roughly 95 KB — larger
        // than the 25 KB the original design budgeted, and acceptable only
        // because this is a validation harness. Bring it back down before
        // anything ships to players.
        int gossipMaxLiveRumors = 40;
        int gossipMaxCarriersPerRumor = 150;
        // Backstop only — a carrier normally recovers via
        // gossipInfectiousDays long before this.
        int gossipCarrierMaxAgeDays = 30;

        // Faction co-membership band. A faction outside this size range
        // is an attribute bucket rather than a social group. Size alone
        // is not sufficient — see NarrativeEngine_GossipFactions.ini for
        // the name-based filter that does the rest of the work.
        int gossipFactionSizeMin = 3;
        int gossipFactionSizeMax = 40;

        // --- Milestone 2: memory harvesting -------------------------
        //
        // Rumors are sourced from SkyrimNet's memory database rather than
        // planted. There is no global memory query — GetMemoriesForActor
        // is strictly per-actor — so a sweep draws one bucket of the
        // participant population and queries every member of it. See
        // iGossipHarvestBuckets below and GossipHarvest.h for why
        // selection does not rank.
        bool gossipHarvestEnabled = true;
        float gossipHarvestIntervalGameHours = 12.0f;
        // Memories older than this are never candidates. Gossip is news.
        float gossipHarvestWindowDays = 50.0f;
        // How long a claimed memory stays claimed.
        //
        // MUST EXCEED gossipHarvestWindowDays. A memory claimed the
        // instant it was created has its claim expire at this age but
        // stops being harvestable at the window — so the claim always
        // outlives eligibility, and the memory can never be seeded
        // twice. Invert the two and a memory claimed early becomes
        // re-harvestable partway through its life. Settings::Load
        // asserts this and complains loudly if it is violated.
        float gossipClaimExpiryDays = 60.0f;
        // Notability floor for a candidate. A memory's importance maps
        // directly onto the rumor's notability, both being 0..1.
        float gossipMinMemoryImportance = 0.45f;
        // How many buckets the participant population splits into. One
        // bucket is examined per sweep, in full, so this is the direct
        // control over per-sweep query cost — and the inverse control
        // over how long any given NPC waits for a turn. At 10 against
        // 881 participants: ~88 queries a sweep, a turn every ~5 game
        // days at the default interval.
        int gossipHarvestBuckets = 10;
        // How many of the most recent bucket selections are excluded from
        // the next draw.
        //
        // -1, the default, means bucketCount-1 and TRACKS the bucket count
        // rather than pinning a number beside it. That is the exact-cycle
        // setting: one bucket eligible per draw, so every bucket gets its
        // turn every bucketCount sweeps with no tail at all. Shorter
        // histories leave real choice in the draw and pay for it in
        // waiting — at 10 buckets and a history of 6 the mean wait is
        // still 10 sweeps but the worst case is 33.
        //
        // A non-negative value is used literally, clamped to bucketCount-1
        // so it can never leave the eligible set empty.
        int gossipBucketHistoryLength = -1;
        // Rows to ask SkyrimNet for, per actor.
        //
        // These come back already filtered, newest first: SkyrimNet
        // applies the tag, window and importance rules in SQL BEFORE
        // truncating to this count (API v10+), so all of them are usable
        // candidates. The sweep ranks the whole bucket's returns by
        // importance afterwards, so this sets how far back one actor can
        // contribute rather than how good the winner is.
        //
        // Set clear of where the cut was observed to bind, without
        // pretending to be unbounded. At 10 a live run truncated exactly
        // the actors worth harvesting: 11 actor-sweeps came back holding
        // precisely 10 rows -- Onmund, Faralda, J'zargo, Nirya, Brelyna,
        // Colette, the memory-rich College NPCs -- while everyone else
        // returned one to nine and was unaffected. Newest-first then hides
        // their older-but-weightier rows from the importance sort that
        // picks the winner, so the cut only ever costs quality where
        // quality was available.
        //
        // Those actors' true eligible depth is unknown -- the run only
        // establishes it is at least 10 -- so this is headroom chosen
        // against the observed bind, not against a measured ceiling. It
        // stays a real bound: an actor whose store outgrows it is capped
        // rather than dragged in whole, and the cost of the cap is one
        // bigger response, never more calls.
        int gossipHarvestMemoriesPerActor = 30;
        // How many rumors one sweep may actually seed. The walk down the
        // candidate pool stops once this many have been accepted.
        int gossipMaxSeedsPerHarvest = 1;
        // How many candidates a sweep may put to the evaluator before it
        // gives up. The sweep draws this many eligible candidates, shuffles
        // them, and evaluates one at a time until it has seeded
        // gossipMaxSeedsPerHarvest rumors or exhausted the pool.
        //
        // At 1 a single refusal wasted the whole sweep, which is what this
        // exists to fix: a third of evaluations come back refused, so a
        // pool of one seeded nothing about a third of the time despite
        // having other perfectly good candidates ranked behind it.
        //
        // The cost is bounded by refusals, not by the pool size — the walk
        // stops at the first acceptance. At the observed refusal rate the
        // expected spend is ~1.5 evaluation calls per sweep whether this is
        // 5 or 50; raising it only buys deeper cover for the unlucky sweep
        // where the first several are all refused.
        int gossipEvalAttemptsPerHarvest = 5;
        // --- Milestone 2: rumor content -----------------------------
        //
        // Generation bands, all produced by ONE call at seed time. Band
        // edges are at generation 3 and 6; measured depth reaches 12 but
        // generations 9+ carry only 4% of traffic, so a fourth band
        // would be output spent on almost nothing.
        //
        // The prompt asset and the LLM variant it runs under are NOT
        // configurable. The prompt ships in statics/ and its contract
        // with GossipContent (the context keys it reads, the JSON shape
        // it returns) is compiled in, so pointing the call at a
        // different asset can only break it. Gossip runs under the
        // existing `narrative_engine_composer` variant, which already
        // covers creative writing in an NPC's voice; a separate variant
        // would be one more thing for the user to tune with no distinct
        // task shape behind it.
        // Refuse to seed a rumor from someone with nobody available to
        // tell. This is the share of an origin's LOCAL rung weight (rungs
        // 1-6; the province rung is excluded, as it is for the stall test)
        // sitting on a rung that holds at least one reachable person.
        //
        // It is a share of WEIGHT, not of people, and the two diverge
        // sharply under the ladder. A carrier with no household, no
        // settlement and no ties scores 0.09 while having ninety
        // reachable hold-mates: the single rung they can use carries 9%
        // of the ladder and the other five come back silent.
        //
        // 0.05 rather than the 0.25 this began at. That figure was set
        // against the old flat contact model, where the share really did
        // mean "this fraction of your conversations reach a live person".
        // Under the ladder an empty rung produces SILENCE, which the model
        // already charges for -- such a carrier talks at a ninth of the
        // normal rate -- so gating them out as well charges twice. At 0.25
        // it excluded 103 of 857 vanilla participants, 69 of them at
        // exactly that 9% signature, including every NPC whose questline
        // stages them alone in a dungeon.
        //
        // No vanilla participant scores 0, so 0.05 reduces this to "has
        // anyone at all". What survives is the case it was built for: a
        // carrier whose rungs are populated but whose people are all dead
        // or quest-disabled, which is Ancano — five rumors seeded in one
        // session, every one reaching nobody, 61% of conversations landing
        // on unavailable listeners.
        float gossipMinAvailableContactShare = 0.05f;

        int gossipContentBands = 3;

        int gossipRandomSeed = 1337;
    };

    // Narrow mutation surface for WriteMcmOverride. One optional per
    // key the dashboard UI can edit; unset optionals are left alone on
    // disk. Field order mirrors Config's field order for readability.
    // The read surface (ReadIniInto) is *universal* — every Config
    // field is populated on read. The write surface is deliberately
    // narrower so callers can't accidentally author a key the UI has
    // no editor for.
    struct McmOverride
    {
        std::optional<bool> debugMode;
        std::optional<bool> traceMode;
        std::optional<bool> tickEnabled;
        std::optional<int> tickIntervalSeconds;
        std::optional<int> minPhaseDurationSeconds;
        std::optional<int> idealDurationExposition;
        std::optional<int> idealDurationRisingAction;
        std::optional<int> idealDurationClimax;
        std::optional<int> idealDurationFallingAction;
        std::optional<int> idealDurationResolution;
        std::optional<int> dashboardHotkeyDXSC;
        std::optional<bool> hotkeyShift;
        std::optional<bool> hotkeyCtrl;
        std::optional<bool> hotkeyAlt;
        std::optional<int> letterComposeMemoryRenderCap;
        std::optional<int> letterComposeDialogueRenderCap;
        std::optional<int> actionSelectEventRenderCap;
        std::optional<int> actionSelectLetterMemoryRenderCap;
        std::optional<int> actionSelectVisitMemoryRenderCap;
        std::optional<int> skyrimNetEventTailSizeForPrompt;
        std::optional<int> decisionLogTailSizeForPrompt;
        std::optional<int> visitComposeMemoryRenderCap;
        std::optional<int> visitComposeDialogueRenderCap;
        std::optional<int> visitConclusionPollRecentLinesRenderCap;
    };

    // Read the plugin INI, then apply any MCM-managed override, and
    // populate the singleton. Call once from kDataLoaded BEFORE any
    // subsystem that reads settings.
    void Load();

    // Access the loaded config. Stable reference for the plugin's lifetime.
    // Fields may be mutated at runtime via ApplyMcmOverride (which re-runs
    // the universal read against the MCM INI) or WriteMcmOverride (which
    // updates specific fields in place before writing them to disk).
    const Config& Get();

    // Re-read Data/MCM/Settings/NarrativeEngine.ini and apply every
    // recognized key as an override on top of the current in-memory
    // Config. Called by MCMEventSink when the MCM page fires
    // "_ne_DashboardHotkeyChanged", and by Load() as the second pass
    // of the cascade. Safe to call any time after Load(); no-ops
    // silently if the MCM INI is absent (fresh install where the
    // player has never opened the page).
    void ApplyMcmOverride();

    // Case-insensitive membership check against the parsed spell-name
    // blocklist derived from Config::spellNameBlocklist. Empty
    // blocklist always returns false. The underlying set is rebuilt
    // by Load / ApplyMcmOverride and read-only afterward, so this is
    // safe to call from any thread (including engine sink threads).
    bool IsSpellNameBlocked(std::string_view spellName);

    // Write a subset of Config fields to the MCM INI at
    // Data/MCM/Settings/NarrativeEngine.ini. Reads the current file
    // (preserving unknown keys / comments), sets the values whose
    // optionals are engaged, writes back atomically, then applies the
    // same mutations to the in-memory Config singleton so
    // Settings::Get() reflects the write immediately (no wait for a
    // subsequent Load / ApplyMcmOverride). Never touches the plugin
    // INI. Safe to call from the main thread only (SimpleIni is
    // single-threaded).
    void WriteMcmOverride(const McmOverride& mutations);

    // Per-beat enable flags, addressed by the beat's `IBeat::Name()`.
    //
    // These are not part of McmOverride because the callers that need
    // them only have a beat NAME — the dashboard's Dispatch tab hands
    // back `{"name":"npc_visit","enabled":false}`, with no way to pick
    // the right struct field. Both functions share one internal
    // name-to-key table so the read path (BeatRegistry's initial state)
    // and the write path (dashboard toggles) cannot drift apart.
    //
    // A new beat needs one row in that table and nothing else.

    // Current enabled state for `beatName`, or `fallback` when the beat
    // has no registered enable key.
    bool GetBeatEnabled(std::string_view beatName, bool fallback);

    // Persist `enabled` for `beatName` to the MCM override INI and
    // update the in-memory Config. Returns false (and logs) for a beat
    // with no registered enable key. Main thread only, same as
    // WriteMcmOverride.
    bool WriteBeatEnabledOverride(std::string_view beatName, bool enabled);
} // namespace NarrativeEngine::Settings
