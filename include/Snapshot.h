#pragma once

#include <DecisionLog.h>

#include <cstdint>
#include <string>
#include <vector>

// Value-only snapshot of game state captured on the main thread at the
// start of a Director evaluation (Phase A). Carrying value-only fields
// — no RE::* pointers — makes the snapshot safe to move to the worker
// thread for prompt context assembly (Phase B) and LLM dispatch (Phase C).
namespace NarrativeEngine
{
    struct PlayerContext
    {
        std::uint32_t formID          = 0;  // player.GetFormID()
        std::uint32_t locationFormID  = 0;  // 0 if wilderness / no current location
        std::string   locationName;
        std::uint32_t cellFormID      = 0;
        std::string   cellName;
        bool          cellIsInterior  = false;
        float         gameDaysPassed  = 0.0f;  // Calendar::GetDaysPassed()
        float         timeOfDayHours  = 0.0f;  // Calendar::GetHour() — [0,24)
    };

    struct Snapshot
    {
        // Wall-clock seconds since plugin load. Monotonic; useful for the
        // dashboard's "last evaluation" timestamp and for DecisionRecord.
        double realTimeSec = 0.0;

        // PhaseTracker state at snapshot time.
        std::string currentPhase;          // PhaseName(PhaseTracker::Get())
        float       timeInPhaseSeconds = 0.0f;

        // Raw JSON string from SkyrimNetAPI::GetRecentEvents(0, N, "").
        // Passed through to the prompt context verbatim — no client-side
        // parsing for MVP.
        std::string skyrimNetEventsJSON;

        // Newest-last copy of the last N DecisionLog entries.
        std::vector<DecisionLog::DecisionRecord> decisionLogTail;

        // Player + world context.
        PlayerContext player;

        // Alpha Canon: name strings of every set bit, plus the raw bitmask
        // (cast from AlphaCanon::Signal). Names are deduplicated +
        // declaration-order; the bitmask is what DecisionRecord stores.
        std::vector<std::string> alphaCanonSignals;
        std::uint32_t            alphaCanonSignalBitmask = 0;
    };
}
