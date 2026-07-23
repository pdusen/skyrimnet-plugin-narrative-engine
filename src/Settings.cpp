#include <Settings.h>

#include <logger.h>

#include <SimpleIni.h>

namespace NarrativeEngine::Settings
{
    namespace
    {
        Config g_config{};

        constexpr const char* kPluginIniPath = "Data/SKSE/Plugins/NarrativeEngine.ini";
        constexpr const char* kMcmIniPath = "Data/MCM/Settings/NarrativeEngine.ini";

        // Populate `dst` from every recognized key in `ini`.
        //
        // The load-bearing convention: each Get*Value call passes the
        // current `dst.<field>` as its default. That single pattern gives
        // the cascade its fall-through semantics regardless of which INI
        // is being read:
        //   * First pass (against the plugin INI, `dst` freshly defaulted
        //     from Config{}): missing keys land on the Config baked-in
        //     default.
        //   * Second pass (against the MCM INI, `dst` already populated
        //     from the plugin INI): missing keys land on the plugin-INI
        //     value.
        // If a future refactor drops the convention (e.g. passing 0 as
        // the default for an int read), the cascade silently breaks for
        // that key — reviewers should watch for that.
        //
        // The hotkey [Dashboard] section holds three separate bools for
        // the modifier keys (MCM Helper toggles have no built-in
        // bit-manipulation semantics; three checkboxes is the standard
        // shape). We reconstruct the SkyUI-convention bitmask on read;
        // the plugin INI can use either shape (bools or `iHotkeyModifiers`),
        // with the bools taking precedence when both are present.
        void ReadIniInto(CSimpleIniA& ini, Config& dst)
        {
            dst.debugMode = ini.GetBoolValue("General", "bDebugMode", dst.debugMode);
            dst.traceMode = ini.GetBoolValue("General", "bTraceMode", dst.traceMode);

            dst.tickIntervalSeconds =
                static_cast<int>(ini.GetLongValue("Director", "iTickIntervalSeconds", dst.tickIntervalSeconds));
            dst.tickEnabled = ini.GetBoolValue("Director", "bTickEnabled", dst.tickEnabled);
            dst.minPhaseDurationSeconds =
                static_cast<int>(ini.GetLongValue("Director", "iMinPhaseDurationSeconds", dst.minPhaseDurationSeconds));
            dst.decisionLogMaxEntries =
                static_cast<int>(ini.GetLongValue("Director", "iDecisionLogMaxEntries", dst.decisionLogMaxEntries));
            dst.decisionLogTailSizeForPrompt = static_cast<int>(
                ini.GetLongValue("Director", "iDecisionLogTailSizeForPrompt", dst.decisionLogTailSizeForPrompt));
            dst.skyrimNetEventTailSizeForPrompt = static_cast<int>(
                ini.GetLongValue("Director", "iSkyrimNetEventTailSizeForPrompt", dst.skyrimNetEventTailSizeForPrompt));

            dst.advanceThresholdExposition = static_cast<int>(
                ini.GetLongValue("Director", "iAdvanceThresholdExposition", dst.advanceThresholdExposition));
            dst.advanceThresholdRisingAction = static_cast<int>(
                ini.GetLongValue("Director", "iAdvanceThresholdRisingAction", dst.advanceThresholdRisingAction));
            dst.advanceThresholdClimax =
                static_cast<int>(ini.GetLongValue("Director", "iAdvanceThresholdClimax", dst.advanceThresholdClimax));
            dst.advanceThresholdFallingAction = static_cast<int>(
                ini.GetLongValue("Director", "iAdvanceThresholdFallingAction", dst.advanceThresholdFallingAction));
            dst.advanceThresholdResolution = static_cast<int>(
                ini.GetLongValue("Director", "iAdvanceThresholdResolution", dst.advanceThresholdResolution));

            dst.idealDurationExposition =
                static_cast<int>(ini.GetLongValue("Director", "iIdealDurationExposition", dst.idealDurationExposition));
            dst.idealDurationRisingAction = static_cast<int>(
                ini.GetLongValue("Director", "iIdealDurationRisingAction", dst.idealDurationRisingAction));
            dst.idealDurationClimax =
                static_cast<int>(ini.GetLongValue("Director", "iIdealDurationClimax", dst.idealDurationClimax));
            dst.idealDurationFallingAction = static_cast<int>(
                ini.GetLongValue("Director", "iIdealDurationFallingAction", dst.idealDurationFallingAction));
            dst.idealDurationResolution =
                static_cast<int>(ini.GetLongValue("Director", "iIdealDurationResolution", dst.idealDurationResolution));

            dst.beatSystemPollIntervalMs = static_cast<int>(
                ini.GetLongValue("BeatSystem", "iBeatSystemPollIntervalMs", dst.beatSystemPollIntervalMs));
            dst.beatCooldownSeconds =
                static_cast<int>(ini.GetLongValue("BeatSystem", "iBeatCooldownSeconds", dst.beatCooldownSeconds));
            dst.beatRepetitionWindowSeconds = static_cast<int>(
                ini.GetLongValue("BeatSystem", "iBeatRepetitionWindowSeconds", dst.beatRepetitionWindowSeconds));

            dst.letterMinSenderCandidates = static_cast<int>(
                ini.GetLongValue("Director", "iLetterMinSenderCandidates", dst.letterMinSenderCandidates));

            dst.doNotDisturbCellEDIDsCSV =
                ini.GetValue("AlphaCanon", "sDoNotDisturbCellEDIDsCSV", dst.doNotDisturbCellEDIDsCSV.c_str());

            // [Dashboard] — DXSC always via GetLongValue; modifiers via
            // the three-bool shape (MCM Helper's schema), reconstructed
            // into the bitmask. If none of the three bool keys are
            // present, fall back to `iHotkeyModifiers` for
            // plugin-INI-only backwards compatibility.
            dst.dashboardHotkeyDXSC =
                static_cast<int>(ini.GetLongValue("Dashboard", "iHotkeyDXSC", dst.dashboardHotkeyDXSC));

            const bool hasShift = ini.GetValue("Dashboard", "bHotkeyShift", nullptr) != nullptr;
            const bool hasCtrl = ini.GetValue("Dashboard", "bHotkeyCtrl", nullptr) != nullptr;
            const bool hasAlt = ini.GetValue("Dashboard", "bHotkeyAlt", nullptr) != nullptr;
            if (hasShift || hasCtrl || hasAlt) {
                const bool shift = ini.GetBoolValue("Dashboard", "bHotkeyShift", false);
                const bool ctrl = ini.GetBoolValue("Dashboard", "bHotkeyCtrl", false);
                const bool alt = ini.GetBoolValue("Dashboard", "bHotkeyAlt", false);
                std::uint8_t mods = 0;
                if (shift)
                    mods |= kModShift;
                if (ctrl)
                    mods |= kModCtrl;
                if (alt)
                    mods |= kModAlt;
                dst.dashboardHotkeyModifiers = mods;
            } else {
                dst.dashboardHotkeyModifiers = static_cast<std::uint8_t>(
                    ini.GetLongValue("Dashboard", "iHotkeyModifiers", dst.dashboardHotkeyModifiers));
            }

            dst.combatEventsHitRadiusUnits =
                static_cast<int>(ini.GetLongValue("CombatEvents", "iHitRadiusUnits", dst.combatEventsHitRadiusUnits));
            dst.combatEventsMaxStored =
                static_cast<int>(ini.GetLongValue("CombatEvents", "iMaxStored", dst.combatEventsMaxStored));

            dst.weatherEventsMaxStored = static_cast<int>(
                ini.GetLongValue("WeatherEvents", "iWeatherEventsMaxStored", dst.weatherEventsMaxStored));
            dst.weatherEventPollIntervalSeconds = static_cast<int>(ini.GetLongValue(
                "WeatherEvents", "iWeatherEventPollIntervalSeconds", dst.weatherEventPollIntervalSeconds));
            dst.weatherEventsDebounceSeconds = static_cast<int>(
                ini.GetLongValue("WeatherEvents", "iWeatherEventDebounceSeconds", dst.weatherEventsDebounceSeconds));

            dst.holdGridDebugBitmap = ini.GetBoolValue("HoldGrid", "bHoldGridDebugBitmap", dst.holdGridDebugBitmap);
            dst.holdGridPruneMaxClusterSize = static_cast<int>(
                ini.GetLongValue("HoldGrid", "iHoldGridPruneMaxClusterSize", dst.holdGridPruneMaxClusterSize));
            dst.holdGridPruneIsolationRadius = static_cast<int>(
                ini.GetLongValue("HoldGrid", "iHoldGridPruneIsolationRadius", dst.holdGridPruneIsolationRadius));

            dst.eventHistoryEnabled = ini.GetBoolValue("EventHistory", "bEventHistoryEnabled", dst.eventHistoryEnabled);
            dst.eventHistoryFlushIntervalSeconds = static_cast<int>(ini.GetLongValue(
                "EventHistory", "iEventHistoryFlushIntervalSeconds", dst.eventHistoryFlushIntervalSeconds));

            dst.travelEventsMaxStored =
                static_cast<int>(ini.GetLongValue("TravelEvents", "iTravelEventsMaxStored", dst.travelEventsMaxStored));
            dst.travelCondensationWindowSeconds = static_cast<int>(ini.GetLongValue(
                "TravelEvents", "iTravelCondensationWindowSeconds", dst.travelCondensationWindowSeconds));
            dst.travelFollowerRadiusUnits = static_cast<int>(
                ini.GetLongValue("TravelEvents", "iTravelFollowerRadiusUnits", dst.travelFollowerRadiusUnits));

            dst.enableAmbush = ini.GetBoolValue("Beats", "bEnableAmbush", dst.enableAmbush);
            dst.enableNpcLetter = ini.GetBoolValue("Beats", "bEnableNpcLetter", dst.enableNpcLetter);

            dst.ambushDefaultBanditCount =
                static_cast<int>(ini.GetLongValue("Beats", "iAmbushDefaultBanditCount", dst.ambushDefaultBanditCount));
            dst.ambushDefaultSpawnDistanceUnits = static_cast<int>(
                ini.GetLongValue("Beats", "iAmbushDefaultSpawnDistanceUnits", dst.ambushDefaultSpawnDistanceUnits));
            dst.ambushMinBanditCount =
                static_cast<int>(ini.GetLongValue("Beats", "iAmbushMinBanditCount", dst.ambushMinBanditCount));
            dst.ambushMaxBanditCount =
                static_cast<int>(ini.GetLongValue("Beats", "iAmbushMaxBanditCount", dst.ambushMaxBanditCount));
            dst.ambushMinSpawnDistanceUnits = static_cast<int>(
                ini.GetLongValue("Beats", "iAmbushMinSpawnDistanceUnits", dst.ambushMinSpawnDistanceUnits));
            dst.ambushMaxSpawnDistanceUnits = static_cast<int>(
                ini.GetLongValue("Beats", "iAmbushMaxSpawnDistanceUnits", dst.ambushMaxSpawnDistanceUnits));
            dst.ambushPerBeatCooldownGameHours = static_cast<int>(
                ini.GetLongValue("Beats", "iAmbushPerBeatCooldownGameHours", dst.ambushPerBeatCooldownGameHours));

            dst.letterContentMinWords =
                static_cast<int>(ini.GetLongValue("Beats", "iLetterContentMinWords", dst.letterContentMinWords));
            dst.letterContentMaxWords =
                static_cast<int>(ini.GetLongValue("Beats", "iLetterContentMaxWords", dst.letterContentMaxWords));
            dst.letterMemoryImportanceThreshold = static_cast<float>(
                ini.GetDoubleValue("Beats", "fLetterMemoryImportanceThreshold", dst.letterMemoryImportanceThreshold));
            dst.letterPoolSize = static_cast<int>(ini.GetLongValue("Beats", "iLetterPoolSize", dst.letterPoolSize));
            dst.letterDispatchVerifyDelaySeconds = static_cast<int>(
                ini.GetLongValue("Beats", "iLetterDispatchVerifyDelaySeconds", dst.letterDispatchVerifyDelaySeconds));
            dst.letterPendingDeliveryTimeoutSeconds = static_cast<int>(ini.GetLongValue(
                "Beats", "iLetterPendingDeliveryTimeoutSeconds", dst.letterPendingDeliveryTimeoutSeconds));
            dst.letterBeatCooldownGameHours = static_cast<int>(
                ini.GetLongValue("Beats", "iLetterBeatCooldownGameHours", dst.letterBeatCooldownGameHours));
            dst.letterSenderCooldownGameHours = static_cast<int>(
                ini.GetLongValue("Beats", "iLetterSenderCooldownGameHours", dst.letterSenderCooldownGameHours));

            dst.letterPoolEvictionLogVerbosity = static_cast<int>(
                ini.GetLongValue("LetterPool", "iLetterPoolEvictionLogVerbosity", dst.letterPoolEvictionLogVerbosity));

            // --- NPCVisitBeat ---

            dst.visitMinSenderCandidates = static_cast<int>(
                ini.GetLongValue("Director", "iVisitMinSenderCandidates", dst.visitMinSenderCandidates));

            dst.enableNpcVisit = ini.GetBoolValue("Beats", "bEnableNpcVisit", dst.enableNpcVisit);

            dst.visitBriefingMinWords =
                static_cast<int>(ini.GetLongValue("Beats", "iVisitBriefingMinWords", dst.visitBriefingMinWords));
            dst.visitBriefingMaxWords =
                static_cast<int>(ini.GetLongValue("Beats", "iVisitBriefingMaxWords", dst.visitBriefingMaxWords));
            dst.visitMarkerMinDistanceUnits = static_cast<int>(
                ini.GetLongValue("Beats", "iVisitMarkerMinDistanceUnits", dst.visitMarkerMinDistanceUnits));
            dst.visitMarkerMaxDistanceUnits = static_cast<int>(
                ini.GetLongValue("Beats", "iVisitMarkerMaxDistanceUnits", dst.visitMarkerMaxDistanceUnits));
            dst.visitSenderCooldownGameHours = static_cast<int>(
                ini.GetLongValue("Beats", "iVisitSenderCooldownGameHours", dst.visitSenderCooldownGameHours));

            dst.visitApproachTimeoutSeconds = static_cast<int>(
                ini.GetLongValue("Beats", "iVisitApproachTimeoutSeconds", dst.visitApproachTimeoutSeconds));
            dst.visitSalutationApproachDistanceUnits = static_cast<int>(ini.GetLongValue(
                "Beats", "iVisitSalutationApproachDistanceUnits", dst.visitSalutationApproachDistanceUnits));
            dst.visitReEngageApproachDistanceUnits = static_cast<int>(ini.GetLongValue(
                "Beats", "iVisitReEngageApproachDistanceUnits", dst.visitReEngageApproachDistanceUnits));
            dst.visitPollGateTickSeconds =
                static_cast<int>(ini.GetLongValue("Beats", "iVisitPollGateTickSeconds", dst.visitPollGateTickSeconds));
            dst.visitPollTurnCountThreshold = static_cast<int>(
                ini.GetLongValue("Beats", "iVisitPollTurnCountThreshold", dst.visitPollTurnCountThreshold));
            dst.visitPollSilenceRealSeconds = static_cast<int>(
                ini.GetLongValue("Beats", "iVisitPollSilenceRealSeconds", dst.visitPollSilenceRealSeconds));
            dst.visitPollMaxIntervalGameMinutes = static_cast<int>(
                ini.GetLongValue("Beats", "iVisitPollMaxIntervalGameMinutes", dst.visitPollMaxIntervalGameMinutes));
            dst.visitConclusionPollMaxConsecutiveFailures = static_cast<int>(ini.GetLongValue(
                "Beats", "iVisitConclusionPollMaxConsecutiveFailures", dst.visitConclusionPollMaxConsecutiveFailures));
            dst.visitMaxIgnoreNudges =
                static_cast<int>(ini.GetLongValue("Beats", "iVisitMaxIgnoreNudges", dst.visitMaxIgnoreNudges));
            dst.visitOnHoldCombatMaxSeconds = static_cast<int>(
                ini.GetLongValue("Beats", "iVisitOnHoldCombatMaxSeconds", dst.visitOnHoldCombatMaxSeconds));
            dst.visitValedictionDwellSeconds = static_cast<int>(
                ini.GetLongValue("Beats", "iVisitValedictionDwellSeconds", dst.visitValedictionDwellSeconds));
            dst.visitReturnHomeExitDistanceUnits = static_cast<int>(
                ini.GetLongValue("Beats", "iVisitReturnHomeExitDistanceUnits", dst.visitReturnHomeExitDistanceUnits));
            dst.visitReturnHomeTimeoutSeconds = static_cast<int>(
                ini.GetLongValue("Beats", "iVisitReturnHomeTimeoutSeconds", dst.visitReturnHomeTimeoutSeconds));
        }
    } // namespace

