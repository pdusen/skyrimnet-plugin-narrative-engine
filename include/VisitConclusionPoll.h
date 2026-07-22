#pragma once

#include <IBeat.h>
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
        bool shouldConclude = false;
        std::string rationale;
        // True iff the LLM observed that the sender has ALREADY
        // said their closing/goodbye line as part of the natural
        // exchange. When true and `shouldConclude` is also true,
        // Valediction dispatches its closing narration via
        // SkyrimNet's RegisterPersistentEvent (silent scene beat)
        // instead of DirectNarration (which would prompt a second
        // spoken goodbye). Ignored when `shouldConclude` is false.
        bool closingAlreadySpoken = false;
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

    // Advance the silence accumulator baseline. Call at the top of
    // every NPCVisitBeat::Tick, BEFORE any mode filter — the wall-
    // clock baseline must stay fresh across Paused / Dialogue / Combat
    // ticks so a resuming Normal tick doesn't retroactively credit the
    // whole gap to silence. Delta is only added to
    // g_silenceRealSeconds when `mode == TickMode::Normal` and the
    // poll is armed; on any other mode the baseline still updates.
    // Cheap: one mutex acquire and a `std::chrono::steady_clock::now()`
    // per call.
    void TickAccumulator(TickMode mode);

    // Cheap tick — evaluate the three thresholds. Returns true if
    // any tripped this tick. Reads RE::Calendar for game time.
    // Assumes the silence accumulator has already been advanced this
    // tick via TickAccumulator.
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

    // Real (wall-clock) seconds since the last observed speech
    // turn from sender/player, counting ONLY intervals during which
    // the beat's Tick was called with `mode == TickMode::Normal` —
    // Paused / Dialogue / Combat ticks freeze the accumulator, so
    // menu time, dialogue-view time, and combat time all elide.
    // Advanced incrementally on each TickAccumulator call, so the
    // accumulation reflects "the player was actively in the game
    // world doing nothing".
    // Returns 0 if the poll is disarmed or no speech has ever been
    // observed. Used by Step 11's verdict handler to decide
    // whether to fire a ContinueConversation nudge on a
    // `should_conclude=false` verdict.
    //
    // Rationale: the previous game-time measurement made the
    // nudge/valediction cadence depend on the user's iTimescale
    // (many users run 4-6 rather than the vanilla 20), so on a
    // slower timescale the silence gate could trip while the
    // player was just reading the dialogue view for what was, in
    // real time, an ordinary reply beat. Real-time-with-pause-
    // subtracted matches how long a human conversation partner
    // would actually wait before assuming they were being ignored.
    double SilenceRealSeconds();

    // Game-time seconds at which Discuss was armed. Consumers use
    // this to filter out pre-Salutation dialogue from event
    // samplers — otherwise old lines from earlier player-NPC
    // interactions poison the poll's `recent_lines` and make the
    // LLM believe the visit is a continuation of an old
    // conversation. Returns 0 if the poll is disarmed.
    double DiscussStartedAtGameSeconds();

    // Per-process ring of the last N poll verdicts. Consumed by
    // the dashboard's Visit tab. Wall-clock timestamp is
    // steady_clock seconds (same time base the dashboard uses).
    struct HistoryEntry
    {
        double firedAtRealSeconds = 0.0;
        bool shouldConclude = false;
        std::string rationale;
    };
    inline constexpr std::size_t kVerdictRingSize = 5;
    std::vector<HistoryEntry> GetRecentVerdicts();
} // namespace NarrativeEngine::VisitConclusionPoll
