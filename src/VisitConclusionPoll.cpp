#include <VisitConclusionPoll.h>

#include <EvaluationPipeline.h>
#include <LLMTextSanitizer.h>
#include <logger.h>
#include <PhaseTracker.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>

#include <nlohmann/json.hpp>

#include <RE/A/Actor.h>
#include <RE/C/Calendar.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESNPC.h>
#include <RE/U/UI.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
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
        constexpr const char* kPromptName = "narrative_engine_visit_conclusion_poll";
        constexpr const char* kPromptVariant = "narrative_engine_director";

        // Recent-lines sample size passed to the poll prompt. Small
        // enough to keep tokens cheap but large enough that a poll
        // near threshold sees the full recent exchange.
        constexpr int kRecentLinesSampleCount = 8;

        // ---- State (main-thread mutation; poll callback marshals
        // back to main before touching). --------------------------

        std::mutex g_mutex;

        bool g_armed = false;

        // Game-time seconds (RE::Calendar::GetHoursPassed() * 3600)
        // stamped whenever the corresponding threshold reset should
        // apply. 0 = "never" — the first tick after Arm establishes
        // a baseline. NOTE: the silence watchdog no longer uses game
        // time — see g_silenceRealSeconds below. The remaining game-
        // time timestamps here drive the interval-since-last-poll
        // safety ceiling and the discuss-armed-at cutoff for pre-
        // Salutation event filtering, which are naturally in game-time
        // terms.
        double g_lastPollGameSeconds = 0.0;
        double g_armedAtGameSeconds = 0.0;

        // Accumulated real (wall-clock) seconds since the last
        // observed speech turn, EXCLUDING intervals during which
        // `RE::UI::GameIsPaused()` returned true. Advanced on each
        // GateTick call by `now - g_lastGateTickRealSeconds` when the
        // game was not paused across that tick. Reset to 0 on Arm and
        // on every RegisterSpeechTurn.
        //
        // Why an accumulator and not `now - lastSpeechTurnReal`: the
        // straight-subtract form counts real time spent in pause
        // menus / load screens, which is exactly what the user does
        // NOT want to count as "the player is ignoring the NPC".
        double g_silenceRealSeconds = 0.0;

        // Real (steady-clock) seconds captured on the previous
        // GateTick, so we can compute a tick-delta and add it to
        // g_silenceRealSeconds when the game wasn't paused. 0 means
        // "no prior tick this arm" — the first tick after Arm
        // establishes the baseline without adding anything.
        double g_lastGateTickRealSeconds = 0.0;

        std::uint32_t g_turnsSinceLastPoll = 0;
        std::atomic<std::uint32_t> g_consecutivePollFailures = 0;

        // Cached snapshot fields captured at Arm-time so the poll
        // doesn't have to re-read them each fire.
        VisitState::Snapshot g_snapshotAtArm{};

        // Recent-verdict ring for the dashboard's Visit tab.
        // Per-process; not persisted.
        std::deque<HistoryEntry> g_recentVerdicts;

        double RealSecondsNow()
        {
            return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count())
                   / 1000.0;
        }

        void PushVerdict(bool shouldConclude, const std::string& rationale)
        {
            std::scoped_lock lock(g_mutex);
            HistoryEntry e;
            e.firedAtRealSeconds = RealSecondsNow();
            e.shouldConclude = shouldConclude;
            e.rationale = rationale;
            g_recentVerdicts.push_back(std::move(e));
            while (g_recentVerdicts.size() > kVerdictRingSize) {
                g_recentVerdicts.pop_front();
            }
        }

        // ---- Helpers -----------------------------------------------

        double GameSecondsNow()
        {
            auto* cal = RE::Calendar::GetSingleton();
            if (!cal)
                return 0.0;
            return static_cast<double>(cal->GetHoursPassed()) * 3600.0;
        }

        // Look up the sender's display name. Returns "" if the
        // FormID no longer resolves (rare — sender got deleted
        // mid-visit).
        std::string SenderDisplayName(RE::FormID senderFormID)
        {
            if (senderFormID == 0)
                return {};
            auto* form = RE::TESForm::LookupByID(senderFormID);
            if (!form)
                return {};
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
            if (!player)
                return {};
            if (auto* name = player->GetName()) {
                return std::string{name};
            }
            return {};
        }

        // Sample the last N player↔sender dialogue turns from
        // SkyrimNet's dialogue log (`GetRecentDialogue`). Returns a
        // JSON array of `{ speaker, text }` objects, oldest first.
        // Empty array on any failure — the prompt handles empty
        // gracefully.
        //
        // Why the dialogue API and not the event stream: SkyrimNet's
        // `GetRecentEvents(..., "dialogue,direct_narration,...")` does
        // not reliably emit the player's chat/voice-input turns as
        // distinct events, and appears to append continuation NPC
        // speech onto the SAME event's `text` field rather than
        // firing new events. Either behavior blinds a poll that wants
        // to see "who said what, in order." `GetRecentDialogue`
        // is purpose-built for player↔NPC dialogue and returns one
        // entry per turn with `{ speaker, text, gameTime }`.
        nlohmann::json SampleRecentLines(RE::FormID senderFormID)
        {
            auto arr = nlohmann::json::array();
            if (!SkyrimNetAPI::IsAvailable())
                return arr;

            // Overfetch — we need to cap the LLM prompt to
            // kRecentLinesSampleCount lines but want headroom to
            // filter out pre-Discuss chatter first.
            const int fetchCap = std::max(kRecentLinesSampleCount * 4, 16);
            const auto raw = SkyrimNetAPI::GetRecentDialogue(senderFormID, fetchCap);
            auto parsed = nlohmann::json::parse(raw, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_array()) {
                logger::warn("VisitConclusionPoll::SampleRecentLines: could not parse "
                             "recent-dialogue payload as JSON array (raw.len={})",
                             raw.size());
                return arr;
            }

            const auto senderName = SenderDisplayName(senderFormID);
            const auto playerName = PlayerDisplayName();
            // Filter entries by gameTime >= discussStartedAt so
            // pre-Salutation exchanges from earlier player-sender
            // conversations don't poison the poll's `recent_lines`
            // context.
            double discussStartedAt = 0.0;
            {
                std::scoped_lock lock(g_mutex);
                discussStartedAt = g_armedAtGameSeconds;
            }

            logger::debug("VisitConclusionPoll::SampleRecentLines: fetched {} raw "
                          "dialogue turn(s) (sender='{}', player='{}', "
                          "discussStartedAt={:.1f})",
                          parsed.size(),
                          senderName,
                          playerName,
                          discussStartedAt);

            std::size_t skippedEmptyText = 0;
            std::size_t skippedBystander = 0;
            std::size_t skippedPreDiscuss = 0;

            // Chronological (oldest-first) is SkyrimNet's guarantee
            // for GetRecentDialogue. Walk once to filter, then take
            // the tail (most-recent kRecentLinesSampleCount) so the
            // prompt sees the newest exchanges even when there's a
            // long backlog.
            nlohmann::json filtered = nlohmann::json::array();
            for (const auto& entry : parsed) {
                if (!entry.is_object())
                    continue;

                if (discussStartedAt > 0.0) {
                    auto gtIt = entry.find("gameTime");
                    if (gtIt != entry.end() && gtIt->is_number()) {
                        if (gtIt->get<double>() < discussStartedAt) {
                            ++skippedPreDiscuss;
                            continue;
                        }
                    }
                }

                // Text extraction. SkyrimNet's GetRecentDialogue
                // stores each turn's utterance under `data` (as a
                // plain string, not a nested object). `text` was our
                // original guess and is retained as a fallback in
                // case a future SkyrimNet build reverts. `content` /
                // `utterance` are further speculative fallbacks — cheap
                // to check, they cover other shapes we've seen on
                // related SkyrimNet APIs.
                std::string text;
                for (const char* candidate : {"data", "text", "content", "utterance"}) {
                    auto it = entry.find(candidate);
                    if (it != entry.end() && it->is_string()) {
                        text = LLMTextSanitizer::Sanitize(it->get<std::string>());
                        if (!text.empty())
                            break;
                    }
                }
                if (text.empty()) {
                    ++skippedEmptyText;
                    if (skippedEmptyText <= 2) {
                        logger::debug("VisitConclusionPoll::SampleRecentLines: entry "
                                      "has no text under known field names — raw JSON: {}",
                                      entry.dump());
                    }
                    continue;
                }

                // Speaker resolution. GetRecentDialogue's `speaker`
                // field is a ROLE literal (`"npc"` or `"player"`),
                // not the actor's display name — the display name for
                // the NPC lives under `npcName`. Map the role to the
                // human-readable name so downstream bystander
                // filtering and prompt rendering both work.
                std::string speakerRaw;
                if (auto it = entry.find("speaker"); it != entry.end() && it->is_string()) {
                    speakerRaw = it->get<std::string>();
                }
                std::string npcNameField;
                if (auto it = entry.find("npcName"); it != entry.end() && it->is_string()) {
                    npcNameField = LLMTextSanitizer::Sanitize(it->get<std::string>());
                }

                std::string speaker;
                if (speakerRaw == "player") {
                    speaker = playerName;
                } else if (speakerRaw == "npc") {
                    speaker = !npcNameField.empty() ? npcNameField : senderName;
                } else {
                    // Older / alternate payloads that carry a real
                    // name in `speaker` — trust it as-is.
                    speaker = LLMTextSanitizer::Sanitize(speakerRaw);
                }

                // Bystander skip — reject anything that resolves to
                // a name other than the sender or the player. Empty
                // speaker still gets kept (better to over-include).
                if (!speaker.empty() && !senderName.empty() && !playerName.empty() && speaker != senderName
                    && speaker != playerName) {
                    ++skippedBystander;
                    continue;
                }

                nlohmann::json line;
                line["speaker"] = speaker.empty() ? std::string{"?"} : speaker;
                line["text"] = text;
                filtered.push_back(std::move(line));
            }

            // Take the last kRecentLinesSampleCount entries so the
            // prompt sees the newest exchange, preserving oldest-
            // first order within the slice.
            const std::size_t keep = static_cast<std::size_t>(std::max(1, kRecentLinesSampleCount));
            const std::size_t start = filtered.size() > keep ? filtered.size() - keep : 0;
            for (std::size_t i = start; i < filtered.size(); ++i) {
                arr.push_back(std::move(filtered[i]));
            }

            logger::info("VisitConclusionPoll::SampleRecentLines: kept {} line(s) for "
                         "poll context (skipped_empty_text={}, skipped_bystander={}, "
                         "skipped_pre_discuss={})",
                         arr.size(),
                         skippedEmptyText,
                         skippedBystander,
                         skippedPreDiscuss);
            return arr;
        }

        nlohmann::json BuildPromptContext(RE::FormID senderFormID,
                                          const std::string& senderName,
                                          const std::string& playerName,
                                          double elapsedSeconds,
                                          std::uint32_t nudgeCount,
                                          const VisitState::Snapshot& snap)
        {
            nlohmann::json root = nlohmann::json::object();
            root["sender_name"] = senderName;
            root["player_name"] = playerName;
            root["sender_goal"] = snap.briefingText;
            root["topic_tag"] = snap.topicTag;
            root["mood"] = snap.mood;
            root["desired_direction"] = "raise"; // baseline; poll doesn't yet consume the live PhaseTracker direction —
                                                 // inputs come from the visit's own composed brief
            root["elapsed_seconds"] = static_cast<int>(elapsedSeconds);
            root["nudge_count"] = nudgeCount;
            root["recent_lines"] = SampleRecentLines(senderFormID);
            return root;
        }
    } // namespace

    void Arm(const VisitState::Snapshot& snapshot)
    {
        std::scoped_lock lock(g_mutex);
        g_armed = true;
        g_snapshotAtArm = snapshot;
        const auto now = GameSecondsNow();
        g_armedAtGameSeconds = now;
        g_lastPollGameSeconds = now;
        g_silenceRealSeconds = 0.0;
        g_lastGateTickRealSeconds = 0.0;
        g_turnsSinceLastPoll = 0;
        g_consecutivePollFailures.store(0);
        logger::info("VisitConclusionPoll: armed (sender=0x{:08X}, topic='{}', mood='{}')",
                     snapshot.senderFormID,
                     snapshot.topicTag,
                     snapshot.mood);
    }

    void Disarm()
    {
        std::scoped_lock lock(g_mutex);
        if (!g_armed)
            return;
        g_armed = false;
        g_snapshotAtArm = VisitState::Snapshot{};
        g_lastPollGameSeconds = 0.0;
        g_armedAtGameSeconds = 0.0;
        g_silenceRealSeconds = 0.0;
        g_lastGateTickRealSeconds = 0.0;
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
        if (!g_armed)
            return false;

        const auto& cfg = Settings::Get();
        const double nowGame = GameSecondsNow();
        const double nowReal = RealSecondsNow();

        // Advance the unpaused-real-seconds silence accumulator.
        // Only the delta since the previous tick is added, and only
        // if the game was not paused at the moment of this tick.
        // (Skyrim's UI::GameIsPaused returns true for menus that
        // freeze the gameplay clock — journal, inventory, map, load
        // screens, main menu. It does NOT return true for the
        // dialogue view, which by design keeps the game running.)
        //
        // On the first tick after Arm, g_lastGateTickRealSeconds is
        // 0.0 — we just establish the baseline and add nothing, so a
        // long stall before the first tick can't retroactively pile
        // up silence.
        bool paused = false;
        if (auto* ui = RE::UI::GetSingleton()) {
            paused = ui->GameIsPaused();
        }
        double addedSilence = 0.0;
        if (g_lastGateTickRealSeconds > 0.0) {
            const double delta = nowReal - g_lastGateTickRealSeconds;
            if (delta > 0.0 && !paused) {
                g_silenceRealSeconds += delta;
                addedSilence = delta;
            }
        }
        g_lastGateTickRealSeconds = nowReal;

        const double silenceLimit = static_cast<double>(std::max(0, cfg.visitPollSilenceRealSeconds));
        const double maxInterval = static_cast<double>(std::max(0, cfg.visitPollMaxIntervalGameMinutes)) * 60.0;
        const std::uint32_t turnLimit = static_cast<std::uint32_t>(std::max(0, cfg.visitPollTurnCountThreshold));

        const bool turnsHit = (turnLimit > 0) && (g_turnsSinceLastPoll >= turnLimit);
        const bool silenceHit = (silenceLimit > 0.0) && (g_silenceRealSeconds >= silenceLimit);
        const bool intervalHit =
            (maxInterval > 0.0) && (g_lastPollGameSeconds > 0.0) && (nowGame - g_lastPollGameSeconds >= maxInterval);

        if (turnsHit || silenceHit || intervalHit) {
            logger::debug("VisitConclusionPoll: gate tripped (turns={} limit={}, "
                          "silenceRealSec={:.1f} limit={:.1f} paused_this_tick={}, "
                          "intervalGameSec={:.1f} limit={:.1f})",
                          g_turnsSinceLastPoll,
                          turnLimit,
                          g_silenceRealSeconds,
                          silenceLimit,
                          paused,
                          nowGame - g_lastPollGameSeconds,
                          maxInterval);
            return true;
        }

        // In debug mode, tick-level trace so we can watch the
        // accumulator climb and confirm pause-time isn't counted.
        // Only log when we actually added silence — every-tick chatter
        // when the game is paused isn't useful and would flood the log.
        if (addedSilence > 0.0 && Settings::Get().debugMode) {
            logger::debug("VisitConclusionPoll: gate tick — silenceRealSec={:.1f}/{:.1f} "
                          "(+{:.2f} this tick, paused={})",
                          g_silenceRealSeconds,
                          silenceLimit,
                          addedSilence,
                          paused);
        }
        return false;
    }

    void FirePoll(std::function<void(std::optional<PollVerdict>)> callback)
    {
        if (!callback)
            return;

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

        const auto promptCtx = BuildPromptContext(snap.senderFormID, senderName, playerName, elapsed, nudge, snap);
        const auto promptCtxStr = promptCtx.dump();

        if (Settings::Get().debugMode) {
            logger::debug("VisitConclusionPoll::FirePoll: prompt context: {}", promptCtxStr);
        }

        // Clone the callback for the !queued fallback so we always
        // notify the caller.
        auto callbackBackup = callback;

        const bool queued = SkyrimNetAPI::SendCustomPromptToLLM(
            kPromptName,
            kPromptVariant,
            promptCtxStr,
            [callback = std::move(callback)](
                const NarrativeEngine::PluginThread::Token&, std::string response, bool success) mutable {
                if (!success) {
                    logger::warn("VisitConclusionPoll: LLM call failed: {}", response);
                    g_consecutivePollFailures.fetch_add(1);
                    callback(std::nullopt);
                    return;
                }
                if (Settings::Get().debugMode) {
                    logger::debug("VisitConclusionPoll: raw response: {}", response);
                }

                const auto body = EvaluationPipeline::StripMarkdownFences(response);
                auto parsed = nlohmann::json::parse(body, nullptr, false);
                if (parsed.is_discarded() || !parsed.is_object()) {
                    logger::warn("VisitConclusionPoll: response not a JSON object: {}", body);
                    g_consecutivePollFailures.fetch_add(1);
                    callback(std::nullopt);
                    return;
                }

                auto shouldIt = parsed.find("should_conclude");
                if (shouldIt == parsed.end() || !shouldIt->is_boolean()) {
                    logger::warn("VisitConclusionPoll: response missing / bad should_conclude");
                    g_consecutivePollFailures.fetch_add(1);
                    callback(std::nullopt);
                    return;
                }

                PollVerdict v;
                v.shouldConclude = shouldIt->get<bool>();
                if (auto ratIt = parsed.find("rationale"); ratIt != parsed.end() && ratIt->is_string()) {
                    v.rationale = LLMTextSanitizer::Sanitize(ratIt->get<std::string>());
                }
                // `closing_already_spoken` is optional — missing
                // defaults to false, which preserves the pre-
                // existing DirectNarration path.
                if (auto casIt = parsed.find("closing_already_spoken"); casIt != parsed.end() && casIt->is_boolean()) {
                    v.closingAlreadySpoken = casIt->get<bool>();
                }

                g_consecutivePollFailures.store(0);
                PushVerdict(v.shouldConclude, v.rationale);
                callback(std::move(v));
            });

        if (!queued) {
            logger::warn("VisitConclusionPoll::FirePoll: SendCustomPromptToLLM returned false");
            g_consecutivePollFailures.fetch_add(1);
            callbackBackup(std::nullopt);
        }
    }

    void RegisterSpeechTurn()
    {
        std::scoped_lock lock(g_mutex);
        if (!g_armed)
            return;
        g_turnsSinceLastPoll += 1;
        // A fresh speech turn resets the unpaused-real-seconds silence
        // accumulator. `g_lastGateTickRealSeconds` deliberately isn't
        // reset — it just marks "when did we last observe a tick" and
        // is used as a delta baseline in GateTick.
        g_silenceRealSeconds = 0.0;
    }

    std::uint32_t ConsecutivePollFailures()
    {
        return g_consecutivePollFailures.load();
    }

    double SilenceRealSeconds()
    {
        std::scoped_lock lock(g_mutex);
        if (!g_armed)
            return 0.0;
        return std::max(0.0, g_silenceRealSeconds);
    }

    double DiscussStartedAtGameSeconds()
    {
        std::scoped_lock lock(g_mutex);
        if (!g_armed)
            return 0.0;
        return g_armedAtGameSeconds;
    }

    std::vector<HistoryEntry> GetRecentVerdicts()
    {
        std::scoped_lock lock(g_mutex);
        return {g_recentVerdicts.begin(), g_recentVerdicts.end()};
    }
} // namespace NarrativeEngine::VisitConclusionPoll
