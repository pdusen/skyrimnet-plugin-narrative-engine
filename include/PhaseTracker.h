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
        Count,  // sentinel for bounds-checking; not a real phase
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

    // Current phase. Thread-safe.
    Phase Get();

    // Accumulated unpaused real-time seconds spent in the current phase, as
    // of the moment of the call. Internally samples the steady clock and
    // brings the accumulator up to "now" before returning, so callers always
    // see a fresh value rather than the value as of the last Tick. Main
    // thread only (calls RE::UI::GameIsPaused()).
    float TimeInPhaseSeconds();

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
}
