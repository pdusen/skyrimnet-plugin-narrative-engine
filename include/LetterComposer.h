#pragma once

#include <BeatParamHelpers.h>
#include <IBeat.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <RE/Skyrim.h>

// LetterComposer — the LLM round trip that produces letter content
// (sender, label, body, mood, topic) from the player's recent
// SkyrimNet engagement history. NPCLetterBeat owns the beat-level
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
        RE::FormID senderNpcFormID = 0;
        // Book display label — the letter's inventory-header name
        // (e.g. "Letter from Ysolda"). Sourced from the compose
        // LLM's `letter_label` field; the deterministic
        // SynthesizeFallbackLabel is used when the LLM's return
        // is missing, empty, or exceeds the 24-byte engine cap.
        std::string senderLabel;
        std::string body;
        std::string mood;
        std::string topicTag;
        std::vector<std::string> tags;
    };

    // Beat-select LLM's urgency signal. Aliased from the shared
    // BeatParamHelpers enum so all sender-picking beats speak one type.
    using UrgencyHint = BeatParamHelpers::UrgencyHint;

    // A single viable letter sender, resolved on the main thread by
    // CollectSenderCandidates. Passed to the beat-select stage so
    // the LLM sees who's available and can pick one; also passed
    // (indirectly, via form_id) into Compose so the letter-writing
    // prompt has the full context without re-fetching.
    struct SenderCandidate
    {
        RE::FormID formId = 0;
        std::string name;
        double engagementScore = 0.0;
        double lastInteractedAt = 0.0;
        nlohmann::json memories = nlohmann::json::array();
    };

    // Safe from the plugin thread (BeatSystem::ConsiderBeat's
    // BuildBeatSelectPrep is the caller). Every underlying read
    // (SenderCandidatePool → SkyrimNetAPI DLL calls + alias walk
    // under BSReadLockGuard) is off-main-safe per the audit doc;
    // no engine mutation happens here.
    //
    // Ranks recent engagement, filters out dead / disabled / cooldown
    // / missing candidates, and pulls each survivor's player-involving
    // memory tail from SkyrimNet. Bounded output size (kCandidateCap
    // internally). Empty when SkyrimNet is unavailable or no viable
    // candidates exist.
    //
    // Called by BeatSystem::ConsiderBeat when npc_letter is among the
    // beat-select candidates, so the LLM sees a live list at pick time.
    std::vector<SenderCandidate> CollectSenderCandidates();

    // Serialize a candidate list into the JSON shape the beat-select
    // and letter-compose prompts consume: [{form_id (hex str), name,
    // engagement_score, last_interacted_at, memories}].
    nlohmann::json SerializeSenderCandidates(const std::vector<SenderCandidate>& candidates);

    // Async. Composes a letter FROM a pre-chosen sender — the beat-
    // select LLM picks the sender, this call embodies them. Fresh-
    // fetches the sender's current memories on the main thread (so
    // any events between beat-select and compose surface here), then
    // fires the compose prompt.
    //
    // The callback fires on a SkyrimNet worker thread — marshal back to
    // the main thread before touching anything engine-side.
    //
    // The callback receives nullopt on any failure path (SkyrimNet
    // unavailable, sender no longer viable, LLM error, parse failure,
    // validation failure). Failure reasons are logged so the call site
    // doesn't need to forward error details.
    void Compose(const BeatContext& ctx,
                 UrgencyHint urgencyHint,
                 RE::FormID senderNpcFormID,
                 std::function<void(std::optional<LetterComposition>)> callback);

    // -----------------------------------------------------------------
    // Sender-label fallback (exposed for testing)
    // -----------------------------------------------------------------
    //
    // Picked when the LLM's `letter_label` is missing, empty, or
    // exceeds the 24-byte engine cap. See the
    // "Sender-label fallback" section of the phase plan for the
    // algorithm. Returns a label ≤ 24 bytes, never empty.
    //
    // Takes a plain string so the callback (which runs on a SkyrimNet
    // worker thread) doesn't need to touch the engine. The sender's
    // full name is captured up-front when LetterComposer assembles its
    // candidate context on the main thread.
    std::string SynthesizeFallbackLabel(const std::string& senderFullName);
} // namespace NarrativeEngine::LetterComposer
