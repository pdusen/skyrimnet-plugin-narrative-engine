#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <PhaseTracker.h>

#include <nlohmann/json_fwd.hpp>

namespace RE
{
    class Actor;
}

// IBeat — the interface every Narrative Beat implements.
//
// The Narrative Beat System's master poll drives every registered beat
// through a single Tick(mode, state) entry point; the beat's own per-
// beat state machine (NOT_RUNNING / COMPOSE / RUNNING / CLEANUP) decides
// what happens on each cycle. Pre-quest work lives in Tick's COMPOSE
// arm; failure detection and completion detection are counter-driven
// transitions inside Tick.
namespace NarrativeEngine
{
    // Which way this beat pushes narrative tension.
    enum class BeatPolarity : std::uint8_t
    {
        Raise,
        Lower,
        Either,
    };

    // Per-beat lifecycle state. Persisted to the beat's own cosave record
    // between Tick invocations; the master poll dispatches to the beat's
    // Tick only when state != NOT_RUNNING.
    enum class BeatState : std::uint8_t
    {
        NOT_RUNNING, // baseline — beat is not in flight
        COMPOSE,     // pre-quest work (LLM compose, alias promote, etc.)
        RUNNING,     // beat's quest is live; stage advances drive Tick
        CLEANUP,     // post-quest teardown before returning to NOT_RUNNING
    };

    // World-state mode passed to Tick each cycle. Only one applies per
    // tick; the master poll uses the precedence
    // Paused > Combat > Dialogue > Normal so a paused game is never also
    // reported as Combat or Dialogue. See PHASE_06 for the design
    // rationale (Tick-modes-and-per-beat-behavior-under-each).
    enum class TickMode : std::uint8_t
    {
        Normal,
        Paused,
        Combat,
        Dialogue,
    };

    // Read-only snapshot of world state for the beat-selection pathway.
    // Built by ConsiderBeat immediately before calling IsAvailable, and
    // by StartBeat immediately before calling OnStart. Beats may not
    // extend the struct.
    //
    // The two call sites run on different threads today: ConsiderBeat
    // + IsAvailable run on the plugin thread (see IsAvailable's doc
    // below); StartBeat + OnStart run on the main thread. The struct
    // itself is thread-agnostic — a plain snapshot of world state
    // captured by the caller before entry.
    //
    // NOT passed to Tick — Tick runs on the BeatSystem's worker thread,
    // where most engine reads still require the token-based main-hop
    // pattern (see docs/THREADING_MODEL.md); beats that need engine
    // state during Tick must marshal to the main thread via
    // MainThread::Run or MainThread::FireAndForget.
    struct BeatContext
    {
        RE::Actor* player = nullptr;
        bool playerInCombat = false;
        bool playerInDialogue = false;
        bool playerInInterior = false;
        std::string locationName; // current Location's display name, may be empty
        std::string cellName;     // current Cell's display name, may be empty

        // Which way the Director wants tension to move on this tick. Beats
        // whose Polarity is Either consume this to shape their behavior;
        // beats with a fixed polarity ignore it. The dispatcher populates
        // both fields from the same values it already computed for the
        // beat-select prompt.
        PhaseTracker::Direction desiredDirection = PhaseTracker::Direction::Raise;
        int tensionDelta = 0;
    };

    // Result of a single Tick call. `transitionTo`, when populated,
    // instructs the master poll to advance the beat's cosave-recorded
    // BeatState. Landing on NOT_RUNNING clears the top-level "running
    // beat" slot and returns the system to NO_BEAT_RUNNING.
    struct TickResult
    {
        std::optional<BeatState> transitionTo;
    };

    class IBeat
    {
    public:
        virtual ~IBeat() = default;

        // Stable snake_case identifier. Used as the value the Director's
        // beat-select LLM returns to choose this beat, as the key that
        // identifies which cosave record belongs to this beat, and as
        // the log tag surfaced in dashboard and log lines. Never empty;
        // never changes for a given beat class.
        virtual std::string Name() const = 0;

        // One-paragraph description read by the beat-select prompt so
        // the LLM understands what each candidate does and when it's
        // appropriate.
        virtual std::string Description() const = 0;

        virtual BeatPolarity Polarity() const = 0;

        // Cheap synchronous check: does current world state permit this
        // beat to fire right now? Called once per beat per Director
        // tick to build the candidate manifest, from the plugin
        // thread (BeatSystem::ConsiderBeat's BuildBeatSelectPrep).
        // Must be side-effect free and safe to call off-main under
        // the codebase's documented "stable engine singleton pointer
        // + plain bool/pointer load" precedent (see
        // docs/MAIN_THREAD_STUTTER_AUDIT.md). SkyrimNet DLL calls and
        // engine reads that carry their own read lock (e.g.
        // ExtraAliasInstanceArray behind BSReadLockGuard) are the
        // canonical off-main-safe shapes here; anything that mutates
        // engine state does not belong in IsAvailable.
        virtual bool IsAvailable(const BeatContext& ctx) const = 0;

        // Called by BeatSystem::StartBeat exactly once, immediately after
        // the top-level state transitions to BEAT_RUNNING and before the
        // first Tick lands. The beat stores whatever LLM-supplied
        // parameters it needs on its own cosave record; typical beats
        // do minimal work here (validate + clamp params, seed the
        // per-beat state to COMPOSE).
        //
        // Plugin thread. This is param-parse + session-state-reset
        // territory — mutex- or atomic-guarded internal state is
        // fine, but do NOT read or mutate engine state here. Beats
        // that need engine state during Tick already marshal
        // explicitly per Tick's doc below; the same rule applies to
        // any first-tick setup that would want engine access — defer
        // it to the first Tick's COMPOSE arm rather than doing it in
        // OnStart.
        virtual void OnStart(const BeatContext& ctx, const nlohmann::json& parameters) = 0;

        // The beat's whole per-poll lifecycle. Called by BeatSystem's
        // master poll every N ms (default 250) while this beat is the
        // running beat. Runs on the master poll's worker thread —
        // engine reads that aren't safe off-thread must be marshaled
        // to the main thread via AsyncDispatch::MarshalToMainThread.
        // `state` is the beat's current BeatState as read from cosave;
        // `mode` is the master poll's gate reading for this tick.
        // Returns a TickResult that may request a state transition;
        // the master poll applies it.
        //
        // Implementations should exit early under Paused / Dialogue
        // (and typically Combat) unless the beat specifically wants to
        // do something under that mode.
        virtual TickResult Tick(TickMode mode, BeatState state) = 0;

        // In-game hours remaining before this beat's own per-beat
        // cooldown expires. Zero means "no cooldown active" / "can fire
        // now". Beats with no per-beat cooldown inherit the default
        // zero. Read-only, main thread, side-effect free — the
        // dashboard queries this every state push, and the beat-select
        // pipeline never touches it (cooldowns are already enforced
        // inside IsAvailable).
        virtual double RemainingCooldownGameHours() const
        {
            return 0.0;
        }

        // Force this beat to terminal cleanup immediately, regardless
        // of sub-phase. Called on the main thread by
        // BeatSystem::AbortRunningBeat in response to the dashboard's
        // Abort button. Must synchronously unwind any world-side
        // effects the beat has installed (quest stages, alias fills,
        // faction promotions, spawned refs, event sinks) and reset
        // internal session state so a subsequent StartBeat can run
        // cleanly. Does NOT roll back memories already written to
        // SkyrimNet or narrations already spoken by NPCs — those are
        // considered "already happened."
        virtual void Abort() = 0;
    };
} // namespace NarrativeEngine
