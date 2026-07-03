#include <Settings.h>

#include <logger.h>

#include <SimpleIni.h>

namespace NarrativeEngine::Settings
{
    namespace
    {
        Config g_config{};

        constexpr const char* kPluginIniPath = "Data/SKSE/Plugins/NarrativeEngine.ini";
        constexpr const char* kMcmIniPath    = "Data/MCM/Settings/NarrativeEngine.ini";

        // Try to load the plugin INI; return true on success (file present
        // and parsed). Missing keys retain whatever value g_config already
        // holds — i.e. the baked-in default seeded by Load() before this call.
        bool LoadPluginIni()
        {
            CSimpleIniA ini;
            ini.SetUnicode();
            const SI_Error err = ini.LoadFile(kPluginIniPath);
            if (err < 0) {
                return false;
            }

            g_config.debugMode =
                ini.GetBoolValue("General", "bDebugMode", g_config.debugMode);

            g_config.tickIntervalSeconds = static_cast<int>(
                ini.GetLongValue("Director", "iTickIntervalSeconds", g_config.tickIntervalSeconds));
            g_config.decisionLogMaxEntries = static_cast<int>(
                ini.GetLongValue("Director", "iDecisionLogMaxEntries", g_config.decisionLogMaxEntries));
            g_config.decisionLogTailSizeForPrompt = static_cast<int>(
                ini.GetLongValue("Director", "iDecisionLogTailSizeForPrompt",
                                 g_config.decisionLogTailSizeForPrompt));
            g_config.skyrimNetEventTailSizeForPrompt = static_cast<int>(
                ini.GetLongValue("Director", "iSkyrimNetEventTailSizeForPrompt",
                                 g_config.skyrimNetEventTailSizeForPrompt));

            g_config.advanceThresholdExposition = static_cast<int>(
                ini.GetLongValue("Director", "iAdvanceThresholdExposition",
                                 g_config.advanceThresholdExposition));
            g_config.advanceThresholdRisingAction = static_cast<int>(
                ini.GetLongValue("Director", "iAdvanceThresholdRisingAction",
                                 g_config.advanceThresholdRisingAction));
            g_config.advanceThresholdClimax = static_cast<int>(
                ini.GetLongValue("Director", "iAdvanceThresholdClimax",
                                 g_config.advanceThresholdClimax));
            g_config.advanceThresholdFallingAction = static_cast<int>(
                ini.GetLongValue("Director", "iAdvanceThresholdFallingAction",
                                 g_config.advanceThresholdFallingAction));
            g_config.advanceThresholdResolution = static_cast<int>(
                ini.GetLongValue("Director", "iAdvanceThresholdResolution",
                                 g_config.advanceThresholdResolution));

            g_config.idealDurationExposition = static_cast<int>(
                ini.GetLongValue("Director", "iIdealDurationExposition",
                                 g_config.idealDurationExposition));
            g_config.idealDurationRisingAction = static_cast<int>(
                ini.GetLongValue("Director", "iIdealDurationRisingAction",
                                 g_config.idealDurationRisingAction));
            g_config.idealDurationClimax = static_cast<int>(
                ini.GetLongValue("Director", "iIdealDurationClimax",
                                 g_config.idealDurationClimax));
            g_config.idealDurationFallingAction = static_cast<int>(
                ini.GetLongValue("Director", "iIdealDurationFallingAction",
                                 g_config.idealDurationFallingAction));
            g_config.idealDurationResolution = static_cast<int>(
                ini.GetLongValue("Director", "iIdealDurationResolution",
                                 g_config.idealDurationResolution));

            g_config.actionCooldownSeconds = static_cast<int>(
                ini.GetLongValue("Director", "iActionCooldownSeconds",
                                 g_config.actionCooldownSeconds));
            g_config.actionRepetitionWindowSeconds = static_cast<int>(
                ini.GetLongValue("Director", "iActionRepetitionWindowSeconds",
                                 g_config.actionRepetitionWindowSeconds));
            g_config.actionStaleLockTimeoutSeconds = static_cast<int>(
                ini.GetLongValue("Director", "iActionStaleLockTimeoutSeconds",
                                 g_config.actionStaleLockTimeoutSeconds));

            g_config.letterMinSenderCandidates = static_cast<int>(
                ini.GetLongValue("Director", "iLetterMinSenderCandidates",
                                 g_config.letterMinSenderCandidates));

            g_config.doNotDisturbCellEDIDsCSV = ini.GetValue(
                "AlphaCanon", "sDoNotDisturbCellEDIDsCSV",
                g_config.doNotDisturbCellEDIDsCSV.c_str());

            g_config.dashboardHotkeyVK = static_cast<int>(
                ini.GetLongValue("Dashboard", "iHotkeyVK", g_config.dashboardHotkeyVK));
            g_config.dashboardHotkeyModifiers = static_cast<std::uint8_t>(
                ini.GetLongValue("Dashboard", "iHotkeyModifiers", g_config.dashboardHotkeyModifiers));

            g_config.combatEventsHitRadiusUnits = static_cast<int>(
                ini.GetLongValue("CombatEvents", "iHitRadiusUnits",
                                 g_config.combatEventsHitRadiusUnits));
            g_config.combatEventsMaxStored = static_cast<int>(
                ini.GetLongValue("CombatEvents", "iMaxStored",
                                 g_config.combatEventsMaxStored));

            g_config.ambushDefaultBanditCount = static_cast<int>(
                ini.GetLongValue("Actions", "iAmbushDefaultBanditCount",
                                 g_config.ambushDefaultBanditCount));
            g_config.ambushDefaultSpawnDistanceUnits = static_cast<int>(
                ini.GetLongValue("Actions", "iAmbushDefaultSpawnDistanceUnits",
                                 g_config.ambushDefaultSpawnDistanceUnits));
            g_config.ambushMinBanditCount = static_cast<int>(
                ini.GetLongValue("Actions", "iAmbushMinBanditCount",
                                 g_config.ambushMinBanditCount));
            g_config.ambushMaxBanditCount = static_cast<int>(
                ini.GetLongValue("Actions", "iAmbushMaxBanditCount",
                                 g_config.ambushMaxBanditCount));
            g_config.ambushMinSpawnDistanceUnits = static_cast<int>(
                ini.GetLongValue("Actions", "iAmbushMinSpawnDistanceUnits",
                                 g_config.ambushMinSpawnDistanceUnits));
            g_config.ambushMaxSpawnDistanceUnits = static_cast<int>(
                ini.GetLongValue("Actions", "iAmbushMaxSpawnDistanceUnits",
                                 g_config.ambushMaxSpawnDistanceUnits));
            g_config.ambushPerActionCooldownGameHours = static_cast<int>(
                ini.GetLongValue("Actions", "iAmbushPerActionCooldownGameHours",
                                 g_config.ambushPerActionCooldownGameHours));

            g_config.letterContentMinWords = static_cast<int>(
                ini.GetLongValue("Actions", "iLetterContentMinWords",
                                 g_config.letterContentMinWords));
            g_config.letterContentMaxWords = static_cast<int>(
                ini.GetLongValue("Actions", "iLetterContentMaxWords",
                                 g_config.letterContentMaxWords));
            g_config.letterPoolSize = static_cast<int>(
                ini.GetLongValue("Actions", "iLetterPoolSize",
                                 g_config.letterPoolSize));
            g_config.letterDispatchVerifyDelaySeconds = static_cast<int>(
                ini.GetLongValue("Actions", "iLetterDispatchVerifyDelaySeconds",
                                 g_config.letterDispatchVerifyDelaySeconds));
            g_config.letterPendingDeliveryTimeoutSeconds = static_cast<int>(
                ini.GetLongValue("Actions", "iLetterPendingDeliveryTimeoutSeconds",
                                 g_config.letterPendingDeliveryTimeoutSeconds));
            g_config.letterActionCooldownGameHours = static_cast<int>(
                ini.GetLongValue("Actions", "iLetterActionCooldownGameHours",
                                 g_config.letterActionCooldownGameHours));
            g_config.letterSenderCooldownGameHours = static_cast<int>(
                ini.GetLongValue("Actions", "iLetterSenderCooldownGameHours",
                                 g_config.letterSenderCooldownGameHours));

            g_config.letterPoolEvictionLogVerbosity = static_cast<int>(
                ini.GetLongValue("LetterPool", "iLetterPoolEvictionLogVerbosity",
                                 g_config.letterPoolEvictionLogVerbosity));

            return true;
        }

