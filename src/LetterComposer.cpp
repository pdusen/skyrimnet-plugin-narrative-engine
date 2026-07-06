#include <LetterComposer.h>

#include <EvaluationPipeline.h>
#include <LLMTextSanitizer.h>
#include <NPCLetterAction.h>
#include <SenderCandidatePool.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <logger.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace NarrativeEngine::LetterComposer
{
    namespace
    {
        // Sender-pool size and per-candidate memory tail caps. Smaller
        // than IntelEngine-style "all relevant NPCs" lists; bounded so
        // the prompt stays cheap.
        //
        // Post-Step-4 (Phase 05): the pool-side over-fetch multiplier
        // and engagement-window constants moved into SenderCandidatePool
        // (Step 4 extraction). LetterComposer's `CollectSenderCandidates`
        // now delegates the engagement walk to SenderCandidatePool::Build
        // and only keeps the caps used by its compose-time memory-fetch
        // path (FetchSenderMemories, called after the sender is
        // pre-chosen and needs a fresh tail with diaries enabled).
        constexpr int kCandidateRenderCap    = 12;
        constexpr int kPerCandidateMemoryCap = 6;

        // How many memories to actually request from SkyrimNet per
        // candidate. SkyrimNet's `PublicGetMemoriesForActor` has no
        // server-side importance filter — it only accepts a maxCount
        // and a semantic-search bias query — so we over-fetch and
        // filter client-side, then truncate to kPerCandidateMemoryCap.
        // The multiplier is a heuristic: at threshold 0.4, most
        // memories in a live actor's tail pass, so 4× gives us
        // headroom without paying much in wasted work when the
        // threshold is loose. If someone raises the threshold hard
        // via INI, they may see shorter tails — that's expected.
        constexpr int kMemoryFetchMultiplier = 4;
        constexpr int kMemoryFetchCap        =
            kPerCandidateMemoryCap * kMemoryFetchMultiplier;

        // How many recent player↔sender dialogue exchanges to include
        // in the compose prompt. SkyrimNet returns them oldest-first
        // and caps at whatever we pass. 25 gives the LLM a fuller
        // running-continuity read on voice / vocabulary / recent
        // topics; entries older than the oldest kept memory are
        // filtered out downstream, so the cap acts as an upper bound,
        // not a target.
        constexpr int kRecentDialogueCap = 25;

        // Valid mood set. The LLM must return one of these; otherwise
        // we treat the response as a validation failure.
        const std::set<std::string>& ValidMoods()
        {
            static const std::set<std::string> kSet{
                "warm", "neutral", "urgent", "menacing", "mournful", "businesslike"
            };
            return kSet;
        }

        // Fetch the player's display name for use as a semantic-search
        // bias query when we pull each candidate's memory tail.
        std::string GetPlayerDisplayName()
        {
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                if (const char* dn = player->GetDisplayFullName()) {
                    return dn;
                }
            }
            return {};
        }

        enum class SenderViability
        {
            Viable,
            MissingActor,
            Dead,
            Disabled,
            SenderCooldown,
            CurrentlyLoaded,
            WalkingDistance,
        };

        // Test whether an actor is within "walking distance" of the
        // player using Skyrim's Location tree. Location is the right
        // abstraction here because it cleanly bridges interior /
        // exterior cells that share the same hub — the Bannered Mare
        // interior and the Whiterun Market exterior are distinct cells
        // (so Is3DLoaded doesn't catch cross-cell proximity) but they
        // both parent up to the Whiterun location. If either actor's
        // location chain, walked up one level, shares a node with the
        // other's, they're at the same hub.
        //
        // The one-level walk (depth <= 1) is deliberately tight. Going
        // deeper would fold in the containing Hold and catch Riverwood
        // NPCs when the player is in Whiterun proper, which stretches
        // "walking distance" past what it should mean.
        //
        // Returns false when either location can't be resolved (the
        // player is in unmarked wilderness, or the actor's cell has no
        // location assignment). Better to fail open than to over-filter
        // on a null-location case we can't reason about.
        bool IsWithinWalkingDistanceOfPlayer(const RE::Actor* actor)
        {
            if (!actor) return false;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return false;

            const auto* playerLoc = player->GetCurrentLocation();
            const auto* actorLoc  = actor->GetCurrentLocation();
            if (!playerLoc || !actorLoc) return false;

            constexpr int kMaxDepth = 1;
            std::set<const RE::BGSLocation*> playerAncestry;
            {
                const auto* p = playerLoc;
                for (int i = 0; i <= kMaxDepth && p; ++i) {
                    playerAncestry.insert(p);
                    p = p->parentLoc;
                }
            }
            const auto* a = actorLoc;
            for (int i = 0; i <= kMaxDepth && a; ++i) {
                if (playerAncestry.contains(a)) return true;
                a = a->parentLoc;
            }
            return false;
        }

        // Test whether a single actor is viable as a letter sender right
        // now: form resolves, actor exists, not dead, not disabled, not
        // on the per-sender cooldown, not currently loaded (present in a
        // loaded cell — see below), and not within walking distance of
        // the player (same location hub — see IsWithinWalkingDistanceOfPlayer).
        // Shared between the pool build and the compose-time
        // re-validation.
        //
        // The `CurrentlyLoaded` and `WalkingDistance` checks overlap
        // in the common case (loaded actors are usually in the same
        // hub) but each catches cases the other doesn't:
        //   - CurrentlyLoaded is triggered by nearby exterior actors
        //     even outside any named location.
        //   - WalkingDistance catches an actor across an interior/
        //     exterior boundary (Whiterun Market NPC vs. player inside
        //     the Bannered Mare) whose 3D isn't loaded.
        // Check loaded first so the more specific reason surfaces in
        // the log when both apply.
        SenderViability CheckSenderViability(RE::FormID formId)
        {
            auto* form  = RE::TESForm::LookupByID(formId);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            if (!actor)                                              return SenderViability::MissingActor;
            if (actor->IsDead())                                     return SenderViability::Dead;
            if (actor->IsDisabled())                                 return SenderViability::Disabled;
            if (actor->Is3DLoaded())                                 return SenderViability::CurrentlyLoaded;
            if (IsWithinWalkingDistanceOfPlayer(actor))              return SenderViability::WalkingDistance;
            if (NPCLetterAction_Cooldowns::IsSenderOnCooldown(formId)) return SenderViability::SenderCooldown;
            return SenderViability::Viable;
        }

        const char* SenderViabilityName(SenderViability v)
        {
            switch (v) {
                case SenderViability::Viable:          return "viable";
                case SenderViability::MissingActor:    return "missing-actor";
                case SenderViability::Dead:            return "dead";
                case SenderViability::Disabled:        return "disabled";
                case SenderViability::SenderCooldown:  return "sender-cooldown";
                case SenderViability::CurrentlyLoaded: return "currently-loaded";
                case SenderViability::WalkingDistance: return "walking-distance";
            }
            return "unknown";
        }

        // Pull a single actor's player-involving memory tail from
        // SkyrimNet, filter by importance floor, and reduce each entry
        // to the minimal shape the prompts render:
        // `{type, content, age_hours, emotion, location}`.
        //
        // Memories with `importance_score` below the configured floor
        // are dropped: SkyrimNet's own retrieval returns a top-N ranked
        // list, but the tail of that list is often incidental chatter
        // that just clutters the prompt without helping the LLM pick
        // a sender or write a letter. `importance_score` itself is not
        // forwarded — the LLM doesn't need a numeric weight once
        // filtering has already gated on it.
        //
        // SkyrimNet's raw memory objects carry ~20 fields — actor_uuid,
        // related_actors, embedding_checksum, condition_expr, pack_id,
        // tags, related_event_ids, and more — that only inflate prompt
        // token count without helping the LLM. Trimming here also pins
        // the schema our prompts depend on: if SkyrimNet renames a
        // field upstream, this function is the single point of
        // adaptation instead of every prompt file.
        //
        // `content`, `emotion`, and `location` are free-form strings
        // and are sanitized per project rule. Returns an empty array
        // on any failure.
        //
        // NOTE: SkyrimNet's PublicAPI.h documents the old field names
        // (`text`, `importance`) that no longer match runtime output;
        // the real names are `content` and `importance_score`. Verified
        // against a captured memory payload — do not "fix" back to the
        // doc names.
        // `includeDiaries` controls the diary-entry filter. Action-
        // select uses false: the LLM only needs the sender's summary
        // memories to judge tonal fit, and long first-person diary
        // rambles just crowd out the shorter, more distinguishing
        // entries the pick should turn on. Compose uses true: the
        // sender's diary is exactly the kind of private, in-voice
        // material a letter-writing prompt should have on hand.
        // Fetch the most recent player↔sender dialogue exchanges from
        // SkyrimNet and trim each entry to `{speaker, text, gameTime}`.
        // `gameTime` is kept on the trimmed shape so downstream can
        // apply the "no dialogue older than the oldest kept memory"
        // filter before rendering; the compose prompt itself doesn't
        // consume it, so callers should strip it once the filter has
        // run. Oldest-first is preserved (SkyrimNet's guarantee). Free-
        // form text fields are sanitized per project rule. Returns an
        // empty array on any failure or when SkyrimNet has no dialogue
        // on file.
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

        // Trim dialogue entries older than the oldest memory in `memories`.
        // Memories carry age relative to now (`age_hours`); dialogue
        // carries absolute game-seconds (`gameTime`). We convert the
        // oldest kept memory's age to a game-seconds cutoff via
        // `RE::Calendar::GetHoursPassed()` (also game-seconds base after
        // × 3600), then drop dialogue entries below the cutoff.
        //
        // If `memories` is empty (no lower bound) or the oldest age
        // resolves to 0 (unknown), no filter is applied — better to
        // show the LLM the full dialogue window than to silently blank
        // it based on a missing field.
        //
        // Preserves `gameTime` on surviving entries; a follow-up call
        // to AnnotateDialogueAges converts it into a rendered age
        // label before the prompt goes out.
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

        // Human-friendly age label. Used as the `[…]` prefix on each
        // rendered dialogue line so the LLM has a sense of how recent
        // each exchange was without having to reason about raw game-
        // seconds. Round to whole hours; pluralize; special-case sub-
        // hour and multi-day ranges so the label reads naturally.
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

        // Compute a rendered age label per dialogue entry (`age_str`)
        // and drop the raw `gameTime` field. Called after the memory-
        // age filter so we only pay the formatting cost on entries the
        // prompt will actually see.
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
                    e["age_str"] = "";
                }
                e.erase("gameTime");
            }
        }

        nlohmann::json FetchSenderMemories(RE::FormID formId,
                                           const std::string& playerName,
                                           bool               includeDiaries)
        {
            // Over-fetch so client-side importance filtering leaves us
            // with roughly kPerCandidateMemoryCap survivors even when
            // the tail of the semantic-search result set has a few
            // low-importance entries. See kMemoryFetchMultiplier above.
            const auto memoriesJson = SkyrimNetAPI::GetMemoriesForActor(
                formId, kMemoryFetchCap, playerName);
            auto raw = nlohmann::json::parse(memoriesJson, nullptr, false);
            if (!raw.is_array()) {
                return nlohmann::json::array();
            }

            const double threshold = static_cast<double>(
                Settings::Get().letterMemoryImportanceThreshold);

            // Collect ALL above-threshold survivors first; do not
            // short-circuit at the render cap here. We select the
            // final N by recency below, and that pick has to see the
            // whole survivor set — stopping at the first N in
            // SkyrimNet's returned (relevance-ordered) list would
            // instead keep the N most-*relevant*, then chronologically
            // sort those, which is the opposite of the intent.
            auto trimmed = nlohmann::json::array();
            int droppedBelowThreshold = 0;
            int droppedDiary          = 0;
            for (auto& m : raw) {
                if (!m.is_object()) continue;

                // Diary-entry filter, applied only when
                // `includeDiaries` is false. SkyrimNet's memory store
                // folds NPC diary entries into the same table that
                // PublicGetMemoriesForActor queries, tagged with the
                // regular `EXPERIENCE` type but with content that
                // opens with `Diary Entry:`. There is no server-side
                // flag; the content prefix is the only reliable
                // discriminator we have. Callers whose consumers want
                // in-voice interior monologue (letter composition)
                // opt in; callers whose consumers just need summary
                // memory shape (action selection) opt out.
                if (!includeDiaries) {
                    if (auto it = m.find("content");
                        it != m.end() && it->is_string()) {
                        const auto& contentRef = it->get_ref<const std::string&>();
                        if (contentRef.rfind("Diary Entry:", 0) == 0) {
                            ++droppedDiary;
                            continue;
                        }
                    }
                }

                double importance = 0.0;
                if (auto it = m.find("importance_score");
                    it != m.end() && it->is_number()) {
                    importance = it->get<double>();
                }
                if (importance < threshold) {
                    ++droppedBelowThreshold;
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
                // `emotion` and `location` are optional on the memory
                // object — SkyrimNet emits `null` when not set. Always
                // materialize as (possibly-empty) strings so the
                // prompt template's `length(...) > 0` guard is safe.
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
            // Sort the kept memories oldest-to-newest so the prompt
            // reads as a chronological narrative. SkyrimNet returns
            // memories ranked by semantic relevance to the search
            // query (the player's name), which is the right lens for
            // *which* memories to consider — but a jumbled order
            // confuses the LLM when it's trying to reason about how
            // the sender's relationship with the player has evolved.
            //
            // Sort ascending on age_hours (newest first) so the
            // truncate step below keeps the most recent survivors,
            // then reverse for oldest-to-newest presentation.
            std::sort(trimmed.begin(), trimmed.end(),
                [](const nlohmann::json& a, const nlohmann::json& b) {
                    const double aAge = a.value("age_hours", 0.0);
                    const double bAge = b.value("age_hours", 0.0);
                    return aAge < bAge;
                });

            // Truncate to the render cap. AFTER the sort so we're
            // keeping the most-recent N above-threshold memories, not
            // the most-relevant N. The `break at cap` shortcut in the
            // collection loop would have given the wrong semantics.
            int droppedBeyondCap = 0;
            if (static_cast<int>(trimmed.size()) > kPerCandidateMemoryCap) {
                droppedBeyondCap =
                    static_cast<int>(trimmed.size()) - kPerCandidateMemoryCap;
                trimmed.erase(
                    trimmed.begin() + kPerCandidateMemoryCap, trimmed.end());
            }

            // Reverse to oldest-first for the LLM's chronological read.
            std::reverse(trimmed.begin(), trimmed.end());

            if ((droppedBelowThreshold > 0 || droppedBeyondCap > 0 ||
                 droppedDiary > 0) && Settings::Get().debugMode) {
                logger::debug(
                    "LetterComposer: sender 0x{:X} — memories kept={}, "
                    "dropped diary={}, dropped below threshold {:.2f}={}, "
                    "dropped as older than the most-recent {}={}",
                    formId, trimmed.size(), droppedDiary,
                    threshold, droppedBelowThreshold,
                    kPerCandidateMemoryCap, droppedBeyondCap);
            }
            return trimmed;
        }
    }

    std::vector<SenderCandidate> CollectSenderCandidates()
    {
        // Delegate the engagement fetch + universal viability walk +
        // memory fetch to SenderCandidatePool, passing letter-specific
        // options (importance threshold, diary-exclude) and the letter-
        // specific extra viability rules (currently-loaded / walking-
        // distance / sender-cooldown).
        SenderCandidatePool::BuildOptions opts;
        opts.maxCandidates              = kCandidateRenderCap;
        opts.maxMemoriesPerCandidate    = kPerCandidateMemoryCap;
        opts.memoryImportanceThreshold  =
            static_cast<double>(Settings::Get().letterMemoryImportanceThreshold);
        opts.excludeDiaryEntries        = true;   // action-select tail
        opts.memoryFetchMultiplier      = kMemoryFetchMultiplier;
        opts.shuffleResult              = true;
        opts.requireMemories            = true;
        opts.extraViabilityFilter =
            [](RE::Actor* actor, std::string* skipReasonOut) -> bool {
                if (!actor) {
                    if (skipReasonOut) *skipReasonOut = "missing-actor";
                    return false;
                }
                if (actor->Is3DLoaded()) {
                    if (skipReasonOut) *skipReasonOut = "currently-loaded";
                    return false;
                }
                if (IsWithinWalkingDistanceOfPlayer(actor)) {
                    if (skipReasonOut) *skipReasonOut = "walking-distance";
                    return false;
                }
                if (NPCLetterAction_Cooldowns::IsSenderOnCooldown(
                        actor->GetFormID())) {
                    if (skipReasonOut) *skipReasonOut = "sender-cooldown";
                    return false;
                }
                return true;
            };

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
            // Hex string so the LLM sees the form in a recognizable
            // format (matches IntelEngine convention).
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

    namespace
    {
        // Build the letter-compose prompt context for a single, already-
        // chosen sender. Fresh memories are supplied by the caller so we
        // don't re-fetch after already validating on the main thread.
        nlohmann::json BuildComposePromptContext(
            const ActionContext&  ctx,
            UrgencyHint           urgencyHint,
            const std::string&    playerName,
            const std::string&    senderName,
            RE::FormID            senderFormID,
            const nlohmann::json& senderMemories,
            const nlohmann::json& recentDialogue)
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

            root["player_name"] = playerName;

            char idBuf[16];
            std::snprintf(idBuf, sizeof(idBuf), "0x%X", senderFormID);
            nlohmann::json sender = nlohmann::json::object();
            sender["form_id"]         = idBuf;
            sender["name"]            = senderName;
            sender["memories"]        = senderMemories;
            sender["recent_dialogue"] = recentDialogue;
            root["sender"]            = std::move(sender);

            // SkyrimNet's system_head / character_profile submodules
            // read the sender from the `npc.UUID` context key — that's
            // the seat they use for `decnpc(...)`, `render_character_profile(...)`,
            // and every downstream personality / bio decorator. Seed
            // it here so `{{ render_subcomponent("system_head", "full") }}`
            // in our prompt resolves against the actual letter sender
            // instead of failing silently or falling back to whoever
            // the ambient dialogue context points at.
            const std::uint64_t senderUUID =
                SkyrimNetAPI::FormIDToUUID(senderFormID);
            if (senderUUID == 0) {
                logger::warn(
                    "LetterComposer: FormIDToUUID(0x{:X}) returned 0; "
                    "system_head submodules will render against a null NPC",
                    senderFormID);
            }
            nlohmann::json npc = nlohmann::json::object();
            npc["UUID"] = senderUUID;
            root["npc"] = std::move(npc);
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
                 RE::FormID           senderNpcFormID,
                 std::function<void(std::optional<LetterComposition>)> callback)
    {
        if (!callback) return;

        if (senderNpcFormID == 0) {
            logger::warn("LetterComposer: Compose called with sender formID=0");
            callback(std::nullopt);
            return;
        }

        if (!SkyrimNetAPI::IsAvailable() || !SkyrimNetAPI::IsMemorySystemReady()) {
            logger::warn("LetterComposer: SkyrimNet unavailable or memory system not ready");
            callback(std::nullopt);
            return;
        }

        // Re-check viability. The action-select round-trip may have
        // taken several seconds; the chosen sender could have died,
        // been disabled, or hit their cooldown in the interim.
        const auto viab = CheckSenderViability(senderNpcFormID);
        if (viab != SenderViability::Viable) {
            logger::warn(
                "LetterComposer: chosen sender 0x{:X} no longer viable ({}); "
                "declining to compose",
                senderNpcFormID, SenderViabilityName(viab));
            callback(std::nullopt);
            return;
        }

        // Resolve the sender's live display name on the main thread —
        // GetDisplayFullName touches engine state. The worker-thread
        // callback needs this later for the label-fallback path, so
        // capture it into the lambda.
        std::string senderName;
        if (auto* form = RE::TESForm::LookupByID(senderNpcFormID)) {
            if (auto* actor = form->As<RE::Actor>()) {
                if (const char* dn = actor->GetDisplayFullName()) {
                    senderName = LLMTextSanitizer::Sanitize(dn);
                }
            }
        }
        if (senderName.empty()) {
            logger::warn(
                "LetterComposer: chosen sender 0x{:X} has no resolvable display name",
                senderNpcFormID);
            callback(std::nullopt);
            return;
        }

        const std::string playerName = GetPlayerDisplayName();

        // Fresh memories at compose time — captures any SkyrimNet
        // events generated between action-select and compose (the
        // round-trip is seconds, not zero).
        const auto memories = FetchSenderMemories(
            senderNpcFormID, playerName, /*includeDiaries=*/true);

        // Most-recent player↔sender dialogue history. Gives the LLM a
        // running-continuity read on how the two of them talk to each
        // other — vocabulary, warmth, formality — beyond what the
        // (third-person) memory tail conveys.
        //
        // Filter dialogue entries older than the oldest kept memory so
        // the two sections stay temporally coherent: if the memory
        // tail only reaches back N hours, a dialogue line from earlier
        // than that would reference events the LLM has no memory
        // context for, and would read as sudden past-life inserts.
        auto recentDialogue = FetchRecentDialogue(senderNpcFormID);
        FilterDialogueByMemoryAge(recentDialogue, memories);
        AnnotateDialogueAges(recentDialogue);

        const auto promptCtx = BuildComposePromptContext(
            ctx, urgencyHint, playerName, senderName,
            senderNpcFormID, memories, recentDialogue);
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

        // Route through the `narrative_engine_composer` variant —
        // NarrativeEngine's second registered SkyrimNet profile,
        // dedicated to creative writing in an NPC's voice. Declared
        // alongside `narrative_engine_director` in the plugin
        // manifest; defaults to inheriting SkyrimNet's base dialogue
        // config, and exposes its own "Composer LLM Overrides"
        // category in the SkyrimNet UI so the user can tune model /
        // temperature / max_tokens independently from the Director's
        // decision-making variant. Any future in-voice prompts
        // (dialogue lines, narrative interstitials, in-world text)
        // should reuse this same variant.
        const bool queued = SkyrimNetAPI::SendCustomPromptToLLM(
            "narrative_engine_letter_compose",
            "narrative_engine_composer",
            promptCtxStr,
            [callback = std::move(callback),
             senderNpcFormID,
             senderName = std::move(senderName),
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

                // Required fields. sender_npc_form_id is NOT expected
                // from the LLM anymore — the sender was chosen at the
                // action-select stage and is captured above.
                auto getStr = [&](const char* key, std::string& out) -> bool {
                    auto it = parsed.find(key);
                    if (it == parsed.end() || !it->is_string()) return false;
                    out = LLMTextSanitizer::Sanitize(it->get<std::string>());
                    return true;
                };

                std::string label, bodyText, mood, topic;
                if (!getStr("body",      bodyText) ||
                    !getStr("mood",      mood) ||
                    !getStr("topic_tag", topic)) {
                    logger::warn("LetterComposer: response missing one of the required keys");
                    callback(std::nullopt);
                    return;
                }
                // `letter_label` is soft-required. Missing / empty
                // / oversize responses fall back to the deterministic
                // label helper rather than failing the whole compose.
                // The LLM's version, when honored, gives more
                // characterful inventory titles ("Ysolda's note", "A
                // sealed letter") than the fallback's
                // `"Note from <name>"` template can produce.
                (void)getStr("letter_label", label);

                // Parse the optional-ish `tags` array. Soft-fail: a
                // missing or malformed value logs a warning and yields
                // an empty vector; the letter itself is still usable
                // without facet tags (topic_tag alone remains the
                // primary subject). Each surviving entry passes
                // through the same sanitizer as every other free-form
                // LLM string in this project; empty-after-sanitize
                // entries are dropped silently.
                std::vector<std::string> tags;
                if (auto tagsIt = parsed.find("tags"); tagsIt != parsed.end()) {
                    if (!tagsIt->is_array()) {
                        logger::warn(
                            "LetterComposer: `tags` present but not a JSON array; "
                            "treating as empty");
                    } else {
                        tags.reserve(tagsIt->size());
                        for (const auto& t : *tagsIt) {
                            if (!t.is_string()) continue;
                            auto s = LLMTextSanitizer::Sanitize(t.get<std::string>());
                            if (!s.empty()) tags.push_back(std::move(s));
                        }
                    }
                } else {
                    logger::warn(
                        "LetterComposer: response has no `tags` key; "
                        "memory writes will fall back to topic_tag alone");
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

                // Engine FULL field is 24 bytes hard. Fall back to
                // the deterministic label helper when the LLM's
                // `letter_label` is missing, empty, or too long.
                // Sanitization above is bytes-in / bytes-out — measure
                // the post-sanitize size.
                if (label.empty() || label.size() > 24) {
                    const std::string original = label;
                    label = SynthesizeFallbackLabel(senderName);
                    if (original.empty()) {
                        logger::info(
                            "LetterComposer: letter_label missing/empty; "
                            "fell back to '{}'",
                            label);
                    } else {
                        logger::info(
                            "LetterComposer: letter_label '{}' exceeded 24-byte cap "
                            "(was {} bytes); fell back to '{}'",
                            original, original.size(), label);
                    }
                }

                LetterComposition comp;
                comp.senderNpcFormID = senderNpcFormID;
                comp.senderLabel     = std::move(label);
                comp.body            = std::move(bodyText);
                comp.mood            = std::move(mood);
                comp.topicTag        = std::move(topic);
                comp.tags            = std::move(tags);

                logger::info(
                    "LetterComposer: composed letter (sender=0x{:X} '{}', "
                    "label='{}', body={} words, mood='{}', topic='{}', tags={})",
                    comp.senderNpcFormID, senderName,
                    comp.senderLabel, wc, comp.mood, comp.topicTag,
                    comp.tags.size());
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
