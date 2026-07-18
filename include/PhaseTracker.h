#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace SKSE
{
    class SerializationInterface;
}

// Tracks the Director's current Freytag-pyramid phase and the unpaused
// wall-clock time spent in it. Persists across save/load via the SKSE
// co-save serialization interface.
//
// Threading: writers (Tick / AdvanceTo / Reset / OnLoad) run on the main
// thread; readers (Get / TimeInPhaseSeconds) may be called from any thread,
// including SkyrimNet's prompt-rendering thread once the Step 12 decorators
// register. A small internal mutex makes that safe.
namespace NarrativeEngine::PhaseTracker
{
    enum class Phase : std::uint8_t
    {
        Exposition = 0,
        RisingAction,
        Climax,
        FallingAction,
        Resolution,
        Count, // sentinel for bounds-checking; not a real phase
    };

    // Which way tension needs to move to leave a given phase. Used by
    // BeatSystem::ConsiderBeat to filter the registry down to candidates
    // whose polarity matches the current phase's outgoing direction.
    //   Exposition / RisingAction / Resolution → Raise
    //   Climax / FallingAction                 → Lower
    enum class Direction : std::uint8_t
    {
        Raise,
        Lower
    };

    // SKSE co-save record type ID for the phase tracker. Frozen — changing it
    // would orphan every previously-saved PhaseTracker payload.
    inline constexpr std::uint32_t kRecordTypeId = 'PHTR';

    // Stable string form of each phase. Returns "Unknown" if `p` is out of
    // range. Never nullptr.
    const char* PhaseName(Phase p);

    // Inverse of PhaseName. Returns nullopt on unrecognized input.
    std::optional<Phase> PhaseFromName(std::string_view name);

    // Returns the immediate successor in Freytag order. The loop is
    // **cyclical**: NextPhase(Resolution) wraps to Exposition. There is no
    // terminal phase, so this is total — every Phase has a valid successor.
    Phase NextPhase(Phase p);

    // Given a tension score in [0..100], the current phase, and the current
    // dwell time in that phase (unpaused real-time seconds), decides whether
    // the Director should advance into NextPhase(current). Pure function —
    // reads only Settings::Get() thresholds and minPhaseDurationSeconds.
    // Returns the new phase when the tension threshold has been crossed AND
    // the dwell has met the minimum floor; nullopt to remain. Used by
    // EvaluationPipeline::ParseDecision so the LLM only has to produce a
    // tension score, not a stay-or-advance verdict.
    std::optional<Phase> EvaluateAdvance(Phase current, std::uint32_t tensionScore, float timeInPhaseSeconds);

    // Returns the tension-movement direction the cycle wants in order to
    // leave the given phase. Total — every Phase has a defined direction.
    // Pure, stateless; safe to call from any thread.
    Direction OutgoingDirection(Phase p);

    // Current phase. Thread-safe.
    Phase Get();

    // Accumulated unpaused real-time seconds spent in the current phase, as
    // of the moment of the call. Internally samples the steady clock and
    // brings the accumulator up to "now" before returning, so callers always
    // see a fresh value rather than the value as of the last Tick. Main
    // thread only (calls RE::UI::GameIsPaused()).
    float TimeInPhaseSeconds();

    // Unix-epoch real-wall-clock seconds at which the current phase was
    // entered. Used by the evaluation pipeline to filter the SkyrimNet
    // event tail to only events that occurred during the current phase —
    // events from before the last phase change have already been "consumed"
    // by whichever decision drove that change, and shouldn't justify another
    // round of advancement. Real time (not game time) because SkyrimNet's
    // per-event `gameTime` is time-of-day-in-seconds, which can't be
    // compared against a cumulative cutoff once a session crosses a day;
    // SkyrimNet's `localTime` field is monotonic Unix-epoch real-seconds,
    // which is what this returns. Thread-safe. Returns 0.0 if no
    // advance/reset has happened yet (filter then includes everything,
    // which is the safe default).
    double PhaseEnteredAtRealTime();

    // Switch phases. Resets time-in-phase to 0 and logs the transition.
    // Main thread only.
    void AdvanceTo(Phase newPhase);

    // Reset to a known state (defaults to Exposition, time 0). Main thread.
    void Reset(Phase initial = Phase::Exposition);

    // Sample the steady clock and roll the elapsed time since the last
    // sample into the accumulator — but only when the game is not paused
    // (RE::UI::GameIsPaused() — true during menus, console, dialogue).
    // Called periodically by the tick driver (Step 8) on the main thread.
    void Tick();

    // SKSE serialization callbacks. OnLoad receives the per-record version
    // and length advanced past the header by the central OnLoad dispatcher
    // in Plugin.cpp.
    void OnSave(SKSE::SerializationInterface* intfc);
    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
    void OnRevert();
} // namespace NarrativeEngine::PhaseTracker