    void Load()
    {
        // Reset to defaults first so repeated Load() calls (should that ever
        // happen) produce a deterministic result rather than stacking values.
        g_config = Config{};

        CSimpleIniA plugin;
        plugin.SetUnicode();
        if (plugin.LoadFile(kPluginIniPath) >= 0) {
            ReadIniInto(plugin, g_config);
            logger::info("Settings: loaded from {}", kPluginIniPath);
        } else {
            logger::info("Settings: no plugin INI at {}; using defaults", kPluginIniPath);
        }

        // Apply the MCM Helper-written override on top of the plugin INI,
        // if the file exists. Silent no-op otherwise (first-run before the
        // player has opened the MCM page). The universal cascade honors
        // *every* recognized key in the MCM INI — not just [Dashboard].
        ApplyMcmOverride();

        if (g_config.debugMode) {
            logger::info("Settings: debug mode ON");
        }
        if (g_config.traceMode) {
            logger::info("Settings: trace mode ON");
        }
        logger::info("Settings: dashboard hotkey DXSC={} mods={}",
                     g_config.dashboardHotkeyDXSC,
                     static_cast<int>(g_config.dashboardHotkeyModifiers));
    }

    const Config& Get()
    {
        return g_config;
    }

    void ApplyMcmOverride()
    {
        CSimpleIniA ini;
        ini.SetUnicode();
        if (ini.LoadFile(kMcmIniPath) < 0) {
            // Fresh install, or player hasn't opened the MCM page yet.
            // Leave the plugin-INI-loaded values in place.
            return;
        }
        ReadIniInto(ini, g_config);
        logger::info("Settings: MCM overrides applied from {}", kMcmIniPath);
    }

