#include <SenderCandidatePool.h>

#include <LLMTextSanitizer.h>
#include <SkyrimNetAPI.h>
#include <logger.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <random>
#include <utility>

namespace NarrativeEngine::SenderCandidatePool
{
    namespace
    {
        // How many raw engagement entries to fetch. Multiplier over the
        // requested `maxCandidates` so client-side filtering still leaves
        // us with roughly `maxCandidates` survivors even when several
        // entries fail viability. Matches LetterComposer's original 3x
        // headroom.
        constexpr int kEngagementFetchMultiplier = 3;

        // SkyrimNet engagement-window defaults (in game-seconds).
        //   Short  = 1 game-day  = 86400s
        //   Medium = 7 game-days = 604800s
        // Pass-through into GetActorEngagement; SkyrimNet uses these to
        // weight recent-vs-historical engagement scoring.
        constexpr double kShortWindowSeconds  = 86400.0;
        constexpr double kMediumWindowSeconds = 604800.0;

        // Trim a raw memory tail per the caller's importance / diary
        // rules and reshape each entry into the compact
        // `{type, content, age_hours, emotion, location}` shape both
        // compose prompts consume.
        //
        // Returns a JSON array (possibly empty). Preserves the same
        // sort-and-truncate discipline LetterComposer used: sort by
        // ascending age_hours (newest first), truncate to
        // maxMemoriesPerCandidate, then reverse for
        // oldest-to-newest presentation.
        nlohmann::json FilterAndShapeMemories(nlohmann::json      raw,
                                              const BuildOptions& opts)
        {
            if (!raw.is_array()) {
                return nlohmann::json::array();
            }

            auto trimmed = nlohmann::json::array();
            for (auto& m : raw) {
                if (!m.is_object()) continue;

                // Diary-entry filter. SkyrimNet folds diary entries into
                // the same table PublicGetMemoriesForActor queries, tagged
                // with regular `EXPERIENCE` type but whose content starts
                // with `Diary Entry:`. There's no server-side flag; the
                // content prefix is the only discriminator we have.
                if (opts.excludeDiaryEntries) {
                    if (auto it = m.find("content");
                        it != m.end() && it->is_string()) {
                        const auto& contentRef = it->get_ref<const std::string&>();
                        if (contentRef.rfind("Diary Entry:", 0) == 0) {
                            continue;
                        }
                    }
                }

                double importance = 0.0;
                if (auto it = m.find("importance_score");
                    it != m.end() && it->is_number()) {
                    importance = it->get<double>();
                }
                if (importance < opts.memoryImportanceThreshold) {
                    continue;
                }

                nlohmann::json out = nlohmann::json::object();
                if (auto it = m.find("type"); it != m.end() && it->is_string()) {
                    out["type"] = it->get<std::string>();
                }
                if (auto it = m.find("content"); it != m.end() && it->is_string()) {
                    out["content"] = LLMTextSanitizer::Sanitize(it->get<std::string>());
                }
                if (auto it = m.find("age_hours"); it != m.end() && it->is_number()) {
                    out["age_hours"] = it->get<double>();
                }
                std::string emotion;
                if (auto it = m.find("emotion");
                    it != m.end() && it->is_string()) {
                    emotion = LLMTextSanitizer::Sanitize(it->get<std::string>());
                }
                out["emotion"] = std::move(emotion);
                std::string location;
                if (auto it = m.find("location");
                    it != m.end() && it->is_string()) {
                    location = LLMTextSanitizer::Sanitize(it->get<std::string>());
                }
                out["location"] = std::move(location);
                trimmed.push_back(std::move(out));
            }

            // Sort ascending on age_hours (newest first) so truncation
            // keeps the most recent survivors, then reverse for
            // oldest-to-newest presentation.
            std::sort(trimmed.begin(), trimmed.end(),
                [](const nlohmann::json& a, const nlohmann::json& b) {
                    return a.value("age_hours", 0.0) < b.value("age_hours", 0.0);
                });

            const int cap = std::max(0, opts.maxMemoriesPerCandidate);
            if (static_cast<int>(trimmed.size()) > cap) {
                trimmed.erase(trimmed.begin() + cap, trimmed.end());
            }
            std::reverse(trimmed.begin(), trimmed.end());
            return trimmed;
        }

        // Fetch a candidate's memory tail from SkyrimNet, then run it
        // through FilterAndShapeMemories. Returns an empty array on any
        // failure or when SkyrimNet reports nothing for this actor.
        nlohmann::json FetchAndShapeMemories(RE::FormID          formId,
                                              const std::string&  playerName,
                                              const BuildOptions& opts)
        {
            const int fetchCap =
                std::max(1, opts.maxMemoriesPerCandidate *
                            std::max(1, opts.memoryFetchMultiplier));

            const auto raw = SkyrimNetAPI::GetMemoriesForActor(
                formId, fetchCap, playerName);
            auto parsed = nlohmann::json::parse(raw, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_array()) {
                return nlohmann::json::array();
            }
            return FilterAndShapeMemories(std::move(parsed), opts);
        }

