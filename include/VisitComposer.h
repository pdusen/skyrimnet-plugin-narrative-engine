#pragma once

#include <IAction.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <RE/Skyrim.h>

// VisitComposer — the LLM round-trip that (a) picks a sender NPC from a
// candidate pool and (b) generates a scene brief for an in-person visit
// to the player.
//
// Runs on top of SenderCandidatePool (candidate build) and
// SkyrimNetAPI::SendCustomPromptToLLM (the prompt).
//
// The composer differs from LetterComposer in shape: letters pre-choose
// the sender at action-select time and only ask the LLM to write the
// content; visits let the LLM choose the sender AND author the brief in
// one round-trip. The sender is validated against the candidate pool
// after the response arrives.
namespace NarrativeEngine::VisitComposer
{
    struct VisitBriefing
    {
        RE::FormID               senderNpcFormID = 0;
        // First-person, from the sender's frame — the thought they're
        // carrying as they walk up to the player. Used as the
        // natural-conclusion poll's sender_goal input.
        std::string              briefing;
        // Third-person scene narration describing the sender's arrival
        // and their motivation for the visit. Passed straight to
        // SkyrimNet's DirectNarration as scene context; the downstream
        // dialogue LLM uses it to have the sender say something in
        // character about the topic.
        std::string              narration;
        std::string              topicTag;
        std::string              mood;
        std::vector<std::string> tags;
    };

    // Mirrors LetterComposer's shape so the two composers feel parallel.
    enum class UrgencyHint : std::uint8_t
    {
        Low    = 0,
        Medium = 1,
        High   = 2,
    };

    // Async. Main-thread entry; the callback fires on a SkyrimNet worker
    // thread — marshal back to main before touching anything engine-side.
    //
    // The callback receives nullopt on any failure path (SkyrimNet
    // unavailable, no viable candidates, LLM error, parse failure,
    // validation failure, sender mismatch). Reasons are logged.
    void Compose(
        const ActionContext& ctx,
        UrgencyHint          urgencyHint,
        std::function<void(std::optional<VisitBriefing>)> callback);

    // The set of moods the composer will accept. Exposed for testing.
    // Visits add `contrite` on top of the letter's mood set — an
    // apology beat that fits Falling Action / Resolution.
    bool IsValidMood(const std::string& mood);
}
