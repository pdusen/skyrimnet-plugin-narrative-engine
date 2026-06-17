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

            g_config.doNotDisturbCellEDIDsCSV = ini.GetValue(
                "AlphaCanon", "sDoNotDisturbCellEDIDsCSV",
                g_config.doNotDisturbCellEDIDsCSV.c_str());

            g_config.dashboardHotkeyVK = static_cast<int>(
                ini.GetLongValue("Dashboard", "iHotkeyVK", g_config.dashboardHotkeyVK));
            g_config.dashboardHotkeyModifiers = static_cast<std::uint8_t>(
                ini.GetLongValue("Dashboard", "iHotkeyModifiers", g_config.dashboardHotkeyModifiers));

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
