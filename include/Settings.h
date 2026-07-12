#pragma once

#include <cstdint>
#include <string>

// Author-tunable plugin configuration.
//
// Read once at SKSE's kDataLoaded message via Load(). Sources, in order:
//   1. Data/SKSE/Plugins/NarrativeEngine.ini (the plugin INI)
//   2. Data/MCM/Settings/NarrativeEngine.ini (MCM Helper-managed; overrides
//      only the [Dashboard] keys when present)
//
// Any missing file or missing key falls back to the default baked into the
// Config struct below — the plugin is fully functional with no INI at all.
namespace NarrativeEngine::Settings
{
    // Bitmask values for the dashboard hotkey's modifier keys.
    // Combinable: (kModCtrl | kModShift | kModAlt) == 7.
    inline constexpr std::uint8_t kModCtrl  = 1;
    inline constexpr std::uint8_t kModShift = 2;
    inline constexpr std::uint8_t kModAlt   = 4;

    struct Config
    {
        // [General]
        bool debugMode = false;

        // [Director]
        // TEMP: 30 for development iteration; ship default is 90.
        int tickIntervalSeconds              = 30;   // wall-clock seconds between evaluations
        int decisionLogMaxEntries            = 200;  // ring buffer cap
        int decisionLogTailSizeForPrompt     = 10;   // entries fed into BuildPromptContext
        int skyrimNetEventTailSizeForPrompt  = 40;   // maxCount passed to PublicGetRecentEvents

        // Per-current-phase tension thresholds that drive Freytag advancement.
        // The LLM returns only a tension score; the system decides advancement
        // by comparing that score against the threshold for the *current*
        // phase. Each threshold is either a "rises above" or "drops below"
        // gate depending on the dramatic shape of the transition out of that
        // phase — Exposition/RisingAction/Resolution rise into their successor,
        // Climax/FallingAction wind down into theirs. All values are
        // 0..100 to match the tension-score domain.
        int advanceThresholdExposition       = 45;   // -> RisingAction when score >=
        int advanceThresholdRisingAction     = 80;   // -> Climax when score >=
        int advanceThresholdClimax           = 60;   // -> FallingAction when score <=
        int advanceThresholdFallingAction    = 30;   // -> Resolution when score <=
        int advanceThresholdResolution       = 25;   // -> Exposition when score >=

        // Per-phase ideal durations in unpaused real-time seconds. The
        // ActionDispatcher gates on these — actions may only fire after
        // the current phase has overstayed its ideal duration. Total ideal
        // cycle at defaults: 1200s (20 min); proportions follow the design
        // narrative — Exposition and Resolution sit longer; Climax is brief.
        int idealDurationExposition          = 330;  // 5.5 min
        int idealDurationRisingAction        = 225;  // 3.75 min
        int idealDurationClimax              = 90;   // 1.5 min
        int idealDurationFallingAction       = 225;  // 3.75 min
        int idealDurationResolution          = 330;  // 5.5 min

        // [BeatSystem]
        // Master poll cadence for the Narrative Beat System's worker
        // thread. See PHASE_06_BEAT_SYSTEM_REFACTOR.md.
        int beatSystemPollIntervalMs         = 250;
        // Beat dispatch knobs.
        int beatCooldownSeconds              = 120;  // wall-clock seconds after beat COMPLETION before next may fire
        int beatRepetitionWindowSeconds      = 300;  // window during which the same beat name is excluded from picks
        // TODO PHASE-06: actionStaleLockTimeoutSeconds is only consumed by
        // ActionDispatcher, which is deleted in Step 11. Removed with it.
        int actionStaleLockTimeoutSeconds    = 900;  // (transitional — dies with ActionDispatcher)

        // NPCLetterAction precondition: minimum number of recently-engaged
        // NPCs SkyrimNet must report before the action becomes available.
        // Below this, the letter would either fail or fall back to a
        // generated persona; we'd rather skip the action than ship that.
        int letterMinSenderCandidates        = 3;

        // [AlphaCanon]
        // Comma-separated list of cell EditorIDs to treat as do-not-disturb.
        // Empty by default. Whitespace around commas is allowed.
        std::string doNotDisturbCellEDIDsCSV;

