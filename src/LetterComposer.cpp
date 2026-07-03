#include <LetterComposer.h>

#include <EvaluationPipeline.h>
#include <LLMTextSanitizer.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <logger.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NarrativeEngine::LetterComposer
{
    namespace
    {
        // Sender-pool size and per-candidate memory tail caps. Smaller
        // than IntelEngine-style "all relevant NPCs" lists; bounded so
        // the prompt stays cheap.
        constexpr int kCandidateCap          = 12;
        constexpr int kPerCandidateMemoryCap = 6;

        // SkyrimNet engagement-window defaults. Short = 1 game-day,
        // medium = 7 game-days. Plays well with the
        // recent-vs-historical scoring SkyrimNet does internally.
        constexpr double kShortWindowSeconds  = 86400.0;
        constexpr double kMediumWindowSeconds = 604800.0;

        // Valid mood set. The LLM must return one of these; otherwise
        // we treat the response as a validation failure.
        const std::set<std::string>& ValidMoods()
        {
            static const std::set<std::string> kSet{
                "warm", "neutral", "urgent", "menacing", "mournful", "businesslike"
            };
            return kSet;
        }

        // Mirror of the SkyrimNet engagement entry we care about.
        struct Candidate
        {
            std::uint32_t      formId = 0;
            std::string        name;
            double             engagementScore   = 0.0;
            double             lastInteractedAt  = 0.0;
            nlohmann::json     memories = nlohmann::json::array();
        };

        // Build the candidate pool by calling SkyrimNet's engagement
        // ranker, then pulling each top candidate's player-involving
        // memory tail. Must run on the main thread (the action's Start
        // is on the main thread; the SkyrimNet calls themselves are
        // safe off-thread but the action call sites are simpler if we
        // bundle them with the rest of the main-thread setup).
        std::vector<Candidate> CollectCandidates()
        {
            std::vector<Candidate> out;

            // Pull the live player name once; SkyrimNet's
            // PublicGetMemoriesForActor takes this as a semantic-search
            // bias query, so it has to be the actual player character's
            // name — not a literal "Dragonborn" string that won't match
            // anything for players who aren't on the main-quest path.
            std::string playerName;
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                if (const char* dn = player->GetDisplayFullName()) {
                    playerName = dn;
                }
            }

            const auto enrolledJson = SkyrimNetAPI::GetActorEngagement(
                kCandidateCap, /*excludePlayer=*/true,
                /*playerEventsOnly=*/false,
                kShortWindowSeconds, kMediumWindowSeconds);
            auto enrolled = nlohmann::json::parse(enrolledJson, nullptr, false);
            if (enrolled.is_discarded() || !enrolled.is_array()) {
                logger::warn(
                    "LetterComposer: GetActorEngagement returned non-array; "
                    "raw='{}'", enrolledJson);
                return out;
            }

            int skippedDead     = 0;
            int skippedDisabled = 0;
            int skippedMissing  = 0;
            for (auto& entry : enrolled) {
                if (!entry.is_object()) continue;
                Candidate c;
                if (auto it = entry.find("formId"); it != entry.end() && it->is_number_unsigned()) {
                    c.formId = it->get<std::uint32_t>();
                }
                if (c.formId == 0) continue;

                // Viability gate. A letter sender must be a live, enabled
                // actor in the world right now — the per-slot delivery
                // quest's `LetterRef` alias uses "Create Reference to
                // Object" with Create-In = Sender, and the engine can only
                // spawn a REFR into an instantiated, enabled, non-corpse
                // inventory. Dead or disabled candidates pass selection but
                // then silently fail the alias-fill, leaving `LetterRef`
                // empty and stranding the dispatch. Filter them out here.
                auto* form  = RE::TESForm::LookupByID(c.formId);
                auto* actor = form ? form->As<RE::Actor>() : nullptr;
                if (!actor) {
                    ++skippedMissing;
                    continue;
                }
                if (actor->IsDead()) {
                    ++skippedDead;
                    continue;
                }
                if (actor->IsDisabled()) {
                    ++skippedDisabled;
                    continue;
                }

                if (auto it = entry.find("name"); it != entry.end() && it->is_string()) {
                    // SkyrimNet-returned text — sanitize before we cache or
                    // forward to the prompt.
                    c.name = LLMTextSanitizer::Sanitize(it->get<std::string>());
                }
                if (c.name.empty()) continue;
                if (auto it = entry.find("totalMemoryImportance"); it != entry.end() && it->is_number()) {
                    c.engagementScore = it->get<double>();
                }
                if (auto it = entry.find("lastEventTime"); it != entry.end() && it->is_number()) {
                    c.lastInteractedAt = it->get<double>();
                }

                const auto memoriesJson = SkyrimNetAPI::GetMemoriesForActor(
                    c.formId, kPerCandidateMemoryCap, playerName);
                auto memories = nlohmann::json::parse(memoriesJson, nullptr, false);
                if (memories.is_array()) {
                    // Sanitize free-form text inside each memory entry
                    // (the rest are numeric / typed enums and pass
                    // through unchanged).
                    for (auto& m : memories) {
                        if (!m.is_object()) continue;
                        if (auto it = m.find("text");
                            it != m.end() && it->is_string()) {
                            *it = LLMTextSanitizer::Sanitize(it->get<std::string>());
                        }
                    }
                    c.memories = std::move(memories);
                }
                out.push_back(std::move(c));
            }

            if (skippedMissing || skippedDead || skippedDisabled) {
                logger::info(
                    "LetterComposer: filtered candidates (kept={}, "
                    "skipped: missing-actor={}, dead={}, disabled={})",
                    out.size(), skippedMissing, skippedDead, skippedDisabled);
            }
            return out;
        }

        nlohmann::json BuildPromptContext(const ActionContext&            ctx,
                                          UrgencyHint                     urgencyHint,
                                          const std::vector<Candidate>&   candidates)
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
            root["min_words"] = cfg.letterContentMinWords;
            root["max_words"] = cfg.letterContentMaxWords;

            auto candidatesJson = nlohmann::json::array();
            for (const auto& c : candidates) {
                nlohmann::json cj = nlohmann::json::object();
                // Hex string so the LLM sees the form in a recognizable
                // format (matches IntelEngine convention).
                char idBuf[16];
                std::snprintf(idBuf, sizeof(idBuf), "0x%X", c.formId);
                cj["form_id"]            = idBuf;
                cj["name"]               = c.name;
                cj["engagement_score"]   = c.engagementScore;
                cj["last_interacted_at"] = c.lastInteractedAt;
                cj["memories"]           = c.memories;
                candidatesJson.push_back(std::move(cj));
            }
            root["sender_candidates"] = std::move(candidatesJson);
            return root;
        }

        // Count whitespace-separated words. Cheap; doesn't need to be
        // Unicode-aware because we already sanitized the body.
        std::size_t WordCount(const std::string& s)
        {
            std::size_t n = 0;
            bool inWord = false;
            for (char c : s) {
                const bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
                if (!ws && !inWord) { ++n; inWord = true; }
                else if (ws)        { inWord = false; }
            }
            return n;
        }

        // Helper: case-insensitive whole-word prefix match. Returns
        // index past the prefix (and any following whitespace) if the
        // string begins with `prefix`+whitespace, else 0.
        std::size_t PrefixSkip(const std::string& s, std::string_view prefix)
        {
            if (s.size() <= prefix.size()) return 0;
            for (std::size_t i = 0; i < prefix.size(); ++i) {
                const char a = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(s[i])));
                const char b = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(prefix[i])));
                if (a != b) return 0;
            }
            const char nextCh = s[prefix.size()];
            if (nextCh != ' ' && nextCh != '\t') return 0;
            std::size_t i = prefix.size();
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
            return i;
        }

        // Strip a trailing " the X" title, case-insensitive. Returns
        // the original name if no match.
        std::string StripSuffixTitle(const std::string& name)
        {
            // Find the last " the " (case-insensitive), then drop it
            // and everything after.
            const std::string lower = [&] {
                std::string l = name;
                for (auto& c : l) c = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
                return l;
            }();
            const auto pos = lower.rfind(" the ");
            if (pos == std::string::npos) return name;
            // Make sure there's a word after " the " — otherwise the
            // " the " is mid-name (like "Wuuthrad of the Ysmir") and
            // we shouldn't strip.
            if (pos + 5 >= name.size()) return name;
            return name.substr(0, pos);
        }

        // Strip a leading title prefix. Returns the original name if
        // no match.
        std::string StripPrefixTitle(const std::string& name)
        {
            static constexpr std::array<std::string_view, 14> kPrefixes{
                "Jarl",      "Thane",     "Master",    "Mistress",
                "Captain",   "Commander", "Housecarl", "Lord",
                "Lady",      "Sir",       "Brother",   "Sister",
                "Mother",    "Father",
            };
            for (auto p : kPrefixes) {
                const auto skip = PrefixSkip(name, p);
                if (skip > 0) return name.substr(skip);
            }
            return name;
        }
    }

    std::string SynthesizeFallbackLabel(const std::string& senderFullName)
    {
        // Note: deliberately leave `senderFullName` un-trimmed beyond
        // sanitization — leading / trailing whitespace inside the name
        // itself would already have been stripped by LLMTextSanitizer
        // upstream.
        static constexpr std::string_view kPrefix = "Note from ";
        constexpr std::size_t kBudget = 24;
        constexpr std::size_t kNameRoom = kBudget - kPrefix.size();  // = 14

        auto tryName = [&](const std::string& name) -> std::optional<std::string> {
            if (name.empty()) return std::nullopt;
            if (name.size() <= kNameRoom) {
                std::string out;
                out.reserve(kPrefix.size() + name.size());
                out.append(kPrefix);
                out.append(name);
                return out;
            }
            return std::nullopt;
        };

        // Candidate 1: full name as-is.
        if (auto r = tryName(senderFullName)) return *r;

        // Candidate 2: SkyrimNet "ShortName" — not currently exposed
        // through PublicAPI.h in a way callable from a worker-thread
        // callback. Skipping for now; documented in the phase plan as
        // a known omission. If/when SkyrimNet exposes it, plumb here.

        // Candidate 3: strip trailing " the X" title.
        const auto suffixStripped = StripSuffixTitle(senderFullName);
        if (suffixStripped != senderFullName) {
            if (auto r = tryName(suffixStripped)) return *r;
        }

        // Candidate 4: also strip leading title prefix.
        const auto bothStripped = StripPrefixTitle(suffixStripped);
        if (bothStripped != suffixStripped) {
            if (auto r = tryName(bothStripped)) return *r;
        }

        // Candidate 5: build "Note from <bothStripped>" and right-
        // truncate the whole string to 24 bytes. Guarantees a result.
        std::string out;
        out.reserve(kBudget);
        out.append(kPrefix);
        out.append(bothStripped.empty() ? senderFullName : bothStripped);
        if (out.size() > kBudget) out.resize(kBudget);
        return out;
    }

    void Compose(const ActionContext& ctx,
                 UrgencyHint          urgencyHint,
                 std::function<void(std::optional<LetterComposition>)> callback)
    {
        if (!callback) return;

        if (!SkyrimNetAPI::IsAvailable() || !SkyrimNetAPI::IsMemorySystemReady()) {
            logger::warn("LetterComposer: SkyrimNet unavailable or memory system not ready");
            callback(std::nullopt);
            return;
        }

        auto candidates = CollectCandidates();
        if (static_cast<int>(candidates.size()) < Settings::Get().letterMinSenderCandidates) {
            logger::warn(
                "LetterComposer: only {} sender candidates available; "
                "minimum is {} — declining to compose",
                candidates.size(),
                Settings::Get().letterMinSenderCandidates);
            callback(std::nullopt);
            return;
        }

        // Side table: form_id → cached display name, so the worker-
        // thread callback can compute the sender-label fallback without
        // touching the engine.
        std::unordered_map<std::uint32_t, std::string> nameByFormID;
        for (const auto& c : candidates) {
            nameByFormID.emplace(c.formId, c.name);
        }

        const auto promptCtx = BuildPromptContext(ctx, urgencyHint, candidates);
        const auto promptCtxStr = promptCtx.dump();
        if (Settings::Get().debugMode) {
            logger::debug("LetterComposer: prompt context: {}", promptCtxStr);
        }

        const auto& cfg = Settings::Get();
        const int   minWords = cfg.letterContentMinWords;
        const int   maxWords = cfg.letterContentMaxWords;

        // Clone the callback before move so the !queued failure path
        // below can still notify the caller. SkyrimNet's
        // SendCustomPromptToLLM doesn't fire the callback on a queue
        // failure, so without this backup the caller would wait
        // forever for a result that never comes.
        auto callbackBackup = callback;

        const bool queued = SkyrimNetAPI::SendCustomPromptToLLM(
            "narrative_engine_letter_compose",
            "narrative_engine_director",
            promptCtxStr,
            [callback = std::move(callback),
             nameByFormID = std::move(nameByFormID),
             minWords, maxWords]
            (std::string response, bool success) mutable
            {
                if (!success) {
                    logger::warn("LetterComposer: LLM call failed: {}", response);
                    callback(std::nullopt);
                    return;
                }
                if (Settings::Get().debugMode) {
                    logger::debug("LetterComposer: raw response: {}", response);
                }

                const auto body = EvaluationPipeline::StripMarkdownFences(response);
                auto parsed = nlohmann::json::parse(body, nullptr, false);
                if (parsed.is_discarded() || !parsed.is_object()) {
                    logger::warn("LetterComposer: response not a JSON object: {}", body);
                    callback(std::nullopt);
                    return;
                }

                // Required fields.
                auto getStr = [&](const char* key, std::string& out) -> bool {
                    auto it = parsed.find(key);
                    if (it == parsed.end() || !it->is_string()) return false;
                    out = LLMTextSanitizer::Sanitize(it->get<std::string>());
                    return true;
                };

                std::string idStr, label, bodyText, mood, topic;
                if (!getStr("sender_npc_form_id", idStr) ||
                    !getStr("sender_label",       label) ||
                    !getStr("body",               bodyText) ||
                    !getStr("mood",               mood) ||
                    !getStr("topic_tag",          topic)) {
                    logger::warn("LetterComposer: response missing one of the required keys");
                    callback(std::nullopt);
                    return;
                }

                // sender_npc_form_id must be a hex form ID that matches
                // one of the candidates.
                std::uint32_t senderFormID = 0;
                try {
                    senderFormID = static_cast<std::uint32_t>(
                        std::stoul(idStr, nullptr, /*base=*/0));
                } catch (...) {
                    logger::warn("LetterComposer: sender_npc_form_id unparseable: '{}'", idStr);
                    callback(std::nullopt);
                    return;
                }
                auto nameIt = nameByFormID.find(senderFormID);
                if (nameIt == nameByFormID.end()) {
                    logger::warn(
                        "LetterComposer: sender_npc_form_id 0x{:X} not in candidate set",
                        senderFormID);
                    callback(std::nullopt);
                    return;
                }

                // Mood must be one of the known set.
                if (!ValidMoods().contains(mood)) {
                    logger::warn("LetterComposer: invalid mood '{}'", mood);
                    callback(std::nullopt);
                    return;
                }

                // Body word count in bounds.
                const auto wc = static_cast<int>(WordCount(bodyText));
                if (wc < minWords || wc > maxWords) {
                    logger::warn(
                        "LetterComposer: body word count {} outside [{}..{}]",
                        wc, minWords, maxWords);
                    callback(std::nullopt);
                    return;
                }

                // sender_label cap (per docs/LLM_RESPONSE_HANDLING.md
                // and the phase plan's "Sender-label fallback").
                // Sanitization above is bytes-in / bytes-out — measure
                // the post-sanitize size. Engine FULL field is 24
                // bytes hard.
                if (label.size() > 24) {
                    const std::string original = label;
                    label = SynthesizeFallbackLabel(nameIt->second);
                    logger::info(
                        "LetterComposer: sender_label '{}' exceeded 24-byte cap "
                        "(was {} bytes); fell back to '{}'",
                        original, original.size(), label);
                }

                LetterComposition comp;
                comp.senderNpcFormID = senderFormID;
                comp.senderLabel     = std::move(label);
                comp.body            = std::move(bodyText);
                comp.mood            = std::move(mood);
                comp.topicTag        = std::move(topic);

                logger::info(
                    "LetterComposer: composed letter (sender=0x{:X} '{}', "
                    "label='{}', body={} words, mood='{}', topic='{}')",
                    comp.senderNpcFormID, nameIt->second,
                    comp.senderLabel, wc, comp.mood, comp.topicTag);
                callback(std::move(comp));
            });

        if (!queued) {
            logger::warn(
                "LetterComposer: SendCustomPromptToLLM returned false; "
                "treating as failure");
            if (callbackBackup) callbackBackup(std::nullopt);
        }
    }
}