        // MCM Helper writes its managed values here when the player rebinds
        // the dashboard hotkey via the MCM. Only the [Dashboard] keys are
        // read — every other setting stays as the plugin INI / default left
        // it. Returns true if the file was found and parsed.
        bool ApplyMcmOverride()
        {
            CSimpleIniA ini;
            ini.SetUnicode();
            const SI_Error err = ini.LoadFile(kMcmIniPath);
            if (err < 0) {
                return false;
            }

            g_config.dashboardHotkeyVK = static_cast<int>(
                ini.GetLongValue("Dashboard", "iHotkeyVK", g_config.dashboardHotkeyVK));
            g_config.dashboardHotkeyModifiers = static_cast<std::uint8_t>(
                ini.GetLongValue("Dashboard", "iHotkeyModifiers", g_config.dashboardHotkeyModifiers));

            return true;
        }
    }

    void Load()
    {
        // Reset to defaults first so repeated Load() calls (should that ever
        // happen) produce a deterministic result rather than stacking values.
        g_config = Config{};

        if (LoadPluginIni()) {
            logger::info("Settings: loaded from {}", kPluginIniPath);
        } else {
            logger::info("Settings: no plugin INI at {}; using defaults", kPluginIniPath);
        }

        if (ApplyMcmOverride()) {
            logger::info("Settings: applied MCM override from {}", kMcmIniPath);
        }

        if (g_config.debugMode) {
            logger::info("Settings: debug mode ON");
        }
        logger::info("Settings: dashboard hotkey VK={} mods={}",
                     g_config.dashboardHotkeyVK,
                     static_cast<int>(g_config.dashboardHotkeyModifiers));
    }

    const Config& Get()
    {
        return g_config;
    }
}
