#pragma once

#include <PhaseTracker.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace SKSE
{
    class SerializationInterface;
}

// Persistent, bounded ring buffer of every Director evaluation (including
// no-beat evaluations). Doubles as the Director's record of its own past
// dispatches — `beatSelected` + `narrativeNote` + `advancedToPhase` capture
// everything Beta-canon needs.
//
// Threading: appends and structural mutations are exclusive; snapshot reads
// (Tail, LatestTensionScore) take a shared lock so the SkyrimNet decorators
// from Step 12 can read concurrently from the prompt-rendering thread.
namespace NarrativeEngine::DecisionLog
{
    struct DecisionRecord
    {
        double                              realTimeSec        = 0.0;
        float                               gameDaysPassed     = 0.0f;
        std::uint32_t                       tensionScore       = 0;  // 0..100
        PhaseTracker::Phase                 currentPhase       = PhaseTracker::Phase::Exposition;
        std::optional<PhaseTracker::Phase>  advancedToPhase;          // nullopt = no advancement this tick
        std::string                         beatSelected;             // empty = no beat selected
        std::string                         beatParametersJSON;
        std::string                         narrativeNote;            // LLM-supplied rationale
        std::uint32_t                       alphaCanonActiveSignals = 0;  // bitmask snapshot at evaluation time
    };

    // SKSE co-save record type ID for the decision log. Frozen.
    inline constexpr std::uint32_t kRecordTypeId = 'DCLG';

    // Push a record onto the back. Trims the front if over the configured
    // max-entries cap.
    void Append(DecisionRecord record);

    // Copy of the last `n` records, oldest first. If `n` exceeds the current
    // size, returns everything available. Safe to call from any thread.
    std::vector<DecisionRecord> Tail(std::size_t n);

    // Drop every record.
    void Clear();

    // Set the ring-buffer cap and trim immediately if exceeded. Wired at
    // kDataLoaded from Settings::Get().decisionLogMaxEntries.
    void SetMaxEntries(std::size_t n);

    // Most-recent decision's tension score, or nullopt if the log is empty.
    // Backs the Step 12 `ne_narrative_tension` decorator (which maps nullopt
    // to "0" so the prompt template always has a numeric value to interpret).
    std::optional<std::uint32_t> LatestTensionScore();

    // SKSE serialization callbacks. OnLoad receives the per-record version
    // and length advanced past the header by Plugin.cpp's dispatcher.
    void OnSave(SKSE::SerializationInterface* intfc);
    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
    void OnRevert();
}
