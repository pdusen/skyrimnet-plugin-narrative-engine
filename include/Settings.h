#pragma once

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

        // [Dashboard]
        int dashboardHotkeyDXSC = 65;              // DirectX scan code; 65 == DIK_F7; -1 disables
        std::uint8_t dashboardHotkeyModifiers = 0; // kModShift|kModCtrl|kModAlt bitmask; 0 = none

        // [CombatEvents]
        int combatEventsHitRadiusUnits = 6000; // ~90 ft; distance gate for hit / collapse capture
        int combatEventsMaxStored = 256;       // ring-buffer cap on retained internal combat events

        // [Beats]
        // Per-beat enable defaults seeded into BeatRegistry at Register
        // time. Dashboard Dispatch tab surfaces runtime toggles for
        // these, but runtime changes don't write back to INI — reload
        // the game and the INI value wins again. Debug testing aid, not
        // a persistent config surface.
        bool enableAmbush = true;
        bool enableNpcLetter = true;

        // AmbushBeat parameter defaults + clamps. The LLM may supply
        // bandit_count and spawn_distance_units in its beat-select
        // response; the beat validates against these bounds and falls
        // back to the default when the supplied value is out of range
        // or missing.
        int ambushDefaultBanditCount = 3;
        int ambushDefaultSpawnDistanceUnits = 2000;
        int ambushMinBanditCount = 2;
        int ambushMaxBanditCount = 4;
        int ambushMinSpawnDistanceUnits = 1500;
        int ambushMaxSpawnDistanceUnits = 3000;
        // Per-action cooldown in *in-game hours* applied after a
        // successful ambush completion (the global
        // iBeatCooldownSeconds also applies on top of this). 0
        // disables. Persists via the beat's own co-save record.
        int ambushPerBeatCooldownGameHours = 24;

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
        int letterPoolSize = 20;                       // informational; ESP defines the actual 20 forms
        int letterDispatchVerifyDelaySeconds = 5;      // grace window before RUNNING gives up on the courier handoff
        int letterPendingDeliveryTimeoutSeconds = 600; // load-time demotion gate for stuck PendingDelivery slots

        // Per-beat cooldown in *in-game hours* applied after the letter
        // successfully reaches the vanilla courier container.
        // Independent of the global iBeatCooldownSeconds real-time
        // cooldown, which still applies on top. 0 disables. Persists
        // via the beat's own co-save record. See notes on
        // ambushPerBeatCooldownGameHours for the same pattern.
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
        int visitSalutationApproachDistanceUnits = 900;
        // Distance for the ReEngage resumption line to fire (slightly larger
        // than Salutation to give the sender room to catch up).
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

        // Enable toggle for the visit beat; runtime dashboard toggles
        // don't write back to INI.
        bool enableNpcVisit = true;
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
        std::optional<bool> tickEnabled;
        std::optional<int> tickIntervalSeconds;
        std::optional<int> idealDurationExposition;
        std::optional<int> idealDurationRisingAction;
        std::optional<int> idealDurationClimax;
        std::optional<int> idealDurationFallingAction;
        std::optional<int> idealDurationResolution;
        std::optional<int> dashboardHotkeyDXSC;
        std::optional<bool> hotkeyShift;
        std::optional<bool> hotkeyCtrl;
        std::optional<bool> hotkeyAlt;
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
} // namespace NarrativeEngine::Settings
