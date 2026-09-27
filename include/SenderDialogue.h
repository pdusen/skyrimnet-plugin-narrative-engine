#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

#include <RE/Skyrim.h>

// SenderDialogue — the spoken history between the player and one
// sender, fetched and shaped for a compose prompt. Shared by
// LetterComposer and VisitComposer, which each carried their own copy
// of this until the two-party filter below made keeping them in step a
// correctness requirement rather than a tidiness one.
//
// Why this reads events rather than SkyrimNet's dialogue endpoint:
//
//   PublicGetRecentDialogue(formId, n) returns `{speaker, text,
//   gameTime}` and nothing else. There is no counterparty on the row,
//   so "the player said this TO the sender" and "the player said this
//   in front of the sender, to somebody else" are indistinguishable —
//   and so are the sender's own lines to a third party. The endpoint's
//   docstring says it returns dialogue "between the player and an
//   NPC", but a filter that has to trust that claim cannot be written,
//   only assumed.
//
//   PublicGetRecentEvents(formId, n, "dialogue,...") carries
//   `originatingActorName` and `targetActorName` on every row, so the
//   same question is answerable from the data. An entry survives here
//   only when it is the player speaking to this sender or this sender
//   speaking to the player. Everything else — ambient chatter, the
//   sender talking to a third NPC, the player addressing somebody else
//   within earshot — is dropped before the prompt is built.
//
// Threading: no engine access, so no token. The fetch is a SkyrimNet
// DLL call; the two shaping functions are pure over their arguments,
// which is why they take `nowGameSeconds` rather than reading
// RE::Calendar themselves. Call sites pass
// EventLogUtil::NowGameTimeSeconds().
namespace NarrativeEngine::SenderDialogue
{
    // The event types that carry a spoken line. `dialogue_background`
    // is deliberately absent: it is ambient NPC-to-NPC chatter, which
    // the two-party filter would drop anyway, so asking for it only
    // spends fetch budget on rows that cannot survive.
    inline constexpr const char* kDialogueEventTypes = "dialogue,dialogue_player_text";

    // Up to `cap` of the most recent two-party exchanges, oldest first,
    // as an array of `{speaker, text, gameTime}`. `speaker` is the
    // originating actor's display name, sanitized; `gameTime` is
    // absolute game-seconds, left on the row for FilterByMemoryAge and
    // stripped by AnnotateAges.
    //
    // `senderName` and `playerName` are what the row's actor names are
    // matched against, case-insensitively and after sanitizing both
    // sides — an accented or smart-quoted name would otherwise fail to
    // match itself. The literal "Player" also counts as the player,
    // since that is what SkyrimNet's own example row carries.
    //
    // Returns an empty array on any failure, and on the ordinary case
    // of two people who have never spoken. A caller cannot tell those
    // apart and does not need to: both mean "render no dialogue".
    nlohmann::json Fetch(RE::FormID senderFormID, std::string_view senderName, std::string_view playerName, int cap);

    // Drop entries older than the oldest memory in `memories`, so the
    // dialogue block and the memory block cover the same window. A line
    // from before the memory tail begins would reference events the
    // prompt has no memory context for, and reads as a past-life
    // insert.
    //
    // No-op when `memories` is empty, when the oldest age resolves to 0
    // (unknown), or when `nowGameSeconds` is 0 (no clock) — better to
    // show the full window than to blank it on a missing field.
    void FilterByMemoryAge(nlohmann::json& dialogue, const nlohmann::json& memories, double nowGameSeconds);

    // Replace each entry's `gameTime` with a rendered `age_str`. Called
    // after the age filter so the formatting cost is only paid on
    // entries the prompt renders.
    void AnnotateAges(nlohmann::json& dialogue, double nowGameSeconds);
} // namespace NarrativeEngine::SenderDialogue
