#include <Settings.h>

#include <logger.h>

#include <SimpleIni.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace NarrativeEngine::Settings
{
    namespace
    {
        Config g_config{};

        // Parsed form of Config::spellNameBlocklist. Rebuilt by
        // RebuildSpellNameBlocklistSet after every Load / ApplyMcmOverride;
        // read-only outside that path so lookups from engine sink threads
        // (CombatEventLog HitSink) don't need locking.
        std::unordered_set<std::string> g_spellNameBlocklistSet;

        constexpr const char* kPluginIniPath = "Data/SKSE/Plugins/NarrativeEngine.ini";
        constexpr const char* kMcmIniPath = "Data/MCM/Settings/NarrativeEngine.ini";
        // Companion files that MCM Helper reads to populate the MCM page.
        // Traced at load time so a missing / mis-installed MCM asset shows
        // up in the plugin log without needing Papyrus.log or MCM Helper's
        // own logs to diagnose the "MCM page doesn't appear" report.
        constexpr const char* kMcmConfigJsonPath = "Data/MCM/Config/NarrativeEngine/config.json";
        constexpr const char* kMcmHelperDllPath = "Data/SKSE/Plugins/MCMHelper.dll";

        std::string ToLowerCopy(std::string_view s)
        {
            std::string r;
            r.reserve(s.size());
            for (char c : s) {
                r += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            }
            return r;
        }

        std::string_view TrimAsciiSpace(std::string_view s)
        {
            std::size_t start = 0;
            while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) {
                ++start;
            }
            std::size_t end = s.size();
            while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
                --end;
            }
            return s.substr(start, end - start);
        }

        // Split Config::spellNameBlocklist on ';', trim each piece,
        // lowercase it, and repopulate g_spellNameBlocklistSet. Called
        // after every path that mutates g_config.spellNameBlocklist.
        void RebuildSpellNameBlocklistSet()
        {
            g_spellNameBlocklistSet.clear();
            std::string_view raw{g_config.spellNameBlocklist};
            std::size_t pos = 0;
            while (pos <= raw.size()) {
                const auto sep = raw.find(';', pos);
                const auto end = (sep == std::string_view::npos) ? raw.size() : sep;
                const std::string_view piece = TrimAsciiSpace(raw.substr(pos, end - pos));
                if (!piece.empty()) {
                    g_spellNameBlocklistSet.insert(ToLowerCopy(piece));
                }
                if (sep == std::string_view::npos) {
                    break;
                }
                pos = sep + 1;
            }
            logger::info("Settings: spell-name blocklist has {} entries", g_spellNameBlocklistSet.size());
        }

        // Sync spdlog's level filter to the current traceMode setting.
        // `logger.h::SetupLog` initializes at trace level (so startup
        // probes always land), then this pulls the level down to debug
        // whenever traceMode goes false — silencing both gated and
        // ungated `logger::trace(...)` calls (e.g., MCMEventSink's
        // per-ModCallback line, PrismaUI / DashboardUIManager sink
        // chatter) without touching the callsites. Called from every
        // path that can flip `g_config.traceMode`.
        void ApplyLogLevelForTraceMode()
        {
            spdlog::set_level(g_config.traceMode ? spdlog::level::trace : spdlog::level::debug);
        }

        // Emit one trace line summarizing a file's presence on disk.
        // Absolute path is resolved (so the log tells you EXACTLY where
        // the plugin looked, past MO2 VFS quirks), then existence, size,
        // and last-write time are logged. Missing files are logged
        // explicitly rather than silently — the whole point is diagnosing
        // "why isn't X being loaded?".
        void TraceFilePresence(const char* label, const char* relPath)
        {
            std::error_code ec;
            const std::filesystem::path rel{relPath};
            const auto abs = std::filesystem::absolute(rel, ec);
            const std::string absStr = ec ? std::string{relPath} : abs.string();
            const bool exists = !ec && std::filesystem::exists(abs, ec);
            if (!exists) {
                logger::trace("Settings[trace]: {} MISSING at '{}' (rel='{}')", label, absStr, relPath);
                return;
            }
            std::error_code szEc;
            const auto sz = std::filesystem::file_size(abs, szEc);
            std::error_code tsEc;
            const auto lwt = std::filesystem::last_write_time(abs, tsEc);
            // last_write_time is a file_clock time_point; convert to a
            // wall-clock time_t for readable logging (portable-enough on
            // MSVC where file_clock == system_clock for practical purposes).
            std::time_t mtime = 0;
            if (!tsEc) {
                const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    lwt - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
                mtime = std::chrono::system_clock::to_time_t(sctp);
            }
            logger::trace("Settings[trace]: {} present at '{}' (size={} bytes, mtime_epoch={})",
                          label,
                          absStr,
                          szEc ? 0ull : static_cast<unsigned long long>(sz),
                          static_cast<long long>(mtime));
        }

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
            dst.spellNameBlocklist =
                ini.GetValue("CombatEvents", "sSpellNameBlocklist", dst.spellNameBlocklist.c_str());

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

            dst.enableNpcLetter = ini.GetBoolValue("Beats", "bEnableNpcLetter", dst.enableNpcLetter);

            dst.letterContentMinWords =
                static_cast<int>(ini.GetLongValue("Beats", "iLetterContentMinWords", dst.letterContentMinWords));
            dst.letterContentMaxWords =
                static_cast<int>(ini.GetLongValue("Beats", "iLetterContentMaxWords", dst.letterContentMaxWords));
            dst.letterMemoryImportanceThreshold = static_cast<float>(
                ini.GetDoubleValue("Beats", "fLetterMemoryImportanceThreshold", dst.letterMemoryImportanceThreshold));
            dst.letterComposeMemoryRenderCap = static_cast<int>(
                ini.GetLongValue("Beats", "iLetterComposeMemoryRenderCap", dst.letterComposeMemoryRenderCap));
            dst.letterComposeDialogueRenderCap = static_cast<int>(
                ini.GetLongValue("Beats", "iLetterComposeDialogueRenderCap", dst.letterComposeDialogueRenderCap));
            dst.actionSelectEventRenderCap = static_cast<int>(
                ini.GetLongValue("Beats", "iActionSelectEventRenderCap", dst.actionSelectEventRenderCap));
            dst.actionSelectLetterMemoryRenderCap = static_cast<int>(
                ini.GetLongValue("Beats", "iActionSelectLetterMemoryRenderCap", dst.actionSelectLetterMemoryRenderCap));
            dst.actionSelectVisitMemoryRenderCap = static_cast<int>(
                ini.GetLongValue("Beats", "iActionSelectVisitMemoryRenderCap", dst.actionSelectVisitMemoryRenderCap));
            // Enforce per-setting floors on the compose caps regardless
            // of source (plugin INI, MCM override, or a hand-edited file
            // that bypassed the dashboard sliders). Sub-floor values
            // produce threadbare letters — the compose prompt loses
            // enough context that the LLM fabricates instead of drawing
            // on real sender history. Memory floor is 3 (default is 6);
            // dialogue floor is 5 (default is 25).
            if (dst.letterComposeMemoryRenderCap < 3)
                dst.letterComposeMemoryRenderCap = 3;
            if (dst.letterComposeDialogueRenderCap < 5)
                dst.letterComposeDialogueRenderCap = 5;
            // Same floor discipline for the action-select caps. Event
            // floor is 3 (default 10). Letter/visit memory floors are
            // 3 (default 6) — matches the compose-side letter memory
            // floor for consistency.
            if (dst.actionSelectEventRenderCap < 3)
                dst.actionSelectEventRenderCap = 3;
            if (dst.actionSelectLetterMemoryRenderCap < 3)
                dst.actionSelectLetterMemoryRenderCap = 3;
            if (dst.actionSelectVisitMemoryRenderCap < 3)
                dst.actionSelectVisitMemoryRenderCap = 3;
            // Floors for the story-eval prompt tails. Recent-events
            // floor is 5 (default 40) — the tension evaluator leans
            // hard on very recent world state, so dropping too low
            // starves it. Decision-log tail floor is 3 (default 10) —
            // the LLM benefits from a few of its own recent judgements
            // for continuity.
            if (dst.skyrimNetEventTailSizeForPrompt < 5)
                dst.skyrimNetEventTailSizeForPrompt = 5;
            if (dst.decisionLogTailSizeForPrompt < 3)
                dst.decisionLogTailSizeForPrompt = 3;
            // Floors for the visit compose caps. Same rationale as the
            // letter-compose floors — memory 3 (default 6), dialogue
            // 5 (default 25).
            if (dst.visitComposeMemoryRenderCap < 3)
                dst.visitComposeMemoryRenderCap = 3;
            if (dst.visitComposeDialogueRenderCap < 5)
                dst.visitComposeDialogueRenderCap = 5;
            // Visit-conclusion poll floor is 3 (default 8) — below that
            // the poll can't see enough back-and-forth to distinguish
            // "just delivered opener" from "beat has landed."
            if (dst.visitConclusionPollRecentLinesRenderCap < 3)
                dst.visitConclusionPollRecentLinesRenderCap = 3;
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
            dst.visitComposeMemoryRenderCap = static_cast<int>(
                ini.GetLongValue("Beats", "iVisitComposeMemoryRenderCap", dst.visitComposeMemoryRenderCap));
            dst.visitComposeDialogueRenderCap = static_cast<int>(
                ini.GetLongValue("Beats", "iVisitComposeDialogueRenderCap", dst.visitComposeDialogueRenderCap));
            dst.visitConclusionPollRecentLinesRenderCap = static_cast<int>(ini.GetLongValue(
                "Beats", "iVisitConclusionPollRecentLinesRenderCap", dst.visitConclusionPollRecentLinesRenderCap));
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

            // --- AmbushBeat ---

            dst.enableAmbush = ini.GetBoolValue("Beats", "bEnableAmbush", dst.enableAmbush);

            dst.ambushDefaultAttackerCount = static_cast<int>(
                ini.GetLongValue("Beats", "iAmbushDefaultAttackerCount", dst.ambushDefaultAttackerCount));
            dst.ambushMinAttackerCount =
                static_cast<int>(ini.GetLongValue("Beats", "iAmbushMinAttackerCount", dst.ambushMinAttackerCount));
            dst.ambushMaxAttackerCount =
                static_cast<int>(ini.GetLongValue("Beats", "iAmbushMaxAttackerCount", dst.ambushMaxAttackerCount));

            dst.ambushDefaultSpawnDistanceUnits = static_cast<int>(
                ini.GetLongValue("Beats", "iAmbushDefaultSpawnDistanceUnits", dst.ambushDefaultSpawnDistanceUnits));
            dst.ambushMinSpawnDistanceUnits = static_cast<int>(
                ini.GetLongValue("Beats", "iAmbushMinSpawnDistanceUnits", dst.ambushMinSpawnDistanceUnits));
            dst.ambushMaxSpawnDistanceUnits = static_cast<int>(
                ini.GetLongValue("Beats", "iAmbushMaxSpawnDistanceUnits", dst.ambushMaxSpawnDistanceUnits));

            dst.ambushEngageDistanceUnits = static_cast<int>(
                ini.GetLongValue("Beats", "iAmbushEngageDistanceUnits", dst.ambushEngageDistanceUnits));
            dst.ambushAbandonDistanceUnits = static_cast<int>(
                ini.GetLongValue("Beats", "iAmbushAbandonDistanceUnits", dst.ambushAbandonDistanceUnits));
            dst.ambushMaxDurationSeconds =
                static_cast<int>(ini.GetLongValue("Beats", "iAmbushMaxDurationSeconds", dst.ambushMaxDurationSeconds));
            dst.ambushPerBeatCooldownGameHours = static_cast<int>(
                ini.GetLongValue("Beats", "iAmbushPerBeatCooldownGameHours", dst.ambushPerBeatCooldownGameHours));

            // Ambush clamps, applied on the read path rather than at use
            // sites so every consumer sees a coherent range (the same
            // discipline the compose-cap floors above follow).
            //
            // The attacker-count ceiling is the load-bearing one: eight
            // Attacker0N aliases exist on _ne_AmbushQuest, so a request
            // for nine has nowhere to go. Clamp to the authored slot
            // count and shout, because an INI asking for more than the
            // ESP can hold is an author error worth seeing.
            if (dst.ambushMaxAttackerCount > kAmbushAttackerSlotCount) {
                logger::warn("Settings: iAmbushMaxAttackerCount={} exceeds the {} authored attacker "
                             "alias slots on _ne_AmbushQuest; clamping to {}.",
                             dst.ambushMaxAttackerCount,
                             kAmbushAttackerSlotCount,
                             kAmbushAttackerSlotCount);
                dst.ambushMaxAttackerCount = kAmbushAttackerSlotCount;
            }
            if (dst.ambushMinAttackerCount < 1)
                dst.ambushMinAttackerCount = 1;
            if (dst.ambushMaxAttackerCount < dst.ambushMinAttackerCount)
                dst.ambushMaxAttackerCount = dst.ambushMinAttackerCount;
            if (dst.ambushDefaultAttackerCount < dst.ambushMinAttackerCount)
                dst.ambushDefaultAttackerCount = dst.ambushMinAttackerCount;
            if (dst.ambushDefaultAttackerCount > dst.ambushMaxAttackerCount)
                dst.ambushDefaultAttackerCount = dst.ambushMaxAttackerCount;

            // Distance band. A zero or negative floor would let the
            // spawn search place attackers on top of the player, which
            // reads as a teleport rather than an ambush.
            if (dst.ambushMinSpawnDistanceUnits < 1)
                dst.ambushMinSpawnDistanceUnits = 1;
            if (dst.ambushMaxSpawnDistanceUnits < dst.ambushMinSpawnDistanceUnits)
                dst.ambushMaxSpawnDistanceUnits = dst.ambushMinSpawnDistanceUnits;
            if (dst.ambushDefaultSpawnDistanceUnits < dst.ambushMinSpawnDistanceUnits)
                dst.ambushDefaultSpawnDistanceUnits = dst.ambushMinSpawnDistanceUnits;
            if (dst.ambushDefaultSpawnDistanceUnits > dst.ambushMaxSpawnDistanceUnits)
                dst.ambushDefaultSpawnDistanceUnits = dst.ambushMaxSpawnDistanceUnits;

            // Abandon distance below the spawn distance would abandon the
            // beat on the tick after it spawns.
            if (dst.ambushAbandonDistanceUnits <= dst.ambushMaxSpawnDistanceUnits)
                dst.ambushAbandonDistanceUnits = dst.ambushMaxSpawnDistanceUnits + 1;
            if (dst.ambushEngageDistanceUnits < 1)
                dst.ambushEngageDistanceUnits = 1;
            if (dst.ambushMaxDurationSeconds < 1)
                dst.ambushMaxDurationSeconds = 1;
            if (dst.ambushPerBeatCooldownGameHours < 0)
                dst.ambushPerBeatCooldownGameHours = 0;
        }
    } // namespace

    void Load()
    {
        // Reset to defaults first so repeated Load() calls (should that ever
        // happen) produce a deterministic result rather than stacking values.
        g_config = Config{};

        // Startup presence trace for the four MCM/dashboard files that
        // most commonly cause "MCM doesn't appear" or "hotkey doesn't
        // work" bug reports. spdlog is initialized at trace level in
        // `logger.h::SetupLog` (which runs before this function), so
        // these one-shot probes land in the log even on a cold boot
        // where `bTraceMode=false`. The level is pulled down to `debug`
        // at the end of this function via `ApplyLogLevelForTraceMode`,
        // silencing subsequent trace chatter until the player flips
        // traceMode on.
        TraceFilePresence("plugin INI", kPluginIniPath);
        TraceFilePresence("MCM overrides INI", kMcmIniPath);
        TraceFilePresence("MCM Helper config JSON", kMcmConfigJsonPath);
        TraceFilePresence("MCM Helper DLL", kMcmHelperDllPath);

        CSimpleIniA plugin;
        plugin.SetUnicode();
        const SI_Error pluginRc = plugin.LoadFile(kPluginIniPath);
        if (pluginRc >= 0) {
            ReadIniInto(plugin, g_config);
            logger::info("Settings: loaded from {}", kPluginIniPath);
            logger::trace("Settings[trace]: plugin INI parse ok (SimpleIni rc={}); "
                          "post-load hotkey DXSC={} mods={} debugMode={} traceMode={}",
                          static_cast<int>(pluginRc),
                          g_config.dashboardHotkeyDXSC,
                          static_cast<int>(g_config.dashboardHotkeyModifiers),
                          g_config.debugMode ? 1 : 0,
                          g_config.traceMode ? 1 : 0);
        } else {
            logger::info("Settings: no plugin INI at {}; using defaults", kPluginIniPath);
            logger::trace("Settings[trace]: plugin INI load failed (SimpleIni rc={}); Config baseline retained",
                          static_cast<int>(pluginRc));
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
        // Full post-cascade summary — the definitive "what the plugin
        // actually thinks the settings are" line for diagnostics. If the
        // player reports "I set X in the MCM but it isn't taking effect,"
        // this is the line to check.
        logger::trace("Settings[trace]: post-cascade final: hotkey_dxsc={} hotkey_mods=0x{:02X} "
                      "(shift={} ctrl={} alt={}) debug={} trace={} tick_enabled={} tick_interval_s={}",
                      g_config.dashboardHotkeyDXSC,
                      static_cast<int>(g_config.dashboardHotkeyModifiers),
                      (g_config.dashboardHotkeyModifiers & kModShift) ? 1 : 0,
                      (g_config.dashboardHotkeyModifiers & kModCtrl) ? 1 : 0,
                      (g_config.dashboardHotkeyModifiers & kModAlt) ? 1 : 0,
                      g_config.debugMode ? 1 : 0,
                      g_config.traceMode ? 1 : 0,
                      g_config.tickEnabled ? 1 : 0,
                      g_config.tickIntervalSeconds);
        ApplyLogLevelForTraceMode();
        RebuildSpellNameBlocklistSet();
    }

    const Config& Get()
    {
        return g_config;
    }

    void ApplyMcmOverride()
    {
        CSimpleIniA ini;
        ini.SetUnicode();
        const SI_Error rc = ini.LoadFile(kMcmIniPath);
        if (rc < 0) {
            // Fresh install, or player hasn't opened the MCM page yet.
            // Leave the plugin-INI-loaded values in place. Explicit trace
            // so this well-known "MCM never opened" state doesn't look
            // like the same log as "MCM loaded but wrote nothing".
            std::error_code ec;
            const auto abs = std::filesystem::absolute(std::filesystem::path{kMcmIniPath}, ec);
            logger::trace("Settings[trace]: ApplyMcmOverride NO-OP — file absent or unreadable "
                          "(SimpleIni rc={}, abs='{}'). Corollary: the MCM page has never been "
                          "opened successfully, or MCM Helper is not installed.",
                          static_cast<int>(rc),
                          ec ? std::string{kMcmIniPath} : abs.string());
            return;
        }
        // Snapshot the values MCM-editable keys hold BEFORE the override
        // pass so the trace can show what actually changed. Cheap to
        // capture (a few ints and a bitmask) and pays off every time the
        // MCM writes something.
        const int prevDxsc = g_config.dashboardHotkeyDXSC;
        const std::uint8_t prevMods = g_config.dashboardHotkeyModifiers;
        const bool prevDebug = g_config.debugMode;
        const bool prevTrace = g_config.traceMode;
        const bool prevTick = g_config.tickEnabled;
        const int prevInterval = g_config.tickIntervalSeconds;

        ReadIniInto(ini, g_config);
        logger::info("Settings: MCM overrides applied from {}", kMcmIniPath);
        logger::trace("Settings[trace]: MCM override delta: "
                      "hotkey_dxsc {}->{} hotkey_mods 0x{:02X}->0x{:02X} debug {}->{} "
                      "trace {}->{} tick_enabled {}->{} tick_interval {}->{}",
                      prevDxsc,
                      g_config.dashboardHotkeyDXSC,
                      static_cast<int>(prevMods),
                      static_cast<int>(g_config.dashboardHotkeyModifiers),
                      prevDebug ? 1 : 0,
                      g_config.debugMode ? 1 : 0,
                      prevTrace ? 1 : 0,
                      g_config.traceMode ? 1 : 0,
                      prevTick ? 1 : 0,
                      g_config.tickEnabled ? 1 : 0,
                      prevInterval,
                      g_config.tickIntervalSeconds);
        ApplyLogLevelForTraceMode();
        RebuildSpellNameBlocklistSet();
    }

    bool IsSpellNameBlocked(std::string_view spellName)
    {
        if (g_spellNameBlocklistSet.empty() || spellName.empty()) {
            return false;
        }
        return g_spellNameBlocklistSet.contains(ToLowerCopy(spellName));
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
            ApplyLogLevelForTraceMode();
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
        if (mutations.letterComposeMemoryRenderCap) {
            ini.SetLongValue("Beats", "iLetterComposeMemoryRenderCap", *mutations.letterComposeMemoryRenderCap);
            g_config.letterComposeMemoryRenderCap = *mutations.letterComposeMemoryRenderCap;
            logger::info("Settings: MCM override write: iLetterComposeMemoryRenderCap={}",
                         *mutations.letterComposeMemoryRenderCap);
        }
        if (mutations.letterComposeDialogueRenderCap) {
            ini.SetLongValue("Beats", "iLetterComposeDialogueRenderCap", *mutations.letterComposeDialogueRenderCap);
            g_config.letterComposeDialogueRenderCap = *mutations.letterComposeDialogueRenderCap;
            logger::info("Settings: MCM override write: iLetterComposeDialogueRenderCap={}",
                         *mutations.letterComposeDialogueRenderCap);
        }
        if (mutations.actionSelectEventRenderCap) {
            ini.SetLongValue("Beats", "iActionSelectEventRenderCap", *mutations.actionSelectEventRenderCap);
            g_config.actionSelectEventRenderCap = *mutations.actionSelectEventRenderCap;
            logger::info("Settings: MCM override write: iActionSelectEventRenderCap={}",
                         *mutations.actionSelectEventRenderCap);
        }
        if (mutations.actionSelectLetterMemoryRenderCap) {
            ini.SetLongValue(
                "Beats", "iActionSelectLetterMemoryRenderCap", *mutations.actionSelectLetterMemoryRenderCap);
            g_config.actionSelectLetterMemoryRenderCap = *mutations.actionSelectLetterMemoryRenderCap;
            logger::info("Settings: MCM override write: iActionSelectLetterMemoryRenderCap={}",
                         *mutations.actionSelectLetterMemoryRenderCap);
        }
        if (mutations.actionSelectVisitMemoryRenderCap) {
            ini.SetLongValue("Beats", "iActionSelectVisitMemoryRenderCap", *mutations.actionSelectVisitMemoryRenderCap);
            g_config.actionSelectVisitMemoryRenderCap = *mutations.actionSelectVisitMemoryRenderCap;
            logger::info("Settings: MCM override write: iActionSelectVisitMemoryRenderCap={}",
                         *mutations.actionSelectVisitMemoryRenderCap);
        }
        if (mutations.skyrimNetEventTailSizeForPrompt) {
            ini.SetLongValue(
                "Director", "iSkyrimNetEventTailSizeForPrompt", *mutations.skyrimNetEventTailSizeForPrompt);
            g_config.skyrimNetEventTailSizeForPrompt = *mutations.skyrimNetEventTailSizeForPrompt;
            logger::info("Settings: MCM override write: iSkyrimNetEventTailSizeForPrompt={}",
                         *mutations.skyrimNetEventTailSizeForPrompt);
        }
        if (mutations.decisionLogTailSizeForPrompt) {
            ini.SetLongValue("Director", "iDecisionLogTailSizeForPrompt", *mutations.decisionLogTailSizeForPrompt);
            g_config.decisionLogTailSizeForPrompt = *mutations.decisionLogTailSizeForPrompt;
            logger::info("Settings: MCM override write: iDecisionLogTailSizeForPrompt={}",
                         *mutations.decisionLogTailSizeForPrompt);
        }
        if (mutations.visitComposeMemoryRenderCap) {
            ini.SetLongValue("Beats", "iVisitComposeMemoryRenderCap", *mutations.visitComposeMemoryRenderCap);
            g_config.visitComposeMemoryRenderCap = *mutations.visitComposeMemoryRenderCap;
            logger::info("Settings: MCM override write: iVisitComposeMemoryRenderCap={}",
                         *mutations.visitComposeMemoryRenderCap);
        }
        if (mutations.visitComposeDialogueRenderCap) {
            ini.SetLongValue("Beats", "iVisitComposeDialogueRenderCap", *mutations.visitComposeDialogueRenderCap);
            g_config.visitComposeDialogueRenderCap = *mutations.visitComposeDialogueRenderCap;
            logger::info("Settings: MCM override write: iVisitComposeDialogueRenderCap={}",
                         *mutations.visitComposeDialogueRenderCap);
        }
        if (mutations.visitConclusionPollRecentLinesRenderCap) {
            ini.SetLongValue("Beats",
                             "iVisitConclusionPollRecentLinesRenderCap",
                             *mutations.visitConclusionPollRecentLinesRenderCap);
            g_config.visitConclusionPollRecentLinesRenderCap = *mutations.visitConclusionPollRecentLinesRenderCap;
            logger::info("Settings: MCM override write: iVisitConclusionPollRecentLinesRenderCap={}",
                         *mutations.visitConclusionPollRecentLinesRenderCap);
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
