#include <VisitComposer.h>

#include <EvaluationPipeline.h>
#include <LLMTextSanitizer.h>
#include <SenderCandidatePool.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <logger.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
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
        //   - Currently in a dialogue (SkyrimNet or vanilla).
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

        // Parse a hex FormID string as returned by the LLM. Accepts
        // "0xHEX" or bare hex. Returns 0 on any failure.
        RE::FormID ParseHexFormID(const std::string& s)
        {
            if (s.empty()) return 0;
            std::size_t skip = 0;
            if (s.size() >= 2 &&
                (s[0] == '0') &&
                (s[1] == 'x' || s[1] == 'X')) {
                skip = 2;
            }
            std::uint64_t value = 0;
            for (std::size_t i = skip; i < s.size(); ++i) {
                const unsigned char c =
                    static_cast<unsigned char>(std::tolower(s[i]));
                std::uint64_t d;
                if (c >= '0' && c <= '9')      d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else return 0;
                value = (value << 4) | d;
            }
            return static_cast<RE::FormID>(value);
        }

        nlohmann::json SerializeCandidates(
            const std::vector<SenderCandidatePool::Candidate>& candidates)
        {
            auto arr = nlohmann::json::array();
            arr.get_ref<nlohmann::json::array_t&>().reserve(candidates.size());
            for (const auto& c : candidates) {
                nlohmann::json cj = nlohmann::json::object();
                char idBuf[16];
                std::snprintf(idBuf, sizeof(idBuf), "0x%X", c.formId);
                cj["form_id"]            = idBuf;
                cj["name"]               = c.name;
                cj["engagement_score"]   = c.engagementScore;
                cj["last_interacted_at"] = c.lastInteractedAt;
                cj["memories"]           = c.memories;
                arr.push_back(std::move(cj));
            }
            return arr;
        }

        std::string GetPlayerName()
        {
            // Prefer the shared helper — same lookup shape both composers
            // depend on. Empty when the player hasn't loaded yet.
            return SenderCandidatePool::GetPlayerDisplayName();
        }

        nlohmann::json BuildPromptContext(
            const ActionContext&                                ctx,
            UrgencyHint                                         urgencyHint,
            const std::string&                                  playerName,
            const std::vector<SenderCandidatePool::Candidate>&  candidates)
        {
            const auto& cfg = Settings::Get();

            nlohmann::json root = nlohmann::json::object();
            root["desired_direction"] =
                (ctx.desiredDirection == PhaseTracker::Direction::Raise)
                    ? "raise" : "lower";
            root["tension_delta"] = ctx.tensionDelta;
            root["urgency_hint"]  =
                (urgencyHint == UrgencyHint::High)   ? "high"
              : (urgencyHint == UrgencyHint::Low)    ? "low"
                                                     : "medium";
            root["min_words"] = cfg.visitBriefingMinWords;
            root["max_words"] = cfg.visitBriefingMaxWords;
            root["player_name"] = playerName;
            root["candidates"] = SerializeCandidates(candidates);
            return root;
        }
    }

    bool IsValidMood(const std::string& mood)
    {
        return ValidMoods().contains(mood);
    }

    void Compose(
        const ActionContext& ctx,
        UrgencyHint          urgencyHint,
        std::function<void(std::optional<VisitBriefing>)> callback)
    {
        if (!callback) return;

        if (!SkyrimNetAPI::IsAvailable() || !SkyrimNetAPI::IsMemorySystemReady()) {
            logger::warn("VisitComposer: SkyrimNet unavailable or memory system not ready");
            callback(std::nullopt);
            return;
        }

        const auto& cfg = Settings::Get();

        // Build the candidate pool. Visits use a stricter viability
        // filter than letters (unique / not in combat / not follower /
        // has current location) but relax the "no memories" gate — visits
        // work from live actor context rather than a memory-driven brief,
        // so a candidate with a thin memory tail can still be a valid
        // sender if the LLM picks them.
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
        opts.requireMemories            = false;  // see comment above
        opts.extraViabilityFilter       = &VisitViabilityFilter;

        auto candidates = SenderCandidatePool::Build(opts);
        if (candidates.empty()) {
            logger::warn(
                "VisitComposer: no viable candidates after filter — declining to compose");
            callback(std::nullopt);
            return;
        }

        const int minRequired = std::max(1, cfg.visitMinSenderCandidates);
        if (static_cast<int>(candidates.size()) < minRequired) {
            logger::warn(
                "VisitComposer: only {} viable candidate(s) (min required {}) — declining to compose",
                candidates.size(), minRequired);
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

        // Capture the set of valid sender FormIDs for post-response
        // validation. We can't trust the LLM to return a FormID we
        // actually offered; validation is client-side.
        std::set<RE::FormID> validSenders;
        for (const auto& c : candidates) validSenders.insert(c.formId);

        const auto promptCtx = BuildPromptContext(
            ctx, urgencyHint, playerName, candidates);
        const auto promptCtxStr = promptCtx.dump();
        if (Settings::Get().debugMode) {
            logger::debug("VisitComposer: prompt context: {}", promptCtxStr);
        }

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
             validSenders = std::move(validSenders),
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

                std::string senderIdStr, briefing, mood, topic;
                if (!getStr("sender_npc_form_id", senderIdStr) ||
                    !getStr("briefing",           briefing)    ||
                    !getStr("mood",               mood)        ||
                    !getStr("topic_tag",          topic)) {
                    logger::warn(
                        "VisitComposer: response missing one of the required keys");
                    callback(std::nullopt);
                    return;
                }

                const RE::FormID senderFormID = ParseHexFormID(senderIdStr);
                if (senderFormID == 0 || !validSenders.contains(senderFormID)) {
                    logger::warn(
                        "VisitComposer: LLM returned sender_npc_form_id '{}' "
                        "which is not in the candidate pool (bad_sender)",
                        senderIdStr);
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

                std::vector<std::string> tags;
                if (auto tagsIt = parsed.find("tags");
                    tagsIt != parsed.end()) {
                    if (!tagsIt->is_array()) {
                        logger::warn(
                            "VisitComposer: `tags` present but not a JSON array; "
                            "treating as empty");
                    } else {
                        tags.reserve(tagsIt->size());
                        for (const auto& t : *tagsIt) {
                            if (!t.is_string()) continue;
                            auto s = LLMTextSanitizer::Sanitize(
                                t.get<std::string>());
                            if (!s.empty()) tags.push_back(std::move(s));
                        }
                    }
                } else {
                    logger::warn(
                        "VisitComposer: response has no `tags` key");
                }

                VisitBriefing briefingOut;
                briefingOut.senderNpcFormID = senderFormID;
                briefingOut.briefing        = std::move(briefing);
                briefingOut.mood            = std::move(mood);
                briefingOut.topicTag        = std::move(topic);
                briefingOut.tags            = std::move(tags);

                logger::info(
                    "VisitComposer: parsed briefing (sender=0x{:X}, "
                    "briefing={} words, mood='{}', topic='{}', tags={})",
                    briefingOut.senderNpcFormID, wc, briefingOut.mood,
                    briefingOut.topicTag, briefingOut.tags.size());
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
