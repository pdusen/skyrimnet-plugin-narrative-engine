#pragma once

#include <VisitState.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// VisitConclusionPoll — cheap-signal-gated natural-conclusion LLM
// poll for the Discuss phase of an NPC visit.
//
// The poll is expensive; running it on a fixed real-time interval
// wastes LLM calls (too short) or misses the moment the beat has
// landed (too long). Instead a fast 1s tick checks three cheap
// thresholds — turn count since last poll, in-game time since last
// observed speech turn, and in-game time since last poll — and only
// when one trips does the LLM call fire.
//
// Threading:
//   * Arm / Disarm / GateTick / RegisterSpeechTurn: main thread.
//   * FirePoll's callback fires on a SkyrimNet worker thread — the
//     caller marshals to main before touching engine state.
//
// State ownership: file-scoped in the .cpp; the poll is single-
// visit-in-flight anyway, so a single slot suffices.
namespace NarrativeEngine::VisitConclusionPoll
{
    struct PollVerdict
    {
        bool        shouldConclude = false;
        std::string rationale;
    };

    // Reset internal counters / timers to a fresh Discuss entry.
    // Snapshot supplies the sender goal / topic / mood / briefing
    // the poll prompt needs.
    void Arm(const VisitState::Snapshot& snapshot);

    // Clear internal state — called on any exit from Discuss
    // (Valediction, ReturnHome via rollback, hard-abort).
    // Idempotent.
    void Disarm();

    // True iff the poll is currently armed (in the Discuss window).
    bool IsArmed();

    // Cheap tick — evaluate the three thresholds. Returns true if
    // any tripped this tick. Reads RE::Calendar for game time.
    bool GateTick();

    // Fire the natural-conclusion LLM poll. Async — callback fires
    // on a SkyrimNet worker thread. Passes nullopt on any failure
    // path (SkyrimNet unavailable, LLM error, parse failure).
    // Increments `consecutivePollFailures` on failure, resets to 0
    // on success. Resets the turn-count / last-poll clocks
    // regardless of verdict.
    void FirePoll(std::function<void(std::optional<PollVerdict>)> callback);

    // Called by the internal speech-turn watcher when a new
    // dialogue turn is observed. Bumps the turn-count threshold
    // and stamps the last-turn game-time. Also usable from
    // outside for tests / debug UI.
    void RegisterSpeechTurn();

    // Reads the current failure counter (for the hard-abort logic
    // in Step 15).
    std::uint32_t ConsecutivePollFailures();

    // Game-time seconds since the last observed speech turn from
    // sender/player. Returns 0 if the poll is disarmed or no
    // speech has ever been observed. Used by Step 11's verdict
    // handler to decide whether to fire a ContinueConversation
    // nudge on a `should_conclude=false` verdict.
    double SilenceGameSeconds();

    // Per-process ring of the last N poll verdicts. Consumed by
    // the dashboard's Visit tab. Wall-clock timestamp is
    // steady_clock seconds (same time base the dashboard uses).
    struct HistoryEntry
    {
        double      firedAtRealSeconds = 0.0;
        bool        shouldConclude     = false;
        std::string rationale;
    };
    inline constexpr std::size_t kVerdictRingSize = 5;
    std::vector<HistoryEntry> GetRecentVerdicts();
}
