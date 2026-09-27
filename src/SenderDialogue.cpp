#include <SenderDialogue.h>

#include <JsonUtils.h>
#include <LLMTextSanitizer.h>
#include <logger.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <SkyrimNetEvents.h>

#include <algorithm>
#include <string>
#include <utility>

namespace NarrativeEngine::SenderDialogue
{
    namespace
    {
        // Over-fetch factor. The two-party filter is the whole point of
        // reading events rather than the dialogue endpoint, and it can
        // reject most of a batch: a sender in a busy market is in
        // "dialogue" rows with everyone around them. Asking for `cap`
        // rows would then render two lines when twenty survive further
        // back.
        constexpr int kFetchMultiplier = 4;

        // Ceiling on the over-fetch, so a large render cap cannot ask
        // SkyrimNet for an unbounded batch.
        constexpr int kMaxFetch = 200;

        std::string ToLowerCopy(std::string_view s)
        {
            std::string r;
            r.reserve(s.size());
            for (char c : s) {
                r += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            }
            return r;
        }

        // Names are compared after sanitizing both sides. The row's
        // names come out of SkyrimNet raw, and the caller's come from
        // the sender pool already sanitized — so an NPC whose name
        // carries a typographic apostrophe would otherwise fail to
        // match itself and lose their whole history.
        bool NamesMatch(std::string_view a, std::string_view b)
        {
            if (a.empty() || b.empty()) {
                return false;
            }
            return ToLowerCopy(LLMTextSanitizer::Sanitize(std::string{a}))
                   == ToLowerCopy(LLMTextSanitizer::Sanitize(std::string{b}));
        }

        // SkyrimNet's own documented example row names the player as
        // the literal "Player" rather than by their chosen name, and
        // the sink-side payloads carry the display name. Accept both;
        // a player actually named "Player" costs nothing here.
        bool IsPlayer(std::string_view name, std::string_view playerName)
        {
            return NamesMatch(name, playerName) || NamesMatch(name, "Player");
        }

        // The spoken line off a raw event row.
        //
        // A row from PublicGetRecentEvents has NO `text` field. What it
        // carries is `type` plus a `data` object, and
        // SkyrimNetEvents::FormatEventsText is what synthesizes `text`
        // from the two -- in place, for callers that want the rendered
        // "Speaker -> Listener: line" form. We want the bare line, so
        // we read `data.dialogue` directly and never run that pass.
        //
        // The header's own example row shows a flat `text`, which is
        // why the first cut of this read one and silently dropped every
        // dialogue row it was given. `text` is kept as a fallback for a
        // caller that hands us an already-formatted array, and because
        // costing nothing is the only argument it needs.
        std::string ExtractLine(const nlohmann::json& row)
        {
            if (auto it = row.find("data"); it != row.end() && it->is_object()) {
                if (auto line = JsonUtils::StringOr(*it, "dialogue"); !line.empty()) {
                    return line;
                }
            }
            return JsonUtils::StringOr(row, "text");
        }
    } // namespace

