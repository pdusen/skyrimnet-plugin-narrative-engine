#include <VisitConclusionPoll.h>

#include <EvaluationPipeline.h>
#include <LLMTextSanitizer.h>
#include <PhaseTracker.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <logger.h>

#include <nlohmann/json.hpp>

#include <RE/A/Actor.h>
#include <RE/C/Calendar.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESNPC.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace NarrativeEngine::VisitConclusionPoll
{
    namespace
    {
        // Prompt template ID + variant. Matches the file under
        // statics/SKSE/Plugins/SkyrimNet/prompts/.
        constexpr const char* kPromptName    = "narrative_engine_visit_conclusion_poll";
        constexpr const char* kPromptVariant = "narrative_engine_director";

        // Recent-lines sample size passed to the poll prompt. Small
        // enough to keep tokens cheap but large enough that a poll
        // near threshold sees the full recent exchange.
        constexpr int         kRecentLinesSampleCount = 8;

        // ---- State (main-thread mutation; poll callback marshals
        // back to main before touching). --------------------------

        std::mutex                  g_mutex;

        bool                        g_armed                       = false;

        // Game-time seconds (RE::Calendar::GetHoursPassed() * 3600)
        // stamped whenever the corresponding threshold reset should
        // apply. 0 = "never" — the first tick after Arm establishes
        // a baseline.
        double                      g_lastPollGameSeconds         = 0.0;
        double                      g_lastSpeechTurnGameSeconds   = 0.0;
        double                      g_armedAtGameSeconds          = 0.0;

        std::uint32_t               g_turnsSinceLastPoll          = 0;
        std::atomic<std::uint32_t>  g_consecutivePollFailures     = 0;

        // Cached snapshot fields captured at Arm-time so the poll
        // doesn't have to re-read them each fire.
        VisitState::Snapshot        g_snapshotAtArm{};

        // Recent-verdict ring for the dashboard's Visit tab.
        // Per-process; not persisted.
        std::deque<HistoryEntry>    g_recentVerdicts;

        double RealSecondsNow()
        {
            return static_cast<double>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count())
                / 1000.0;
        }

        void PushVerdict(bool shouldConclude, const std::string& rationale)
        {
            std::scoped_lock lock(g_mutex);
            HistoryEntry e;
            e.firedAtRealSeconds = RealSecondsNow();
            e.shouldConclude     = shouldConclude;
            e.rationale          = rationale;
            g_recentVerdicts.push_back(std::move(e));
            while (g_recentVerdicts.size() > kVerdictRingSize) {
                g_recentVerdicts.pop_front();
            }
        }

        // ---- Helpers -----------------------------------------------

        double GameSecondsNow()
        {
            auto* cal = RE::Calendar::GetSingleton();
            if (!cal) return 0.0;
            return static_cast<double>(cal->GetHoursPassed()) * 3600.0;
        }

        // Look up the sender's display name. Returns "" if the
        // FormID no longer resolves (rare — sender got deleted
        // mid-visit).
        std::string SenderDisplayName(RE::FormID senderFormID)
        {
            if (senderFormID == 0) return {};
            auto* form = RE::TESForm::LookupByID(senderFormID);
            if (!form) return {};
            if (auto* actor = form->As<RE::Actor>()) {
                if (auto* name = actor->GetName()) {
                    return std::string{name};
                }
            }
            return {};
        }

        std::string PlayerDisplayName()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return {};
            if (auto* name = player->GetName()) {
                return std::string{name};
            }
            return {};
        }

        // Extract the spoken text from a SkyrimNet event object.
        // SkyrimNet's various event surfaces use different field
        // names for the actual content — the C API's PublicGetRecentEvents
        // returns `text`, but the internal registration path stores
        // dialogue under `data.dialogue`, and direct_narration events
        // sometimes have `data.content` instead. Try the whole
        // sequence and return the first non-empty match.
        std::string ExtractEventText(const nlohmann::json& entry)
        {
            auto tryField = [&](const nlohmann::json& obj, const char* key) -> std::string {
                auto it = obj.find(key);
                if (it == obj.end() || !it->is_string()) return {};
                return LLMTextSanitizer::Sanitize(it->get<std::string>());
            };

            std::string text = tryField(entry, "text");
            if (!text.empty()) return text;

            auto dataIt = entry.find("data");
            if (dataIt == entry.end() || !dataIt->is_object()) return {};
            text = tryField(*dataIt, "dialogue");
            if (!text.empty()) return text;
            text = tryField(*dataIt, "content");
            if (!text.empty()) return text;
            text = tryField(*dataIt, "narration");
            return text;
        }

        // Extract the speaker name from a SkyrimNet event object.
        // Fallback order: originatingActorName → speaker → data.speaker.
        std::string ExtractEventSpeaker(const nlohmann::json& entry)
        {
            auto tryField = [&](const nlohmann::json& obj, const char* key) -> std::string {
                auto it = obj.find(key);
                if (it == obj.end() || !it->is_string()) return {};
                return LLMTextSanitizer::Sanitize(it->get<std::string>());
            };

            std::string speaker = tryField(entry, "originatingActorName");
            if (!speaker.empty()) return speaker;
            speaker = tryField(entry, "speaker");
            if (!speaker.empty()) return speaker;
            auto dataIt = entry.find("data");
            if (dataIt != entry.end() && dataIt->is_object()) {
                speaker = tryField(*dataIt, "speaker");
            }
            return speaker;
        }

        // Sample the last N speech / dialogue events involving the
        // sender or the player from SkyrimNet's event history.
        // Returns a JSON array of `{ speaker, text }` objects.
        // Empty array on any failure — the prompt handles the empty
        // case gracefully.
        nlohmann::json SampleRecentLines(RE::FormID senderFormID)
        {
            auto arr = nlohmann::json::array();
            if (!SkyrimNetAPI::IsAvailable()) return arr;

            // Ask SkyrimNet for a bounded recent-events window.
            //
            // Filter includes both `dialogue` (normal SkyrimNet
            // dialogue events) AND `direct_narration` (events fired
            // via SkyrimNetApi.DirectNarration — which is exactly
            // how we trigger the sender's opening/reengage/closing
            // lines). SkyrimNet's C API accepts a comma-separated
            // list here.
            const auto raw = SkyrimNetAPI::GetRecentEvents(
                senderFormID, kRecentLinesSampleCount * 4,
                "dialogue,direct_narration,dialogue_npc,dialogue_player");
            auto parsed = nlohmann::json::parse(raw, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_array()) {
                logger::warn(
                    "VisitConclusionPoll::SampleRecentLines: could not parse raw "
                    "event stream as JSON array (raw.len={})",
                    raw.size());
                return arr;
            }

            const auto senderName = SenderDisplayName(senderFormID);
            const auto playerName = PlayerDisplayName();
            // Filter events by gameTime >= discussStartedAt so
            // pre-Salutation dialogue from earlier conversations
            // doesn't poison the poll's `recent_lines` context.
            // Read outside the mutex-guarded g_armed check because
            // we're already inside FirePoll (which is armed).
            double discussStartedAt = 0.0;
            {
                std::scoped_lock lock(g_mutex);
                discussStartedAt = g_armedAtGameSeconds;
            }

            // Debug: dump the raw event stream on first call per
            // Discuss watchdog. Kept at debug so it doesn't blow up
            // the log on every gate tick.
            logger::debug(
                "VisitConclusionPoll::SampleRecentLines: fetched {} raw event(s) "
                "(sender='{}', player='{}', discussStartedAt={:.1f})",
                parsed.size(), senderName, playerName, discussStartedAt);

            std::size_t skippedEmptyText = 0;
            std::size_t skippedBystander = 0;
            std::size_t skippedPreDiscuss = 0;

            for (const auto& entry : parsed) {
                if (!entry.is_object()) continue;

                // Filter out anything that predates the Discuss arm
                // — old exchanges from previous player-NPC
                // encounters would otherwise convince the LLM that
                // the current visit is a continuation of a stale
                // conversation.
                if (discussStartedAt > 0.0) {
                    auto gtIt = entry.find("gameTime");
                    if (gtIt != entry.end() && gtIt->is_number()) {
                        const double eventGameTime = gtIt->get<double>();
                        if (eventGameTime < discussStartedAt) {
                            ++skippedPreDiscuss;
                            continue;
                        }
                    }
                }

                const std::string text = ExtractEventText(entry);
                if (text.empty()) {
                    ++skippedEmptyText;
                    // Emit the offending entry once at debug so we
                    // can figure out what field carries the text.
                    if (skippedEmptyText <= 2) {
                        logger::debug(
                            "VisitConclusionPoll::SampleRecentLines: skipped "
                            "empty-text entry: {}", entry.dump());
                    }
                    continue;
                }

                const std::string speaker = ExtractEventSpeaker(entry);

                // Accept only lines from the sender or the player;
                // skip bystander chatter.
                if (!speaker.empty() &&
                    !senderName.empty() &&
                    !playerName.empty() &&
                    speaker != senderName && speaker != playerName) {
                    ++skippedBystander;
                    continue;
                }

                nlohmann::json line;
                line["speaker"] = speaker.empty() ? std::string{"?"} : speaker;
                line["text"]    = text;
                arr.push_back(std::move(line));
                if (static_cast<int>(arr.size()) >= kRecentLinesSampleCount) break;
            }

            logger::info(
                "VisitConclusionPoll::SampleRecentLines: kept {} line(s) for "
                "poll context (skipped_empty_text={}, skipped_bystander={}, "
                "skipped_pre_discuss={})",
                arr.size(), skippedEmptyText, skippedBystander, skippedPreDiscuss);
            return arr;
        }

        nlohmann::json BuildPromptContext(RE::FormID          senderFormID,
                                           const std::string&  senderName,
                                           const std::string&  playerName,
                                           double              elapsedSeconds,
                                           std::uint32_t       nudgeCount,
                                           const VisitState::Snapshot& snap)
        {
            nlohmann::json root = nlohmann::json::object();
            root["sender_name"]  = senderName;
            root["player_name"]  = playerName;
            root["sender_goal"]  = snap.briefingText;
            root["topic_tag"]    = snap.topicTag;
            root["mood"]         = snap.mood;
            root["desired_direction"] = "raise";  // baseline; poll doesn't yet consume the live PhaseTracker direction — inputs come from the visit's own composed brief
            root["elapsed_seconds"]   = static_cast<int>(elapsedSeconds);
            root["nudge_count"]  = nudgeCount;
            root["recent_lines"] = SampleRecentLines(senderFormID);
            return root;
        }
    }

    void Arm(const VisitState::Snapshot& snapshot)
    {
        std::scoped_lock lock(g_mutex);
        g_armed                     = true;
        g_snapshotAtArm             = snapshot;
        const auto now              = GameSecondsNow();
        g_armedAtGameSeconds        = now;
        g_lastPollGameSeconds       = now;
        g_lastSpeechTurnGameSeconds = now;
        g_turnsSinceLastPoll        = 0;
        g_consecutivePollFailures.store(0);
        logger::info(
            "VisitConclusionPoll: armed (sender=0x{:08X}, topic='{}', mood='{}')",
            snapshot.senderFormID, snapshot.topicTag, snapshot.mood);
    }

    void Disarm()
    {
        std::scoped_lock lock(g_mutex);
        if (!g_armed) return;
        g_armed = false;
        g_snapshotAtArm = VisitState::Snapshot{};
        g_lastPollGameSeconds = 0.0;
        g_lastSpeechTurnGameSeconds = 0.0;
        g_armedAtGameSeconds = 0.0;
        g_turnsSinceLastPoll = 0;
        logger::info("VisitConclusionPoll: disarmed");
    }

    bool IsArmed()
    {
        std::scoped_lock lock(g_mutex);
        return g_armed;
    }

    bool GateTick()
    {
        std::scoped_lock lock(g_mutex);
        if (!g_armed) return false;

        const auto& cfg = Settings::Get();
        const double now = GameSecondsNow();

        const double silenceLimit =
            static_cast<double>(std::max(0, cfg.visitPollSilenceGameMinutes)) * 60.0;
        const double maxInterval =
            static_cast<double>(std::max(0, cfg.visitPollMaxIntervalGameMinutes)) * 60.0;
        const std::uint32_t turnLimit =
            static_cast<std::uint32_t>(std::max(0, cfg.visitPollTurnCountThreshold));

        const bool turnsHit   = (turnLimit > 0) &&
                                (g_turnsSinceLastPoll >= turnLimit);
        const bool silenceHit = (silenceLimit > 0.0) &&
                                (g_lastSpeechTurnGameSeconds > 0.0) &&
                                (now - g_lastSpeechTurnGameSeconds >= silenceLimit);
        const bool intervalHit = (maxInterval > 0.0) &&
                                 (g_lastPollGameSeconds > 0.0) &&
                                 (now - g_lastPollGameSeconds >= maxInterval);

        if (turnsHit || silenceHit || intervalHit) {
            logger::debug(
                "VisitConclusionPoll: gate tripped (turns={} limit={}, "
                "silenceSec={:.1f} limit={:.1f}, intervalSec={:.1f} limit={:.1f})",
                g_turnsSinceLastPoll, turnLimit,
                now - g_lastSpeechTurnGameSeconds, silenceLimit,
                now - g_lastPollGameSeconds, maxInterval);
            return true;
        }
        return false;
    }

    void FirePoll(std::function<void(std::optional<PollVerdict>)> callback)
    {
        if (!callback) return;

        VisitState::Snapshot snap;
        double armedAt = 0.0;
        std::uint32_t nudge = 0;
        {
            std::scoped_lock lock(g_mutex);
            if (!g_armed) {
                logger::debug("VisitConclusionPoll::FirePoll: not armed — skipping");
                callback(std::nullopt);
                return;
            }
            snap = g_snapshotAtArm;
            armedAt = g_armedAtGameSeconds;
            nudge = snap.ignoreNudgeCount;
            // Reset the gate counters regardless of verdict — the
            // next fire waits for a fresh gate trip.
            g_turnsSinceLastPoll = 0;
            g_lastPollGameSeconds = GameSecondsNow();
        }

        if (!SkyrimNetAPI::IsAvailable()) {
            logger::warn("VisitConclusionPoll::FirePoll: SkyrimNet unavailable");
            g_consecutivePollFailures.fetch_add(1);
            callback(std::nullopt);
            return;
        }

        const std::string senderName = SenderDisplayName(snap.senderFormID);
        const std::string playerName = PlayerDisplayName();
        const double now = GameSecondsNow();
        const double elapsed = std::max(0.0, now - armedAt);

        const auto promptCtx = BuildPromptContext(
            snap.senderFormID, senderName, playerName, elapsed, nudge, snap);
        const auto promptCtxStr = promptCtx.dump();

        if (Settings::Get().debugMode) {
            logger::debug("VisitConclusionPoll::FirePoll: prompt context: {}",
                          promptCtxStr);
        }

        // Clone the callback for the !queued fallback so we always
        // notify the caller.
        auto callbackBackup = callback;

        const bool queued = SkyrimNetAPI::SendCustomPromptToLLM(
            kPromptName,
            kPromptVariant,
            promptCtxStr,
            [callback = std::move(callback)](std::string response, bool success) mutable {
                if (!success) {
                    logger::warn("VisitConclusionPoll: LLM call failed: {}", response);
                    g_consecutivePollFailures.fetch_add(1);
                    callback(std::nullopt);
                    return;
                }
                if (Settings::Get().debugMode) {
                    logger::debug("VisitConclusionPoll: raw response: {}", response);
                }

                const auto body =
                    EvaluationPipeline::StripMarkdownFences(response);
                auto parsed = nlohmann::json::parse(body, nullptr, false);
                if (parsed.is_discarded() || !parsed.is_object()) {
                    logger::warn("VisitConclusionPoll: response not a JSON object: {}",
                                 body);
                    g_consecutivePollFailures.fetch_add(1);
                    callback(std::nullopt);
                    return;
                }

                auto shouldIt = parsed.find("should_conclude");
                if (shouldIt == parsed.end() || !shouldIt->is_boolean()) {
                    logger::warn(
                        "VisitConclusionPoll: response missing / bad should_conclude");
                    g_consecutivePollFailures.fetch_add(1);
                    callback(std::nullopt);
                    return;
                }

                PollVerdict v;
                v.shouldConclude = shouldIt->get<bool>();
                if (auto ratIt = parsed.find("rationale");
                    ratIt != parsed.end() && ratIt->is_string()) {
                    v.rationale = LLMTextSanitizer::Sanitize(ratIt->get<std::string>());
                }

                g_consecutivePollFailures.store(0);
                PushVerdict(v.shouldConclude, v.rationale);
                callback(std::move(v));
            });

        if (!queued) {
            logger::warn(
                "VisitConclusionPoll::FirePoll: SendCustomPromptToLLM returned false");
            g_consecutivePollFailures.fetch_add(1);
            callbackBackup(std::nullopt);
        }
    }

    void RegisterSpeechTurn()
    {
        std::scoped_lock lock(g_mutex);
        if (!g_armed) return;
        g_turnsSinceLastPoll += 1;
        g_lastSpeechTurnGameSeconds = GameSecondsNow();
    }

    std::uint32_t ConsecutivePollFailures()
    {
        return g_consecutivePollFailures.load();
    }

    double SilenceGameSeconds()
    {
        std::scoped_lock lock(g_mutex);
        if (!g_armed) return 0.0;
        if (g_lastSpeechTurnGameSeconds <= 0.0) return 0.0;
        const double now = GameSecondsNow();
        return std::max(0.0, now - g_lastSpeechTurnGameSeconds);
    }

    double DiscussStartedAtGameSeconds()
    {
        std::scoped_lock lock(g_mutex);
        if (!g_armed) return 0.0;
        return g_armedAtGameSeconds;
    }

    std::vector<HistoryEntry> GetRecentVerdicts()
    {
        std::scoped_lock lock(g_mutex);
        return { g_recentVerdicts.begin(), g_recentVerdicts.end() };
    }
}
