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

        // ActionDispatcher knobs.
        int actionCooldownSeconds            = 120;  // wall-clock seconds after action COMPLETION before next may fire
        int actionRepetitionWindowSeconds    = 300;  // window during which the same action name is excluded from picks
        int actionStaleLockTimeoutSeconds    = 900;  // auto-clear an in-flight action that never sends completion

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
        int ambushPerActionCooldownGameHours = 24;

        // NPCLetterAction / LetterPool content + dispatch knobs. See
        // PHASE_04_LETTER_POOL_AND_NPC_LETTER_ACTION.md.
        int letterContentMinWords                = 60;   // lower bound on LLM body length
        int letterContentMaxWords                = 180;  // upper bound on LLM body length
        int letterPoolSize                       = 20;   // informational; ESP defines the actual 20 forms
        int letterDispatchVerifyDelaySeconds     = 5;    // grace window before DetectAndRollbackFailedStart gives up
        int letterPendingDeliveryTimeoutSeconds  = 600;  // load-time demotion gate for stuck PendingDelivery slots

        // [LetterPool]
        // 0 = silent, 1 = log evictions, 2 = log every state transition.
        int letterPoolEvictionLogVerbosity       = 1;
    };

    // Read both INIs (plugin then MCM override) and populate the singleton.
    // Call once from kDataLoaded BEFORE any subsystem that reads settings.
    void Load();

    // Access the loaded config. Stable reference for the plugin's lifetime —
    // Load() populates the singleton; nothing else mutates it.
    const Config& Get();
}