        // Common walker: fetches engagement, then for each entry runs the
        // universal viability walk (form / actor / dead / disabled /
        // name) plus the optional extra filter. Calls `onViable(actor,
        // entryJson)` for each survivor. Stops when `stopWhen` returns
        // true after a survivor (allows early-exit for CountViable).
        template <typename OnViable, typename StopWhen>
        void WalkEngagement(int                    fetchCap,
                             const ViabilityFilter& extraFilter,
                             OnViable&&             onViable,
                             StopWhen&&             stopWhen)
        {
            const auto enrolledJson = SkyrimNetAPI::GetActorEngagement(
                fetchCap, /*excludePlayer=*/true,
                /*playerEventsOnly=*/false,
                kShortWindowSeconds, kMediumWindowSeconds);

            auto enrolled = nlohmann::json::parse(enrolledJson, nullptr, false);
            if (enrolled.is_discarded() || !enrolled.is_array()) {
                logger::warn(
                    "SenderCandidatePool: GetActorEngagement returned non-array; "
                    "raw='{}'",
                    enrolledJson);
                return;
            }

            for (auto& entry : enrolled) {
                if (!entry.is_object()) continue;

                RE::FormID formId = 0;
                if (auto it = entry.find("formId");
                    it != entry.end() && it->is_number_unsigned()) {
                    formId = it->get<std::uint32_t>();
                }
                if (formId == 0) continue;

                auto* form  = RE::TESForm::LookupByID(formId);
                auto* actor = form ? form->As<RE::Actor>() : nullptr;
                if (!actor)           continue;
                if (actor->IsDead())    continue;
                if (actor->IsDisabled()) continue;

                // Name is required for the LLM to identify the candidate.
                std::string name;
                if (auto it = entry.find("name");
                    it != entry.end() && it->is_string()) {
                    name = LLMTextSanitizer::Sanitize(it->get<std::string>());
                }
                if (name.empty()) continue;

                if (extraFilter) {
                    std::string skipReason;
                    if (!extraFilter(actor, &skipReason)) {
                        continue;
                    }
                }

                onViable(actor, entry, std::move(name));
                if (stopWhen()) return;
            }
        }
    }

    std::string GetPlayerDisplayName()
    {
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (const char* dn = player->GetDisplayFullName()) {
                return dn;
            }
        }
        return {};
    }

    std::vector<Candidate> Build(const BuildOptions& opts)
    {
        std::vector<Candidate> out;

        if (!SkyrimNetAPI::IsAvailable() || !SkyrimNetAPI::IsMemorySystemReady()) {
            return out;
        }

        const int renderCap = std::max(1, opts.maxCandidates);
        const int fetchCap  = renderCap * kEngagementFetchMultiplier;

        const std::string playerName = GetPlayerDisplayName();

        int skippedNoMemories = 0;

        WalkEngagement(
            fetchCap, opts.extraViabilityFilter,
            [&](RE::Actor* actor, const nlohmann::json& entry, std::string name) {
                if (static_cast<int>(out.size()) >= renderCap) return;

                Candidate c;
                c.formId = actor->GetFormID();
                c.name   = std::move(name);
                if (auto it = entry.find("totalMemoryImportance");
                    it != entry.end() && it->is_number()) {
                    c.engagementScore = it->get<double>();
                }
                if (auto it = entry.find("lastEventTime");
                    it != entry.end() && it->is_number()) {
                    c.lastInteractedAt = it->get<double>();
                }

                c.memories = FetchAndShapeMemories(c.formId, playerName, opts);
                if (opts.requireMemories) {
                    if (!c.memories.is_array() || c.memories.empty()) {
                        ++skippedNoMemories;
                        return;
                    }
                }

                out.push_back(std::move(c));
            },
            [&]() { return static_cast<int>(out.size()) >= renderCap; });

        if (skippedNoMemories > 0) {
            logger::info(
                "SenderCandidatePool: kept={}, dropped-no-significant-memories={}",
                out.size(), skippedNoMemories);
        }

        if (opts.shuffleResult && out.size() > 1) {
            std::random_device rd;
            std::mt19937       rng(rd());
            std::shuffle(out.begin(), out.end(), rng);
        }
        return out;
    }

    std::size_t CountViable(const ViabilityFilter& extraFilter,
                             std::size_t            min)
    {
        if (!SkyrimNetAPI::IsAvailable() || !SkyrimNetAPI::IsMemorySystemReady()) {
            return 0;
        }

        const int fetchCap = std::max<int>(
            static_cast<int>(min), 1) * kEngagementFetchMultiplier;

        std::size_t count = 0;
        WalkEngagement(
            fetchCap, extraFilter,
            [&](RE::Actor* /*actor*/, const nlohmann::json& /*entry*/,
                std::string /*name*/) {
                ++count;
            },
            [&]() { return count >= min; });
        return count;
    }
}