        // [Dashboard]
        int          dashboardHotkeyVK        = 118; // Windows VK code; 118 == VK_F7; -1 disables
        std::uint8_t dashboardHotkeyModifiers = 0;   // kModCtrl|kModShift|kModAlt bitmask; 0 = none

        // [CombatEvents]
        int combatEventsHitRadiusUnits = 6000;  // ~90 ft; distance gate for hit / collapse capture
        int combatEventsMaxStored      = 256;   // ring-buffer cap on retained internal combat events

        // [Actions]
        // Per-action enable defaults seeded into ActionRegistry at
        // Register time. Dashboard Dispatch tab surfaces runtime
        // toggles for these, but runtime changes don't write back to
        // INI — reload the game and the INI value wins again. Debug
        // testing aid, not a persistent config surface.
        bool enableAmbush    = true;
        bool enableNpcLetter = true;

        // AmbushAction parameter defaults + clamps. The LLM may supply
        // bandit_count and spawn_distance_units in its action-select
        // response; the action validates against these bounds and falls
        // back to the default when the supplied value is out of range or
        // missing. See PHASE_03_ACTION_TOOLBOX.md.
        int ambushDefaultBanditCount         = 3;
        int ambushDefaultSpawnDistanceUnits  = 2000;
        int ambushMinBanditCount             = 2;
        int ambushMaxBanditCount             = 4;
        int ambushMinSpawnDistanceUnits      = 1500;
        int ambushMaxSpawnDistanceUnits      = 3000;
        // Per-action cooldown in *in-game hours* applied after a
        // successful ambush completion (the global
        // iActionCooldownSeconds also applies on top of this). 0
        // disables. Persists via the action's own co-save record.
        int ambushPerBeatCooldownGameHours = 24;

        // NPCLetterAction / LetterPool content + dispatch knobs. See
        // PHASE_04_LETTER_POOL_AND_NPC_LETTER_ACTION.md.
        int letterContentMinWords                = 60;   // lower bound on LLM body length
        int letterContentMaxWords                = 180;  // upper bound on LLM body length
        // Minimum `importance_score` (0.0–1.0) a SkyrimNet memory must
        // have to be included in the sender's memory tail passed to
        // the LLM. Filters out low-signal chatter so both the
        // action-select "who should send this?" pick and the compose
        // "what should they say?" call see only memories that carried
        // real weight when they happened.
        float letterMemoryImportanceThreshold    = 0.4f;
        int letterPoolSize                       = 20;   // informational; ESP defines the actual 20 forms
        int letterDispatchVerifyDelaySeconds     = 5;    // grace window before DetectAndRollbackFailedStart gives up
        int letterPendingDeliveryTimeoutSeconds  = 600;  // load-time demotion gate for stuck PendingDelivery slots

        // Per-action cooldown in *in-game hours* applied after the
        // letter successfully reaches the vanilla courier container
        // (Phase C verified by DetectCompletion). Independent of the
        // global iActionCooldownSeconds real-time cooldown, which
        // still applies on top. 0 disables. Persists via the action's
        // own co-save record. See notes in AmbushAction for the same
        // pattern.
        int letterBeatCooldownGameHours          = 24;

        // Per-sender cooldown in *in-game hours* applied after the
        // vanilla courier hands the letter to the player (delivery
        // event). Prevents the same sender from being picked as a
        // candidate again for this many in-game hours, avoiding the
        // "three letters from Ancano in one session" pathology. 0
        // disables the filter. Persists per-sender-FormID in the
        // action's co-save record.
        int letterSenderCooldownGameHours        = 72;

        // [LetterPool]
        // 0 = silent, 1 = log evictions, 2 = log every state transition.
        int letterPoolEvictionLogVerbosity       = 1;

        // --- Phase 05 (NPCVisitAction) ---

        // [Director]
        // NPCVisitAction precondition: minimum number of viable sender
        // candidates required for IsAvailable to return true. Below this the
        // action declines and the dispatcher considers other picks.
        int visitMinSenderCandidates             = 3;

        // [Actions] — dispatch / composition
        int visitBriefingMinWords                = 40;
        int visitBriefingMaxWords                = 120;
        int visitMarkerMinDistanceUnits          = 800;   // closest spawn marker may be
        int visitMarkerMaxDistanceUnits          = 2500;  // farthest spawn marker may be