    void WriteMcmOverride(const McmOverride& mutations)
    {
        CSimpleIniA ini;
        ini.SetUnicode();
        // Loading a non-existent file returns an error but leaves `ini`
        // empty, which is exactly what we want for the "first write"
        // case — SaveFile creates the file with just our keys.
        (void)ini.LoadFile(kMcmIniPath);

        if (mutations.debugMode) {
            ini.SetBoolValue("General", "bDebugMode", *mutations.debugMode);
            g_config.debugMode = *mutations.debugMode;
            logger::info("Settings: MCM override write: bDebugMode={}", *mutations.debugMode ? 1 : 0);
        }
        if (mutations.traceMode) {
            ini.SetBoolValue("General", "bTraceMode", *mutations.traceMode);
            g_config.traceMode = *mutations.traceMode;
            logger::info("Settings: MCM override write: bTraceMode={}", *mutations.traceMode ? 1 : 0);
        }
        if (mutations.tickEnabled) {
            ini.SetBoolValue("Director", "bTickEnabled", *mutations.tickEnabled);
            g_config.tickEnabled = *mutations.tickEnabled;
            logger::info("Settings: MCM override write: bTickEnabled={}", *mutations.tickEnabled ? 1 : 0);
        }
        if (mutations.tickIntervalSeconds) {
            ini.SetLongValue("Director", "iTickIntervalSeconds", *mutations.tickIntervalSeconds);
            g_config.tickIntervalSeconds = *mutations.tickIntervalSeconds;
            logger::info("Settings: MCM override write: iTickIntervalSeconds={}", *mutations.tickIntervalSeconds);
        }
        if (mutations.minPhaseDurationSeconds) {
            ini.SetLongValue("Director", "iMinPhaseDurationSeconds", *mutations.minPhaseDurationSeconds);
            g_config.minPhaseDurationSeconds = *mutations.minPhaseDurationSeconds;
            logger::info("Settings: MCM override write: iMinPhaseDurationSeconds={}",
                         *mutations.minPhaseDurationSeconds);
        }
        if (mutations.idealDurationExposition) {
            ini.SetLongValue("Director", "iIdealDurationExposition", *mutations.idealDurationExposition);
            g_config.idealDurationExposition = *mutations.idealDurationExposition;
            logger::info("Settings: MCM override write: iIdealDurationExposition={}",
                         *mutations.idealDurationExposition);
        }
        if (mutations.idealDurationRisingAction) {
            ini.SetLongValue("Director", "iIdealDurationRisingAction", *mutations.idealDurationRisingAction);
            g_config.idealDurationRisingAction = *mutations.idealDurationRisingAction;
            logger::info("Settings: MCM override write: iIdealDurationRisingAction={}",
                         *mutations.idealDurationRisingAction);
        }
        if (mutations.idealDurationClimax) {
            ini.SetLongValue("Director", "iIdealDurationClimax", *mutations.idealDurationClimax);
            g_config.idealDurationClimax = *mutations.idealDurationClimax;
            logger::info("Settings: MCM override write: iIdealDurationClimax={}", *mutations.idealDurationClimax);
        }
        if (mutations.idealDurationFallingAction) {
            ini.SetLongValue("Director", "iIdealDurationFallingAction", *mutations.idealDurationFallingAction);
            g_config.idealDurationFallingAction = *mutations.idealDurationFallingAction;
            logger::info("Settings: MCM override write: iIdealDurationFallingAction={}",
                         *mutations.idealDurationFallingAction);
        }
        if (mutations.idealDurationResolution) {
            ini.SetLongValue("Director", "iIdealDurationResolution", *mutations.idealDurationResolution);
            g_config.idealDurationResolution = *mutations.idealDurationResolution;
            logger::info("Settings: MCM override write: iIdealDurationResolution={}",
                         *mutations.idealDurationResolution);
        }
        if (mutations.dashboardHotkeyDXSC) {
            ini.SetLongValue("Dashboard", "iHotkeyDXSC", *mutations.dashboardHotkeyDXSC);
            g_config.dashboardHotkeyDXSC = *mutations.dashboardHotkeyDXSC;
            logger::info("Settings: MCM override write: iHotkeyDXSC={}", *mutations.dashboardHotkeyDXSC);
        }
        // The three hotkey modifier bools are written independently but
        // reassembled into the runtime bitmask together — after all three
        // are processed we rebuild `dashboardHotkeyModifiers` from the
        // final in-memory bool state, so a call that only sets a subset
        // still yields a correct combined bitmask.
        if (mutations.hotkeyShift) {
            ini.SetBoolValue("Dashboard", "bHotkeyShift", *mutations.hotkeyShift);
            logger::info("Settings: MCM override write: bHotkeyShift={}", *mutations.hotkeyShift ? 1 : 0);
        }
        if (mutations.hotkeyCtrl) {
            ini.SetBoolValue("Dashboard", "bHotkeyCtrl", *mutations.hotkeyCtrl);
            logger::info("Settings: MCM override write: bHotkeyCtrl={}", *mutations.hotkeyCtrl ? 1 : 0);
        }
        if (mutations.hotkeyAlt) {
            ini.SetBoolValue("Dashboard", "bHotkeyAlt", *mutations.hotkeyAlt);
            logger::info("Settings: MCM override write: bHotkeyAlt={}", *mutations.hotkeyAlt ? 1 : 0);
        }
        if (mutations.hotkeyShift || mutations.hotkeyCtrl || mutations.hotkeyAlt) {
            std::uint8_t mods = 0;
            const bool shift =
                mutations.hotkeyShift ? *mutations.hotkeyShift : (g_config.dashboardHotkeyModifiers & kModShift) != 0;
            const bool ctrl =
                mutations.hotkeyCtrl ? *mutations.hotkeyCtrl : (g_config.dashboardHotkeyModifiers & kModCtrl) != 0;
            const bool alt =
                mutations.hotkeyAlt ? *mutations.hotkeyAlt : (g_config.dashboardHotkeyModifiers & kModAlt) != 0;
            if (shift)
                mods |= kModShift;
            if (ctrl)
                mods |= kModCtrl;
            if (alt)
                mods |= kModAlt;
            g_config.dashboardHotkeyModifiers = mods;
        }

        if (ini.SaveFile(kMcmIniPath) < 0) {
            logger::warn("Settings: failed to save MCM override to {}", kMcmIniPath);
        }
    }
} // namespace NarrativeEngine::Settings
