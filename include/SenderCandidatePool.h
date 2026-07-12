#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <RE/Skyrim.h>

// SenderCandidatePool — shared helper for building a bounded, viability-
// filtered pool of "recently engaged with the player" NPCs, each with a
// short player-involving memory tail attached. Used by both
// LetterComposer and VisitComposer — the two composers have different
// viability rules but share the underlying SkyrimNet fetch scaffolding
// and memory-shape.
//
// The helper handles:
//   - Engagement fetch via SkyrimNetAPI::GetActorEngagement.
//   - The universal viability walk (form resolves / actor exists / not
//     dead / not disabled / has a name).
//   - A caller-supplied extra viability filter for stricter gates.
//   - Per-candidate memory fetch via SkyrimNetAPI::GetMemoriesForActor
//     with importance-threshold and diary-entry filtering, then a
//     chronological sort so the LLM sees a running narrative.
//
// Thread rules: Build() and CountViable() are main-thread-only — they
// touch the RE:: side (form lookup, dead/disabled state) and SkyrimNet
// (which is best invoked from the main thread anyway).
namespace NarrativeEngine::SenderCandidatePool
{
    // A single viable sender candidate ready to hand to a compose prompt.
    // Shape matches what LetterComposer previously emitted — visits use
    // it verbatim, letters wrap it in their own SenderCandidate for
    // backwards compatibility.
    struct Candidate
    {
        RE::FormID     formId           = 0;
        std::string    name;
        double         engagementScore  = 0.0;
        double         lastInteractedAt = 0.0;
        // Per-candidate memory tail; already importance/diary-filtered
        // and sorted oldest-to-newest per the BuildOptions.
        nlohmann::json memories         = nlohmann::json::array();
    };

    // Callback returning true if the actor is a viable sender per the
    // caller's rules. Called AFTER the universal walk (missing / dead /
    // disabled / no-name), so callback body only needs to check caller-
    // specific gates (e.g. letter's "currently-loaded" rejection or
    // visit's "unique NPC" requirement).
    //
    // The optional `skipReasonOut` lets the callback provide a short
    // reason string that gets rolled into Build's summary log line.
    using ViabilityFilter = std::function<bool(RE::Actor* actor, std::string* skipReasonOut)>;

    struct BuildOptions
    {
        // Cap on the returned pool. Post-filter; the internal fetch pulls
        // a multiple of this to give client-side filtering headroom.
        int  maxCandidates             = 12;

        // Per-candidate memory tail cap. Applied after
        // importance/diary/sort filtering.
        int  maxMemoriesPerCandidate   = 6;

        // Extra viability filter — see the alias above.
        ViabilityFilter extraViabilityFilter;

        // Client-side importance-threshold filter on memories. Any memory
        // whose `importance_score` is below this is dropped. 0 disables
        // (keeps all). Set from `Settings::Get().letterMemoryImportanceThreshold`
        // (or an analogous setting) at the call site.
        double memoryImportanceThreshold = 0.0;

        // If true, memories whose content begins with "Diary Entry:" are
        // dropped. Letters exclude diaries at action-select time (they
        // just clutter the "should I write?" pick) but include them at
        // compose time (they're first-person voice the LLM can imitate).
        bool   excludeDiaryEntries     = false;

        // Multiplier applied to `maxMemoriesPerCandidate` when calling
        // SkyrimNetAPI::GetMemoriesForActor — over-fetch so importance
        // filtering still leaves us with roughly `maxMemoriesPerCandidate`
        // survivors even when the semantic-search tail has some
        // low-importance entries.
        int    memoryFetchMultiplier   = 4;

        // Whether to shuffle the returned candidate list. Letters do
        // (breaks the engagement-order LLM anchoring); visits do too.
        bool   shuffleResult           = true;

        // If true, memories with no non-diary content are counted as
        // "kept none" and the candidate is dropped as
        // "no-significant-memories". False accepts candidates with empty
        // memory tails (VisitComposer wants to allow this since visits
        // work from live actor context rather than a memory-driven brief).
        bool   requireMemories         = true;
    };

    // Main-thread. Full pool build: engagement fetch + universal viability
    // walk + extra filter + memory fetch + shuffle. Empty when SkyrimNet
    // is unavailable or no viable candidates survive.
    std::vector<Candidate> Build(const BuildOptions& opts);

    // Main-thread. Cheap IsAvailable-time count — same viability walk as
    // Build (universal + extra filter), no memory fetch. Stops walking
    // once `min` viable candidates have been counted. Returns the
    // count seen, capped at `min` (may exceed `min` if the sample walked
    // beyond it before the early-exit tick fired).
    std::size_t CountViable(const ViabilityFilter& extraFilter,
                             std::size_t            min);

    // Player display name — cached lookup, used as the semantic-search
    // bias when pulling per-candidate memories. Empty when the player
    // hasn't loaded yet.
    std::string GetPlayerDisplayName();
}