    nlohmann::json Fetch(RE::FormID senderFormID, std::string_view senderName, std::string_view playerName, int cap)
    {
        auto out = nlohmann::json::array();
        const int safeCap = std::max(0, cap);
        if (senderFormID == 0 || safeCap == 0 || senderName.empty()) {
            return out;
        }

        const int fetchCount = std::min(safeCap * kFetchMultiplier, kMaxFetch);
        const auto raw = SkyrimNetAPI::GetRecentEvents(senderFormID, fetchCount, kDialogueEventTypes);
        auto parsed = nlohmann::json::parse(raw, nullptr, false);
        if (!parsed.is_array()) {
            return out;
        }

        int droppedThirdParty = 0;
        int droppedUnattributable = 0;
        int droppedNoLine = 0;

        for (const auto& e : parsed) {
            if (!e.is_object()) {
                continue;
            }

            // JsonUtils::StringOr rather than value(): SkyrimNet emits
            // explicit nulls as a matter of course, and value() throws
            // on a present-but-null key rather than falling back.
            const std::string origin = JsonUtils::StringOr(e, "originatingActorName");
            const std::string target = JsonUtils::StringOr(e, "targetActorName");
            if (origin.empty() || target.empty()) {
                // No counterparty means the line cannot be attributed,
                // and an unattributable line is exactly the one that
                // must not reach the prompt.
                ++droppedUnattributable;
                continue;
            }

            const bool playerToSender = IsPlayer(origin, playerName) && NamesMatch(target, senderName);
            const bool senderToPlayer = NamesMatch(origin, senderName) && IsPlayer(target, playerName);
            if (!playerToSender && !senderToPlayer) {
                ++droppedThirdParty;
                continue;
            }

            auto text = LLMTextSanitizer::Sanitize(ExtractLine(e));
            if (text.empty()) {
                // Counted, not silent. A two-party row we could not read
                // a line out of is the shape of a payload change, and
                // the first version of this dropped 31 of them per call
                // without the tally saying so.
                ++droppedNoLine;
                continue;
            }
            auto speaker = LLMTextSanitizer::Sanitize(origin);
            if (speaker.empty()) {
                continue;
            }

            nlohmann::json row = nlohmann::json::object();
            row["speaker"] = std::move(speaker);
            row["text"] = std::move(text);
            row["gameTime"] = JsonUtils::NumberOr(e, "gameTime", 0.0);
            out.push_back(std::move(row));
        }

        // Sort rather than trust the source order: the dialogue
        // endpoint documents oldest-first, the events one documents no
        // ordering at all, and the age filter downstream compares
        // against this field anyway.
        auto& arr = out.get_ref<nlohmann::json::array_t&>();
        std::stable_sort(arr.begin(), arr.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
            return a.value("gameTime", 0.0) < b.value("gameTime", 0.0);
        });

        // Keep the newest `cap`, not the first `cap`. Trimming from the
        // front would hand the prompt the oldest exchanges on file and
        // drop the conversation the sender is actually reacting to.
        if (arr.size() > static_cast<std::size_t>(safeCap)) {
            arr.erase(arr.begin(), arr.end() - safeCap);
        }

        if (Settings::Get().debugMode) {
            logger::debug("SenderDialogue: sender 0x{:X} — {} of {} rows kept "
                          "(dropped: third-party={}, unattributable={}, no-line={})",
                          senderFormID,
                          arr.size(),
                          parsed.size(),
                          droppedThirdParty,
                          droppedUnattributable,
                          droppedNoLine);
        }
        return out;
    }

    void FilterByMemoryAge(nlohmann::json& dialogue, const nlohmann::json& memories, double nowGameSeconds)
    {
        if (!dialogue.is_array() || dialogue.empty() || nowGameSeconds <= 0.0) {
            return;
        }

        // `age_seconds` is the shaped memory's IN-WORLD age. It used to
        // be `age_hours`, which is real elapsed time — mixing that with
        // the game clock put the cutoff arbitrarily far in the past, so
        // the filter silently did nothing on any save resumed after a
        // break.
        double oldestMemoryAgeSeconds = 0.0;
        if (memories.is_array()) {
            for (const auto& m : memories) {
                const double s = m.value("age_seconds", 0.0);
                if (s > oldestMemoryAgeSeconds) {
                    oldestMemoryAgeSeconds = s;
                }
            }
        }
        if (oldestMemoryAgeSeconds <= 0.0) {
            return;
        }

        const double cutoffGameSeconds = nowGameSeconds - oldestMemoryAgeSeconds;
        auto& arr = dialogue.get_ref<nlohmann::json::array_t&>();
        arr.erase(std::remove_if(arr.begin(),
                                 arr.end(),
                                 [cutoffGameSeconds](const nlohmann::json& e) {
                                     return e.value("gameTime", 0.0) < cutoffGameSeconds;
                                 }),
                  arr.end());
    }

    void AnnotateAges(nlohmann::json& dialogue, double nowGameSeconds)
    {
        if (!dialogue.is_array() || dialogue.empty()) {
            return;
        }
        for (auto& e : dialogue) {
            if (!e.is_object()) {
                continue;
            }
            const double gt = e.value("gameTime", 0.0);
            if (nowGameSeconds > 0.0 && gt > 0.0) {
                // Canonical formatter, shared with events and memories,
                // so every age the prompt shows reads the same.
                e["age_str"] = SkyrimNetEvents::FormatRelativeGameTime(std::max(0.0, nowGameSeconds - gt));
            } else {
                e["age_str"] = std::string{"recent"};
            }
            e.erase("gameTime");
        }
    }
} // namespace NarrativeEngine::SenderDialogue
