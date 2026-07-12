#include <VisitComposer.h>

#include <EvaluationPipeline.h>
#include <LLMTextSanitizer.h>
#include <NPCVisitBeat.h>
#include <SenderCandidatePool.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <logger.h>

#include <nlohmann/json.hpp>

#include <RE/C/Calendar.h>

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
                if (skipReasonOut) *skipReasonOut = "missing-actor";
                return false;
            }

            // Uniqueness. TESNPC carries the IsUnique flag; leveled or
            // templated actors do not, and warping one in would create a
            // strange "the same Whiterun Guard visited three times" beat.
            if (auto* base = actor->GetActorBase()) {
                if (!base->IsUnique()) {
                    if (skipReasonOut) *skipReasonOut = "not-unique";
                    return false;
                }
            }

            // Not currently in combat.
            if (actor->IsInCombat()) {
                if (skipReasonOut) *skipReasonOut = "in-combat";
                return false;
            }

            // Not the player's active follower — a follower can't
            // "arrive to talk"; they're already there.
            if (actor->IsPlayerTeammate()) {
                if (skipReasonOut) *skipReasonOut = "player-follower";
                return false;
            }

            // Has a current location — sanity signal that the actor isn't
            // in engine-limbo. Nulls degrade to skip.
            if (!actor->GetCurrentLocation()) {
                if (skipReasonOut) *skipReasonOut = "no-current-location";
                return false;
            }

            // Per-sender cooldown — this NPC visited recently and is
            // still within their in-game-hours cooldown window. Filters
            // out the "Ancano visits three times in a row" pathology.
            if (NPCVisitBeat_Cooldowns::IsSenderOnCooldown(actor->GetFormID())) {
                if (skipReasonOut) *skipReasonOut = "sender-cooldown";
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
                const bool ws = (c == ' ' || c == '\t' ||
                                 c == '\n' || c == '\r');
                if (!ws && !inWord) { ++n; inWord = true; }
                else if (ws)        { inWord = false; }
            }
            return n;
        }

        std::string GetPlayerName()
        {
            // Prefer the shared helper — same lookup shape both composers
            // depend on. Empty when the player hasn't loaded yet.
            return SenderCandidatePool::GetPlayerDisplayName();
        }

        // How many recent player↔sender dialogue lines to request
        // from SkyrimNet per compose call. Same bound the letter
        // composer uses; more than this gets trimmed by the memory-
        // age filter below anyway.
        constexpr int kRecentDialogueCap = 25;

        // Pull the last N spoken exchanges between the player and
        // the chosen sender out of SkyrimNet. Returns a JSON array
        // of `{ speaker, text, gameTime }` objects. Empty on any
        // failure — the prompt template handles the empty case.
        nlohmann::json FetchRecentDialogue(RE::FormID formId)
        {
            const auto raw = SkyrimNetAPI::GetRecentDialogue(
                formId, kRecentDialogueCap);
            auto parsed = nlohmann::json::parse(raw, nullptr, false);
            if (!parsed.is_array()) {
                return nlohmann::json::array();
            }

            auto trimmed = nlohmann::json::array();
            for (auto& e : parsed) {
                if (!e.is_object()) continue;
                nlohmann::json out = nlohmann::json::object();
                if (auto it = e.find("speaker");
                    it != e.end() && it->is_string()) {
                    out["speaker"] = LLMTextSanitizer::Sanitize(it->get<std::string>());
                }
                if (auto it = e.find("text");
                    it != e.end() && it->is_string()) {
                    out["text"] = LLMTextSanitizer::Sanitize(it->get<std::string>());
                }
                if (auto it = e.find("gameTime");
                    it != e.end() && it->is_number()) {
                    out["gameTime"] = it->get<double>();
                }
                if (!out.contains("speaker") || !out.contains("text")) {
                    continue;
                }
                trimmed.push_back(std::move(out));
            }
            return trimmed;
        }

        // Trim dialogue entries older than the oldest kept memory.
        // Memories carry `age_hours` relative to now; dialogue
        // carries absolute `gameTime`. Convert the oldest memory's
        // age to a game-seconds cutoff and drop dialogue below it.
        //
        // If `memories` is empty or the oldest age is 0 (unknown),
        // no filter is applied — better to show the LLM the full
        // dialogue window than to silently blank it on a missing
        // field.
        void FilterDialogueByMemoryAge(nlohmann::json&       dialogue,
                                       const nlohmann::json& memories)
        {
            if (!dialogue.is_array() || dialogue.empty()) return;

            double oldestMemoryAgeHours = 0.0;
            if (memories.is_array()) {
                for (const auto& m : memories) {
                    const double h = m.value("age_hours", 0.0);
                    if (h > oldestMemoryAgeHours) oldestMemoryAgeHours = h;
                }
            }
            if (oldestMemoryAgeHours <= 0.0) return;

            auto* calendar = RE::Calendar::GetSingleton();
            if (!calendar) return;
            const double nowGameSeconds =
                static_cast<double>(calendar->GetHoursPassed()) * 3600.0;
            const double cutoffGameSeconds =
                nowGameSeconds - oldestMemoryAgeHours * 3600.0;

            auto& arr = dialogue.get_ref<nlohmann::json::array_t&>();
            arr.erase(
                std::remove_if(arr.begin(), arr.end(),
                    [cutoffGameSeconds](const nlohmann::json& e) {
                        return e.value("gameTime", 0.0) < cutoffGameSeconds;
                    }),
                arr.end());
        }

        // Human-friendly age label (same shape the letter composer
        // uses) — the `[…]` prefix on each rendered dialogue line
        // so the LLM has a sense of recency without reasoning about
        // raw game-seconds.
        std::string FormatDialogueAgeLabel(double ageHours)
        {
            if (ageHours < 0.5) return "just now";
            if (ageHours < 1.5) return "1 hour ago";
            if (ageHours < 24.0) {
                const long long h = static_cast<long long>(std::round(ageHours));
                return std::to_string(h) + " hours ago";
            }
            const double days = ageHours / 24.0;
            if (days < 1.5) return "1 day ago";
            const long long d = static_cast<long long>(std::round(days));
            return std::to_string(d) + " days ago";
        }

        // Compute an `age_str` per dialogue entry and drop `gameTime`.
        // Called after the memory-age filter so we only pay the
        // formatting cost on entries the prompt actually renders.
        void AnnotateDialogueAges(nlohmann::json& dialogue)
        {
            if (!dialogue.is_array() || dialogue.empty()) return;
            auto* calendar = RE::Calendar::GetSingleton();
            const double nowGameSeconds =
                calendar
                    ? static_cast<double>(calendar->GetHoursPassed()) * 3600.0
                    : 0.0;
            for (auto& e : dialogue) {
                if (!e.is_object()) continue;
                const double gt = e.value("gameTime", 0.0);
                if (nowGameSeconds > 0.0 && gt > 0.0) {
                    const double ageHours = (nowGameSeconds - gt) / 3600.0;
                    e["age_str"] = FormatDialogueAgeLabel(ageHours);
                } else {
                    e["age_str"] = std::string{"recent"};
                }
                e.erase("gameTime");
            }
        }

        // Fresh-fetch memories for a specific sender after the pool
        // build has already resolved them once. Used inside Compose
        // so any events that landed between action-select and compose
        // surface in the prompt. Mirrors LetterComposer's
        // FetchSenderMemories in spirit — visits keep diaries enabled
        // (they're useful narration seeds) and don't retry with a
        // fresh SenderCandidatePool build (the sender was already
        // vetted at action-select time; a stale filter mismatch is
        // acceptable at this point).
        nlohmann::json FetchSenderMemoriesFresh(RE::FormID formId)
        {
            if (!SkyrimNetAPI::IsAvailable() || formId == 0) {
                return nlohmann::json::array();
            }
            SenderCandidatePool::BuildOptions opts;
            opts.maxCandidates              = 1;
            opts.maxMemoriesPerCandidate    = 6;
            opts.memoryImportanceThreshold  =
                static_cast<double>(
                    Settings::Get().letterMemoryImportanceThreshold);
            opts.excludeDiaryEntries        = false;
            opts.memoryFetchMultiplier      = 4;
            opts.shuffleResult              = false;
            opts.requireMemories            = false;
            opts.extraViabilityFilter =
                [formId](RE::Actor* actor, std::string* skipReasonOut) -> bool {
                    if (!actor) {
                        if (skipReasonOut) *skipReasonOut = "missing-actor";
                        return false;
                    }
                    if (actor->GetFormID() != formId) {
                        if (skipReasonOut) *skipReasonOut = "not-target-sender";
                        return false;
                    }
                    // Skip the full visit-viability re-check here.
                    // The sender was already vetted at action-select
                    // time; NPCVisitAction's own callback will
                    // handle any final "sender no longer valid"
                    // case (death mid-round-trip, etc.) via its
                    // own resolution guards.
                    return true;
                };

            auto results = SenderCandidatePool::Build(opts);
            if (results.empty()) return nlohmann::json::array();
            return std::move(results.front().memories);
        }

        nlohmann::json BuildComposePromptContext(
            const BeatContext&  ctx,
            UrgencyHint           urgencyHint,
            const std::string&    playerName,
            const std::string&    senderName,
            RE::FormID            senderFormID,
            const nlohmann::json& senderMemories,
            const nlohmann::json& senderRecentDialogue,
            const std::string&    parameterJustification)
        {
            const auto& cfg = Settings::Get();

            char idBuf[16];
            std::snprintf(idBuf, sizeof(idBuf), "0x%X", senderFormID);

            nlohmann::json root = nlohmann::json::object();
            root["desired_direction"] =
                (ctx.desiredDirection == PhaseTracker::Direction::Raise)
                    ? "raise" : "lower";
            root["tension_delta"] = ctx.tensionDelta;
            root["urgency_hint"]  =
                (urgencyHint == UrgencyHint::High)   ? "high"
              : (urgencyHint == UrgencyHint::Low)    ? "low"
                                                     : "medium";
            root["min_words"]      = cfg.visitBriefingMinWords;
            root["max_words"]      = cfg.visitBriefingMaxWords;
            root["player_name"]    = playerName;
            root["sender_name"]    = senderName;
            root["sender_form_id"] = idBuf;
            root["sender_memories"] = senderMemories;
            root["sender_recent_dialogue"] = senderRecentDialogue;
            root["parameter_justification"] = parameterJustification;
            return root;
        }
    }

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
        opts.maxCandidates              = 12;
        opts.maxMemoriesPerCandidate    = 6;
        // Reuse the letter's importance floor for memory quality —
        // separate visit-specific floor isn't worth wiring up.
        opts.memoryImportanceThreshold  =
            static_cast<double>(cfg.letterMemoryImportanceThreshold);
        opts.excludeDiaryEntries        = false;  // brief benefits from diary-style memories
        opts.memoryFetchMultiplier      = 4;
        opts.shuffleResult              = true;
        // Visits keep senders whose memory tail is thin — live
        // actor context and the composer's briefing carry the
        // narrative weight, unlike letters where the letter body
        // depends on concrete memory anchors.
        opts.requireMemories            = false;
        opts.extraViabilityFilter       = &VisitViabilityFilter;

        auto raw = SenderCandidatePool::Build(opts);

        std::vector<SenderCandidate> out;
        out.reserve(raw.size());
        for (auto& c : raw) {
            SenderCandidate s;
            s.formId           = c.formId;
            s.name             = std::move(c.name);
            s.engagementScore  = c.engagementScore;
            s.lastInteractedAt = c.lastInteractedAt;
            s.memories         = std::move(c.memories);
            out.push_back(std::move(s));
        }
        return out;
    }

    nlohmann::json SerializeSenderCandidates(
        const std::vector<SenderCandidate>& candidates)
    {
        auto out = nlohmann::json::array();
        for (const auto& c : candidates) {
            nlohmann::json cj = nlohmann::json::object();
            char idBuf[16];
            std::snprintf(idBuf, sizeof(idBuf), "0x%X", c.formId);
            cj["form_id"]            = idBuf;
            cj["name"]               = c.name;
            cj["engagement_score"]   = c.engagementScore;
            cj["last_interacted_at"] = c.lastInteractedAt;
            cj["memories"]           = c.memories;
            out.push_back(std::move(cj));
        }
        return out;
    }

    void Compose(
        const BeatContext& ctx,
        UrgencyHint          urgencyHint,
        RE::FormID           senderNpcFormID,
        std::string          parameterJustification,
        std::function<void(std::optional<VisitBriefing>)> callback)
    {
        if (!callback) return;

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
            logger::warn(
                "VisitComposer: sender 0x{:X} no longer resolves to an Actor — "
                "declining to compose",
                senderNpcFormID);
            callback(std::nullopt);
            return;
        }
        if (actor->IsDead()) {
            logger::warn(
                "VisitComposer: sender 0x{:X} is dead — declining to compose",
                senderNpcFormID);
            callback(std::nullopt);
            return;
        }

        std::string senderName;
        if (const char* dn = actor->GetDisplayFullName()) {
            senderName = LLMTextSanitizer::Sanitize(dn);
        }
        if (senderName.empty()) {
            logger::warn(
                "VisitComposer: sender 0x{:X} has no resolvable display name — "
                "declining to compose",
                senderNpcFormID);
            callback(std::nullopt);
            return;
        }

        const std::string playerName = GetPlayerName();
        if (playerName.empty()) {
            logger::warn(
                "VisitComposer: player has no resolvable display name — declining to compose");
            callback(std::nullopt);
            return;
        }

        // Fresh memory pull for the picked sender.
        const auto memories = FetchSenderMemoriesFresh(senderNpcFormID);

        // Most-recent player↔sender dialogue history. Gives the LLM
        // a running-continuity read on how the two of them talk to
        // each other — vocabulary, warmth, formality — beyond what
        // the (third-person) memory tail conveys. Trim entries
        // older than the oldest kept memory so the two sections
        // stay temporally coherent, then annotate each with a
        // human-friendly age label.
        auto recentDialogue = FetchRecentDialogue(senderNpcFormID);
        FilterDialogueByMemoryAge(recentDialogue, memories);
        AnnotateDialogueAges(recentDialogue);

        const auto promptCtx = BuildComposePromptContext(
            ctx, urgencyHint, playerName, senderName, senderNpcFormID,
            memories, recentDialogue, parameterJustification);
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
            [callback = std::move(callback),
             minWords, maxWords]
            (std::string response, bool success) mutable
            {
                if (!success) {
                    logger::warn("VisitComposer: LLM call failed: {}", response);
                    callback(std::nullopt);
                    return;
                }
                if (Settings::Get().debugMode) {
                    logger::debug("VisitComposer: raw response: {}", response);
                }

                const auto body =
                    EvaluationPipeline::StripMarkdownFences(response);
                auto parsed = nlohmann::json::parse(body, nullptr, false);
                if (parsed.is_discarded() || !parsed.is_object()) {
                    logger::warn(
                        "VisitComposer: response not a JSON object: {}", body);
                    callback(std::nullopt);
                    return;
                }

                auto getStr = [&](const char* key, std::string& out) -> bool {
                    auto it = parsed.find(key);
                    if (it == parsed.end() || !it->is_string()) return false;
                    out = LLMTextSanitizer::Sanitize(it->get<std::string>());
                    return true;
                };

                std::string briefing, narration, mood, topic;
                if (!getStr("briefing",  briefing)  ||
                    !getStr("narration", narration) ||
                    !getStr("mood",      mood)      ||
                    !getStr("topic_tag", topic)) {
                    logger::warn(
                        "VisitComposer: response missing one of the required keys");
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
                    logger::warn(
                        "VisitComposer: briefing word count {} outside [{}..{}]",
                        wc, minWords, maxWords);
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
                    logger::warn(
                        "VisitComposer: narration word count {} outside [20..250]",
                        narrationWc);
                    callback(std::nullopt);
                    return;
                }

                VisitBriefing briefingOut;
                briefingOut.briefing        = std::move(briefing);
                briefingOut.narration       = std::move(narration);
                briefingOut.mood            = std::move(mood);
                briefingOut.topicTag        = std::move(topic);

                logger::info(
                    "VisitComposer: parsed briefing (briefing={} words, "
                    "narration={} words, mood='{}', topic='{}')",
                    wc, narrationWc, briefingOut.mood, briefingOut.topicTag);
                callback(std::move(briefingOut));
            });

        if (!queued) {
            logger::warn(
                "VisitComposer: SendCustomPromptToLLM returned false; "
                "callback will not fire — notifying caller with nullopt");
            callbackBackup(std::nullopt);
        }
    }
}
