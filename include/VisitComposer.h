#pragma once

#include <IAction.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <RE/Skyrim.h>

// VisitComposer — the LLM round-trip that authors the narrative
// content for an in-person NPC visit to the player.
//
// Runs on top of SkyrimNetAPI::SendCustomPromptToLLM. The sender is
// **pre-chosen** by the action-select LLM from the candidate list
// exposed on the npc_visit entry — the compose call embodies them,
// mirroring how LetterComposer takes a pre-picked sender from
// action-select and only writes the content.
//
// Callers (NPCVisitAction and ActionDispatcher) drive the two-stage
// flow:
//   1. ActionDispatcher builds `visit_sender_candidates` via
//      `CollectSenderCandidates()` when npc_visit is on the shortlist,
//      attaches them to the action-select prompt.
//   2. Action-select LLM returns `sender_npc_form_id` in the
//      npc_visit parameters block.
//   3. NPCVisitAction::Start reads that FormID and passes it to
//      `Compose()`, which produces the briefing / narration / mood /
//      topic / tags for that sender.
namespace NarrativeEngine::VisitComposer
{
    struct VisitBriefing
    {
        // First-person, from the sender's frame — the thought they're
        // carrying as they walk up to the player. Used as the
        // natural-conclusion poll's sender_goal input.
        std::string briefing;
        // Third-person scene narration describing the sender's arrival
        // and their motivation for the visit. Passed straight to
        // SkyrimNet's DirectNarration as scene context; the downstream
        // dialogue LLM uses it to have the sender say something in
        // character about the topic.
        std::string narration;
        std::string topicTag;
        std::string mood;
    };

    // Mirrors LetterComposer's shape so the two composers feel parallel.
    enum class UrgencyHint : std::uint8_t
    {
        Low    = 0,
        Medium = 1,
        High   = 2,
    };

    // A single viable visit sender, resolved on the main thread by
    // CollectSenderCandidates. Passed to the action-select stage so
    // the LLM sees who's available and can pick one; the picked
    // FormID then flows into Compose (which fresh-fetches the
    // sender's memories on the main thread before composing).
    struct SenderCandidate
    {
        RE::FormID     formId           = 0;
        std::string    name;
        double         engagementScore  = 0.0;
        double         lastInteractedAt = 0.0;
        nlohmann::json memories         = nlohmann::json::array();
    };

    // Main-thread only. Ranks recent engagement through
    // SenderCandidatePool with visit-specific viability rules
    // (unique / not in combat / not follower / has resolvable
    // location) and returns the surviving candidates with their
    // memory tails attached. Bounded output size (12 by default).
    // Empty when SkyrimNet is unavailable or no viable candidates
    // exist.
    //
    // Called by ActionDispatcher when npc_visit is among the
    // action-select candidates.
    std::vector<SenderCandidate> CollectSenderCandidates();

    // Serialize a candidate list into the JSON shape the action-select
    // prompt consumes: [{form_id (hex str), name, engagement_score,
    // last_interacted_at, memories}]. Mirrors
    // LetterComposer::SerializeSenderCandidates.
    nlohmann::json SerializeSenderCandidates(
        const std::vector<SenderCandidate>& candidates);

    // Async. Composes a visit briefing FROM a pre-chosen sender —
    // the action-select LLM picks the sender, this call embodies
    // them. Fresh-fetches the sender's current memories on the
    // main thread (so events between action-select and compose
    // surface here), then fires the compose prompt.
    //
    // The callback fires on a SkyrimNet worker thread — marshal
    // back to main before touching engine state.
    //
    // The callback receives nullopt on any failure path (SkyrimNet
    // unavailable, sender no longer viable, LLM error, parse
    // failure, validation failure). Failure reasons are logged.
    void Compose(
        const ActionContext& ctx,
        UrgencyHint          urgencyHint,
        RE::FormID           senderNpcFormID,
        std::function<void(std::optional<VisitBriefing>)> callback);

    // The set of moods the composer will accept. Exposed for testing.
    // Visits add `contrite` on top of the letter's mood set — an
    // apology beat that fits Falling Action / Resolution.
    bool IsValidMood(const std::string& mood);
}
