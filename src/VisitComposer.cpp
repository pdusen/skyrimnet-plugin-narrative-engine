#include <VisitComposer.h>

#include <EvaluationPipeline.h>
#include <EventLogUtil.h>
#include <LLMTextSanitizer.h>
#include <logger.h>
#include <NPCVisitBeat.h>
#include <SenderCandidatePool.h>
#include <SenderDialogue.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <SkyrimNetEvents.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace NarrativeEngine::VisitComposer
{
    namespace
    {
        // Visit's mood set — same as letter plus `contrite`.
        const std::set<std::string>& ValidMoods()
        {
            static const std::set<std::string> kSet{
                "warm",
                "neutral",
                "urgent",
                "menacing",
                "mournful",
                "contrite",
                "businesslike",
            };
            return kSet;
        }

        // Visit-specific candidate viability, layered on top of
        // SenderCandidatePool's universal walk (missing / dead /
        // disabled / no-name). Rejects candidates who are:
        //   - Not unique (leveled / templated actors — no persistent
        //     identity to warp).
        //   - Currently in combat.
        //   - The player's active follower (they're already here).
        //   - Without a resolvable current location (engine-limbo).
        bool VisitViabilityFilter(RE::Actor* actor, std::string* skipReasonOut)
        {
            if (!actor) {
                if (skipReasonOut)
                    *skipReasonOut = "missing-actor";
                return false;
            }

            // Uniqueness. TESNPC carries the IsUnique flag; leveled or
            // templated actors do not, and warping one in would create a
            // strange "the same Whiterun Guard visited three times" beat.
            if (auto* base = actor->GetActorBase()) {
                if (!base->IsUnique()) {
                    if (skipReasonOut)
                        *skipReasonOut = "not-unique";
                    return false;
                }
            }

            // Not currently in combat.
            if (actor->IsInCombat()) {
                if (skipReasonOut)
                    *skipReasonOut = "in-combat";
                return false;
            }

            // Not the player's active follower — a follower can't
            // "arrive to talk"; they're already there.
            if (actor->IsPlayerTeammate()) {
                if (skipReasonOut)
                    *skipReasonOut = "player-follower";
                return false;
            }

            // Has a current location — sanity signal that the actor isn't
            // in engine-limbo. Nulls degrade to skip.
            if (!actor->GetCurrentLocation()) {
                if (skipReasonOut)
                    *skipReasonOut = "no-current-location";
                return false;
            }

            // Per-sender cooldown — this NPC visited recently and is
            // still within their in-game-hours cooldown window. Filters
            // out the "Ancano visits three times in a row" pathology.
            if (NPCVisitBeat_Cooldowns::IsSenderOnCooldown(actor->GetFormID())) {
                if (skipReasonOut)
                    *skipReasonOut = "sender-cooldown";
                return false;
            }

            return true;
        }

        // Count whitespace-separated words. Cheap; doesn't need to be
        // Unicode-aware because we already sanitized the briefing.
        std::size_t WordCount(const std::string& s)
        {
            std::size_t n = 0;
            bool inWord = false;
            for (char c : s) {
                const bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
                if (!ws && !inWord) {
                    ++n;
                    inWord = true;
                } else if (ws) {
                    inWord = false;
                }
            }
            return n;
        }

        std::string GetPlayerName()
        {
            // Prefer the shared helper — same lookup shape both composers
            // depend on. Empty when the player hasn't loaded yet.
            return SenderCandidatePool::GetPlayerDisplayName();
        }

        // Fresh-fetch memories for the chosen sender at compose time,
        // so any events that landed between action-select and compose
        // surface in the prompt. Fetched for the sender directly rather
        // than through a pool build: the build walks only the top few
        // engagement rows, and a sender ranked below them came back
        // with no memories at all. Visits keep diaries enabled (they're
        // useful narration seeds).
        //
        // NOTE: no per-sender watermark filter here. The pool-side
        // watermark on the action-select build already kept
        // pre-watermark memories out of the list the Director picked
        // the motivating memory from. Leaving the full tail intact so
        // the composer has maximum voice / tone context.
        nlohmann::json FetchSenderMemoriesFresh(RE::FormID formId, int renderCap)
        {
            if (formId == 0) {
                return nlohmann::json::array();
            }
            SenderCandidatePool::BuildOptions opts;
            opts.maxMemoriesPerCandidate = std::max(0, renderCap);
            opts.memoryImportanceThreshold = static_cast<double>(Settings::Get().letterMemoryImportanceThreshold);
            opts.excludeDiaryEntries = false;
            opts.memoryFetchMultiplier = 4;
            return SenderCandidatePool::FetchMemories(formId, opts);
        }

        nlohmann::json BuildComposePromptContext(const BeatContext& ctx,
                                                 UrgencyHint urgencyHint,
                                                 const std::string& playerName,
                                                 const std::string& senderName,
                                                 RE::FormID senderFormID,
                                                 const nlohmann::json& senderMemories,
                                                 const nlohmann::json& senderRecentDialogue,
                                                 const nlohmann::json& motivatingMemory)
        {
            const auto& cfg = Settings::Get();

            char idBuf[16];
            std::snprintf(idBuf, sizeof(idBuf), "0x%X", senderFormID);

            nlohmann::json root = nlohmann::json::object();
            root["desired_direction"] = (ctx.desiredDirection == PhaseTracker::Direction::Raise) ? "raise" : "lower";
            root["tension_delta"] = ctx.tensionDelta;
            root["urgency_hint"] = (urgencyHint == UrgencyHint::High)  ? "high"
                                   : (urgencyHint == UrgencyHint::Low) ? "low"
                                                                       : "medium";
            root["min_words"] = cfg.visitBriefingMinWords;
            root["max_words"] = cfg.visitBriefingMaxWords;
            root["player_name"] = playerName;
            root["sender_name"] = senderName;
            root["sender_form_id"] = idBuf;
            root["sender_memories"] = senderMemories;
            root["sender_recent_dialogue"] = senderRecentDialogue;
            root["has_motivating_memory"] = motivatingMemory.is_object();
            root["motivating_memory"] = motivatingMemory.is_object() ? motivatingMemory : nlohmann::json::object();
            return root;
        }
    } // namespace

    bool IsValidMood(const std::string& mood)
    {
        return ValidMoods().contains(mood);
    }

    std::vector<SenderCandidate> CollectSenderCandidates()
    {
        // Delegate the engagement fetch + universal viability walk +
        // memory fetch to SenderCandidatePool, passing visit-specific
        // options and the visit viability filter.
        const auto& cfg = Settings::Get();

        SenderCandidatePool::BuildOptions opts;
        opts.maxCandidates = 12;
        opts.maxMemoriesPerCandidate = cfg.actionSelectVisitMemoryRenderCap;
        // Reuse the letter's importance floor for memory quality —
        // separate visit-specific floor isn't worth wiring up.
        opts.memoryImportanceThreshold = static_cast<double>(cfg.letterMemoryImportanceThreshold);
        opts.excludeDiaryEntries = false; // brief benefits from diary-style memories
        opts.memoryFetchMultiplier = 4;
        opts.shuffleResult = true;
        // Visits require at least one memory to reach the action-
        // select prompt: with the per-sender memory watermark filter
        // in place, an NPC whose entire memory tail predates their
        // last visit has no legitimate topic to motivate a fresh
        // visit. Dropping them from the candidate list is the
        // intended outcome.
        opts.requireMemories = true;
        opts.memoryWatermarkProvider = [](RE::FormID id) {
            return NPCVisitBeat_Cooldowns::GetSenderMemoryWatermarkGameHours(id);
        };
        opts.extraViabilityFilter = &VisitViabilityFilter;

        auto raw = SenderCandidatePool::Build(opts);

        std::vector<SenderCandidate> out;
        out.reserve(raw.size());
        for (auto& c : raw) {
            SenderCandidate s;
            s.formId = c.formId;
            s.name = std::move(c.name);
            s.engagementScore = c.engagementScore;
            s.lastInteractedAt = c.lastInteractedAt;
            s.memories = std::move(c.memories);
            out.push_back(std::move(s));
        }
        return out;
    }

    nlohmann::json SerializeSenderCandidates(const std::vector<SenderCandidate>& candidates)
    {
        auto out = nlohmann::json::array();
        for (const auto& c : candidates) {
            nlohmann::json cj = nlohmann::json::object();
            char idBuf[16];
            std::snprintf(idBuf, sizeof(idBuf), "0x%X", c.formId);
            cj["form_id"] = idBuf;
            cj["name"] = c.name;
            cj["engagement_score"] = c.engagementScore;
            cj["last_interacted_at"] = c.lastInteractedAt;
            cj["memories"] = c.memories;
            out.push_back(std::move(cj));
        }
        return out;
    }

    void Compose(const BeatContext& ctx,
                 UrgencyHint urgencyHint,
                 RE::FormID senderNpcFormID,
                 nlohmann::json motivatingMemory,
                 std::function<void(std::optional<VisitBriefing>)> callback)
    {
        if (!callback)
            return;

        if (senderNpcFormID == 0) {
            logger::warn("VisitComposer: Compose called with sender formID=0");
            callback(std::nullopt);
            return;
        }

        if (!SkyrimNetAPI::IsAvailable() || !SkyrimNetAPI::IsMemorySystemReady()) {
            logger::warn("VisitComposer: SkyrimNet unavailable or memory system not ready");
            callback(std::nullopt);
            return;
        }

        // Re-resolve the sender on the main thread. The action-select
        // round-trip may have taken seconds; the sender could have
        // died, been disabled, or moved into an invalid state in the
        // interim. A minimal check here beats letting the compose
        // callback fail deeper.
        auto* form = RE::TESForm::LookupByID(senderNpcFormID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (!actor) {
            logger::warn("VisitComposer: sender 0x{:X} no longer resolves to an Actor — "
                         "declining to compose",
                         senderNpcFormID);
            callback(std::nullopt);
            return;
        }
        if (actor->IsDead()) {
            logger::warn("VisitComposer: sender 0x{:X} is dead — declining to compose", senderNpcFormID);
            callback(std::nullopt);
            return;
        }

        std::string senderName;
        if (const char* dn = actor->GetDisplayFullName()) {
            senderName = LLMTextSanitizer::Sanitize(dn);
        }
        if (senderName.empty()) {
            logger::warn("VisitComposer: sender 0x{:X} has no resolvable display name — "
                         "declining to compose",
                         senderNpcFormID);
            callback(std::nullopt);
            return;
        }

        const std::string playerName = GetPlayerName();
        if (playerName.empty()) {
            logger::warn("VisitComposer: player has no resolvable display name — declining to compose");
            callback(std::nullopt);
            return;
        }

        // Fresh memory pull for the picked sender. Render caps pulled
        // from Settings so users on tight-context local LLMs can dial
        // the compose prompt's payload down without editing code.
        const auto& composeCfg = Settings::Get();
        const auto memories = FetchSenderMemoriesFresh(senderNpcFormID, composeCfg.visitComposeMemoryRenderCap);

        // Most-recent player↔sender dialogue history. Gives the LLM
        // a running-continuity read on how the two of them talk to
        // each other — vocabulary, warmth, formality — beyond what
        // the (third-person) memory tail conveys. Trim entries
        // older than the oldest kept memory so the two sections
        // stay temporally coherent, then annotate each with a
        // human-friendly age label.
        // Strictly two-party: only the player addressing this sender
        // and this sender addressing the player. See SenderDialogue.h
        // for why that needs the event stream rather than SkyrimNet's
        // dialogue endpoint.
        const double nowGameSeconds = EventLogUtil::NowGameTimeSeconds();
        auto recentDialogue =
            SenderDialogue::Fetch(senderNpcFormID, senderName, playerName, composeCfg.visitComposeDialogueRenderCap);
        SenderDialogue::FilterByMemoryAge(recentDialogue, memories, nowGameSeconds);
        SenderDialogue::AnnotateAges(recentDialogue, nowGameSeconds);

        const auto promptCtx = BuildComposePromptContext(
            ctx, urgencyHint, playerName, senderName, senderNpcFormID, memories, recentDialogue, motivatingMemory);
        const auto promptCtxStr = promptCtx.dump();
        if (Settings::Get().debugMode) {
            logger::debug("VisitComposer: prompt context: {}", promptCtxStr);
        }

        const auto& cfg = Settings::Get();
        const int minWords = cfg.visitBriefingMinWords;
        const int maxWords = cfg.visitBriefingMaxWords;

        // Clone the callback before move so the !queued failure path can
        // still notify the caller.
        auto callbackBackup = callback;

        const bool queued = SkyrimNetAPI::SendCustomPromptToLLM(
            "narrative_engine_visit_compose",
            "narrative_engine_composer",
            promptCtxStr,
            [callback = std::move(callback), minWords, maxWords](
                const NarrativeEngine::PluginThread::Token&, std::string response, bool success) mutable {
                if (!success) {
                    logger::warn("VisitComposer: LLM call failed: {}", response);
                    callback(std::nullopt);
                    return;
                }
                if (Settings::Get().debugMode) {
                    logger::debug("VisitComposer: raw response: {}", response);
                }

                const auto body = EvaluationPipeline::StripMarkdownFences(response);
                auto parsed = nlohmann::json::parse(body, nullptr, false);
                if (parsed.is_discarded() || !parsed.is_object()) {
                    logger::warn("VisitComposer: response not a JSON object: {}", body);
                    callback(std::nullopt);
                    return;
                }

                auto getStr = [&](const char* key, std::string& out) -> bool {
                    auto it = parsed.find(key);
                    if (it == parsed.end() || !it->is_string())
                        return false;
                    out = LLMTextSanitizer::Sanitize(it->get<std::string>());
                    return true;
                };

                std::string briefing, narration, mood, topic;
                if (!getStr("briefing", briefing) || !getStr("narration", narration) || !getStr("mood", mood)
                    || !getStr("topic_tag", topic)) {
                    logger::warn("VisitComposer: response missing one of the required keys");
                    callback(std::nullopt);
                    return;
                }

                if (!IsValidMood(mood)) {
                    logger::warn("VisitComposer: invalid mood '{}'", mood);
                    callback(std::nullopt);
                    return;
                }

                const auto wc = static_cast<int>(WordCount(briefing));
                if (wc < minWords || wc > maxWords) {
                    logger::warn("VisitComposer: briefing word count {} outside [{}..{}]", wc, minWords, maxWords);
                    callback(std::nullopt);
                    return;
                }

                // Loose narration length guard — the prompt targets
                // one paragraph (~60-150 words), with room for the
                // required "travelled from X" beat plus emotional
                // weight and scene setup. Reject anything shorter
                // than a real scene-setter or wildly over budget
                // (>250) as a probable prompt misfire.
                const auto narrationWc = static_cast<int>(WordCount(narration));
                if (narrationWc < 20 || narrationWc > 250) {
                    logger::warn("VisitComposer: narration word count {} outside [20..250]", narrationWc);
                    callback(std::nullopt);
                    return;
                }

                VisitBriefing briefingOut;
                briefingOut.briefing = std::move(briefing);
                briefingOut.narration = std::move(narration);
                briefingOut.mood = std::move(mood);
                briefingOut.topicTag = std::move(topic);

                logger::info("VisitComposer: parsed briefing (briefing={} words, "
                             "narration={} words, mood='{}', topic='{}')",
                             wc,
                             narrationWc,
                             briefingOut.mood,
                             briefingOut.topicTag);
                callback(std::move(briefingOut));
            });

        if (!queued) {
            logger::warn("VisitComposer: SendCustomPromptToLLM returned false; "
                         "callback will not fire — notifying caller with nullopt");
            callbackBackup(std::nullopt);
        }
    }
} // namespace NarrativeEngine::VisitComposer
