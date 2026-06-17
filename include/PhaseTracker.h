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

    // Returns the immediate successor in Freytag order, or nullopt at
    // Resolution (Resolution is terminal — re-arcing is a later-phase concern).
    std::optional<Phase> NextPhase(Phase p);

    // Current phase. Thread-safe.
    Phase Get();

    // Accumulated unpaused real-time seconds spent in the current phase.
    // Thread-safe.
    float TimeInPhaseSeconds();

    // Switch phases. Resets time-in-phase to 0 and logs the transition.
    // Main thread only.
    void AdvanceTo(Phase newPhase);

    // Reset to a known state (defaults to Exposition, time 0). Main thread.
    void Reset(Phase initial = Phase::Exposition);

    // Increment accumulated time by dtSeconds, *only* when the game isn't
    // paused (RE::UI::GameIsPaused() — true during menus, console, dialogue).
    // Called from the tick driver (Step 8) on the main thread.
    void Tick(float dtSeconds);

    // SKSE serialization callbacks. OnLoad receives the per-record version
    // and length advanced past the header by the central OnLoad dispatcher
    // in Plugin.cpp.
    void OnSave(SKSE::SerializationInterface* intfc);
    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
    void OnRevert();
}
