#pragma once

#include <BeatParamHelpers.h>
#include <IBeat.h>

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
// **pre-chosen** by the beat-select LLM from the candidate list
// exposed on the npc_visit entry — the compose call embodies them,
// mirroring how LetterComposer takes a pre-picked sender from
// beat-select and only writes the content.
//
// The two-stage flow driven by NPCVisitBeat and BeatSystem:
//   1. BeatSystem::ConsiderBeat builds `visit_sender_candidates` via
//      `CollectSenderCandidates()` when npc_visit is on the shortlist,
//      attaches them to the beat-select prompt.
//   2. Beat-select LLM returns `sender_npc_form_id` in the npc_visit
//      parameters block.
//   3. NPCVisitBeat::OnStart records the FormID; the beat's COMPOSE
//      arm passes it to `Compose()`, which produces the briefing /
//      narration / mood / topic for that sender.
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

    // Beat-select LLM's urgency signal. Aliased from the shared
    // BeatParamHelpers enum so all sender-picking beats speak one type.
    using UrgencyHint = BeatParamHelpers::UrgencyHint;

    // A single viable visit sender, resolved on the main thread by
    // CollectSenderCandidates. Passed to the beat-select stage so
    // the LLM sees who's available and can pick one; the picked
    // FormID then flows into Compose (which fresh-fetches the
    // sender's memories on the main thread before composing).
    struct SenderCandidate
    {
        RE::FormID formId = 0;
        std::string name;
        double engagementScore = 0.0;
        double lastInteractedAt = 0.0;
        nlohmann::json memories = nlohmann::json::array();
    };

    // Safe from the plugin thread (BeatSystem::ConsiderBeat's
    // BuildBeatSelectPrep is the caller). Same off-main-safety
    // story as LetterComposer::CollectSenderCandidates — every
    // underlying read is off-main-safe per the audit doc, and no
    // engine mutation happens here.
    //
    // Ranks recent engagement through SenderCandidatePool with
    // visit-specific viability rules (unique / not in combat / not
    // follower / has resolvable location) and returns the surviving
    // candidates with their memory tails attached. Bounded output
    // size (12 by default). Empty when SkyrimNet is unavailable or
    // no viable candidates exist.
    //
    // Called by BeatSystem::ConsiderBeat when npc_visit is among the
    // beat-select candidates.
    std::vector<SenderCandidate> CollectSenderCandidates();

    // Serialize a candidate list into the JSON shape the beat-select
    // prompt consumes: [{form_id (hex str), name, engagement_score,
    // last_interacted_at, memories}]. Mirrors
    // LetterComposer::SerializeSenderCandidates.
    nlohmann::json SerializeSenderCandidates(const std::vector<SenderCandidate>& candidates);

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
    // `parameterJustification` is the `parameter_justification`
    // string the beat-select LLM emitted alongside the sender pick
    // — the in-fiction, sender-frame explanation of why THIS sender
    // is coming (rooted in what they'd actually know). Rendered into
    // the compose prompt as the sender's motivation seed so the
    // compose LLM stays grounded in the director's actual choice
    // rather than inventing one from the sender's memory tail.
    //
    // Deliberately separate from beat-select's `narrative_note`
    // (which is director-frame commentary about world state the
    // sender may not know) — only the sender-frame justification
    // reaches compose. Empty string means beat-select didn't supply
    // one; the prompt handles the empty case by inventing motivation
    // from the sender's memory tail.
    void Compose(const BeatContext& ctx,
                 UrgencyHint urgencyHint,
                 RE::FormID senderNpcFormID,
                 std::string parameterJustification,
                 std::function<void(std::optional<VisitBriefing>)> callback);

    // The set of moods the composer will accept. Exposed for testing.
    // Visits add `contrite` on top of the letter's mood set — an
    // apology beat that fits Falling Action / Resolution.
    bool IsValidMood(const std::string& mood);
} // namespace NarrativeEngine::VisitComposer
