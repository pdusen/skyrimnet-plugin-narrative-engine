#pragma once

#include <IAction.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <RE/Skyrim.h>

// LetterComposer — the LLM round trip that produces letter content
// (sender, label, body, mood, topic) from the player's recent
// SkyrimNet engagement history. NPCLetterAction owns the action-level
// dispatch and quest plumbing; this module owns just the LLM call.
//
// The composer runs on top of:
//   - SkyrimNetAPI::GetActorEngagement       — pick top-N candidates
//   - SkyrimNetAPI::GetMemoriesForActor      — pull each candidate's
//                                              recent player-involving
//                                              memories
//   - SkyrimNetAPI::SendCustomPromptToLLM    — fire the prompt
//   - LLMTextSanitizer                       — scrub returned strings
//
// See PHASE_04_LETTER_POOL_AND_NPC_LETTER_ACTION.md, sections:
//   - Content-generation LLM call
//   - Sender-label fallback
namespace NarrativeEngine::LetterComposer
{
    struct LetterComposition
    {
        RE::FormID  senderNpcFormID = 0;
        std::string senderLabel;
        std::string body;
        std::string mood;
        std::string topicTag;
    };

    enum class UrgencyHint : std::uint8_t
    {
        Low    = 0,
        Medium = 1,
        High   = 2,
    };

    // Async. Builds the candidate context, fires the LLM prompt, parses
    // and validates the response. The callback fires on a SkyrimNet
    // worker thread — marshal back to the main thread before touching
    // anything engine-side.
    //
    // The callback receives nullopt on any failure path (SkyrimNet
    // unavailable, candidate list empty, LLM error, parse failure,
    // validation failure). Failure reasons are logged so the call site
    // doesn't need to forward error details.
    void Compose(
        const ActionContext& ctx,
        UrgencyHint          urgencyHint,
        std::function<void(std::optional<LetterComposition>)> callback);

    // -----------------------------------------------------------------
    // Sender-label fallback (exposed for testing)
    // -----------------------------------------------------------------
    //
    // Picked when the LLM returns a sender_label > 24 bytes. See the
    // "Sender-label fallback" section of the phase plan for the
    // algorithm. Returns a label ≤ 24 bytes, never empty.
    //
    // Takes a plain string so the callback (which runs on a SkyrimNet
    // worker thread) doesn't need to touch the engine. The sender's
    // full name is captured up-front when LetterComposer assembles its
    // candidate context on the main thread.
    std::string SynthesizeFallbackLabel(const std::string& senderFullName);
}