        // Per-sender cooldown in *in-game hours* applied once a visit's
        // Salutation → Discuss transition fires (i.e., the sender actually
        // showed up and spoke to the player). Prevents the same NPC from
        // being picked as a visit sender again for this many in-game
        // hours, avoiding the "Ancano visits three times in one session"
        // pathology. 0 disables the filter. Persists per-sender-FormID
        // in the action's own co-save record.
        int visitSenderCooldownGameHours         = 72;

        // [Actions] — state machine timing
        // Salutation timeout: seconds after Start before rollback if the sender
        // hasn't closed distance to speak the opening line.
        int visitApproachTimeoutSeconds          = 60;
        // Distance at which the Salutation opening line fires and the machine
        // advances to Discuss. Kept generous (~900u) so the LLM + TTS pipeline
        // has time to generate the opening line while the sender is still
        // closing the last stretch — otherwise the sender arrives at the
        // player and stands there silently while the response streams in.
        int visitSalutationApproachDistanceUnits = 900;
        // Distance for the ReEngage resumption line to fire (slightly larger
        // than Salutation to give the sender room to catch up).
        int visitReEngageApproachDistanceUnits   = 1000;
        // Wall-clock cadence (seconds) at which C++ evaluates the three
        // cheap-signal gates that decide whether to fire the natural-conclusion
        // LLM poll during Discuss.
        int visitPollGateTickSeconds             = 1;
        // Speech turns observed since the last poll before a poll fires
        // (rough proxy for ~30 real sec of active exchange).
        int visitPollTurnCountThreshold          = 4;
        // Real seconds of silence since the last observed speech turn before
        // a poll fires. Doubles as the "silence exceeded -> ContinueConversation"
        // threshold. Accumulation pauses while the game is paused (menus, load
        // screens) — see VisitConclusionPoll::GateTick. Real-time rather than
        // game-time so the threshold matches how long a real conversation
        // partner would wait, regardless of the user's iTimescale.
        int visitPollSilenceRealSeconds          = 120;
        // In-game minutes since the last poll before a poll fires as a safety
        // ceiling. Guarantees the LLM verdict refreshes even during a
        // productive back-and-forth that never trips the other gates.
        int visitPollMaxIntervalGameMinutes      = 10;
        // Consecutive poll failures (parse errors, LLM timeouts) before
        // hard-abort.
        int visitConclusionPollMaxConsecutiveFailures = 6;
        // Consecutive ContinueConversation fires without a poll ever returning
        // "concluded" in between; on this cap, force Valediction with an
        // elevated nudge_count so the closing line reads as a frustrated exit.
        int visitMaxIgnoreNudges                 = 3;
        // How long OnHold may persist while combat is the trigger before
        // hard-abort.
        int visitOnHoldCombatMaxSeconds          = 60;
        // Wall-clock seconds between Valediction closing line and the
        // ReturnHome transition (dwell for the closing line to play out).
        int visitValedictionDwellSeconds         = 10;
        // Sender-to-player distance during ReturnHome that triggers the final
        // teleport + shutdown. Also triggered by line-of-sight loss or cell
        // unload — whichever comes first.
        int visitReturnHomeExitDistanceUnits     = 8000;
        // Outer wall-clock cap on ReturnHome — if the sender is still walking
        // after this many seconds, teleport anyway.
        int visitReturnHomeTimeoutSeconds        = 300;
        // Upper bound on the wall-clock duration of the OnHold / ReEngage /
        // Discuss polling watchdogs' async lifetimes. Not a visit-abort
        // clock — its onTimeout branches only stop the watchdog and log a
        // warning, they do not teleport the sender or teardown the visit.
        // Sized well past any natural visit length so it functions purely
        // as an infrastructure safety-net; the visit itself ends via poll
        // conclusions, ignore-nudge cap, sender/player death, or combat-
        // stuck (see CheckHardAbortConditions).
        int visitHardTimeoutSeconds              = 86400;

        // Enable toggle for the visit action; seeded into the ActionRegistry
        // the same way ambush / letter enables are. Runtime dashboard toggles
        // don't write back to INI.
        bool enableNpcVisit                      = true;
    };

    // Read both INIs (plugin then MCM override) and populate the singleton.
    // Call once from kDataLoaded BEFORE any subsystem that reads settings.
    void Load();

    // Access the loaded config. Stable reference for the plugin's lifetime —
    // Load() populates the singleton; nothing else mutates it.
    const Config& Get();
}
