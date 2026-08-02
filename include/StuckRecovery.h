#pragma once

#include <MainThread.h>

#include <RE/Skyrim.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// StuckRecovery — "this actor we spawned isn't going anywhere; put it
// somewhere it will."
//
// Beats that place actors in the world do not get to assume the
// placement worked. A point can pass every pre-spawn gate and still
// leave the actor unable to travel: the search validates a POSITION,
// while the engine moves a COLLISION CAPSULE along a route, and the two
// diverge wherever geometry the navmesh nominally covers can't actually
// be walked. Observed failures include an attacker embedded in a
// boulder with its head poking out, and a whole group beached on a
// shallow waterfall — neither of which the spawn search could have
// predicted, and neither of which ever reached the player.
//
// == Why this replaced a local-search approach ==
//
// The first version tried to reason its way out: detect off-navmesh or
// no-progress, then hunt for a nearby valid spot and nudge the actor
// onto it. It failed in practice for a reason worth recording. "Valid
// ground near an actor stuck in bad terrain" is overwhelmingly MORE BAD
// TERRAIN — the search kept finding technically-standable spots a body
// width away, inside the same feature, and the actor stayed stuck. No
// amount of widening the local search fixed that, because the premise
// was wrong: proximity to a trap is not a useful criterion.
//
// This version doesn't search. The caller already ran a real spawn
// search and validated more positions than it used, so recovery just
// works down that list. Those runner-up positions were vetted by the
// same gates as the winner and are far enough away to be genuinely
// different terrain, which is exactly the property local search
// couldn't provide.
//
// == The escalation ==
//
//   1. Every `checkIntervalSeconds`, compare the actor's position to
//      where it was at the last check. Moved at least
//      `movementThresholdUnits`? It is travelling; leave it alone.
//   2. Stalled? Warp it to the next unused runner-up position and start
//      the clock again.
//   3. Runner-ups exhausted? Warp it `closeInStepUnits` nearer the goal
//      along the line it would have walked, repeatedly, until it starts
//      moving on its own. These destinations are dropped in from a
//      little above the ground: they are picked off a bare line with
//      none of the vetting the runner-ups had, so landing on top of an
//      obstruction beats landing inside one.
//   4. Inside `minGoalDistanceUnits` and still stalled? Stranded.
//      Report it and stop — there is nowhere left to put it that isn't
//      on top of the player.
//
// Actors within `arrivedDistanceUnits` of the goal are never touched.
// They have got where they were going, and standing still at that range
// means fighting, not stuck.
//
// Neither is an actor IN COMBAT, at any range. This is the load-bearing
// exclusion. Combat AI does not walk in straight lines: it circles,
// holds position to shoot, backs off, and — when its target is too far
// away to perceive — searches, which looks exactly like being stuck. A
// warp on top of that destroys whatever the combat AI was doing and the
// actor searches again, so the next check warps it again. That loop was
// observed in the wild: three attackers warped four times in sixteen
// seconds, every one of them mid-fight, none of them stuck.
//
// The rule is therefore: this module supervises the WALK IN. Once an
// actor is fighting, it belongs to the combat AI, and an attacker that
// cannot reach the player from there is the caller's abandon timeout to
// deal with.
//
// == Threading ==
//
// Anything that touches an actor is main thread, and says so by taking a
// MainThread::Token. The one exception is Escort::DueForCheck, which is
// the check CLOCK: pure arithmetic over a double, called from the
// caller's tick on the plugin thread. Keeping it out of the main-thread
// hop is the whole reason it is a separate call — otherwise a beat
// ticking at 250 ms would marshal a task every tick just to find out
// that six seconds hadn't passed yet.
//
// Elapsed time is always passed in from the caller's tick accumulator;
// this module never reads a clock.
namespace NarrativeEngine::StuckRecovery
{
    // Nominal humanoid height, exported for callers sizing cover probes
    // against the same body the spawn search assumes.
    inline constexpr float kActorHeightUnits = 128.0f;

    // Lift applied to a grounded position so an actor placed there
    // starts just above the surface and settles down, rather than
    // starting embedded and being shoved out sideways.
    inline constexpr float kGroundClearanceUnits = 8.0f;

    struct Options
    {
        // Movement below this between two checks counts as stalled. Read
        // together with checkIntervalSeconds: the pair is really one
        // setting, "slower than this counts as stopped".
        //
        // Callers that expose tuning should override both from config —
        // the ambush reads iStuckRecoveryMovementThresholdUnits and
        // iStuckRecoveryCheckIntervalSeconds. These defaults exist so
        // the module is usable without any.
        float movementThresholdUnits = 100.0f;

        // Gap between position checks. Long enough that an actor which
        // was merely turning around, picking a path, or walking a
        // detour around something isn't judged prematurely.
        double checkIntervalSeconds = 4.0;

        // Once the runner-up list is exhausted, how much nearer the goal
        // each subsequent warp puts the actor.
        float closeInStepUnits = 600.0f;

        // Never warp an actor nearer the goal than this. Below it the
        // arrival stops reading as an approach.
        float minGoalDistanceUnits = 900.0f;

        // Actors already this close to the goal are left alone entirely
        // — they have arrived, and stillness at that range is combat
        // rather than a trap.
        float arrivedDistanceUnits = 1500.0f;
    };

    enum class Action : std::uint8_t
    {
        // Not time to check yet.
        None,
        // Arrived, or moving under its own power.
        Moving,
        WarpedToFallback,
        WarpedCloser,
        // Out of options: at the minimum goal distance and still stuck.
        Stranded,
    };

    const char* ActionName(Action action);

    struct Outcome
    {
        Action action = Action::None;
        RE::NiPoint3 movedTo{};

        bool Warped() const
        {
            return action == Action::WarpedToFallback || action == Action::WarpedCloser;
        }
    };

    // ---- Placement primitives ---------------------------------------
    //
    // Shared with the spawn searches: the same "can an actor stand here"
    // question is asked of spawn candidates and of recovery positions,
    // and one copy of the navmesh triangle walk is enough.

    // True when `pos` sits on navmesh — i.e. an actor placed there is on
    // ground the pathing system knows about. False when the position's
    // cell isn't loaded or carries no navmesh at all.
    //
    // Containment, not connectivity: this cannot say whether the mesh
    // under the point connects to the mesh under anything else. That
    // limitation is the reason this module exists.
    bool IsOnNavmesh(const RE::NiPoint3& pos);

    // True when `pos` sits under the local water surface. Takes the cell
    // so callers that already resolved one don't pay for a second
    // lookup; pass nullptr to have it resolved here.
    bool IsUnderwater(RE::TESObjectCELL* cell, const RE::NiPoint3& pos, float groundZ);

    // Resolve `pos` onto the terrain, writing the grounded position
    // (with kGroundClearanceUnits applied) to `out`. False when no
    // ground resolves — void, or an unloaded cell.
    bool GroundPoint(const RE::NiPoint3& pos, RE::NiPoint3& out, float& outGroundZ);

    // Grounded, dry, and on navmesh.
    bool IsStandable(const RE::NiPoint3& pos, RE::NiPoint3& out);

    // Warp `actor` to `pos` and re-evaluate its package so it resumes
    // whatever it was doing. Both the char-controller flag and the warp
    // are required, or the physics body stays behind and the actor walks
    // back into the geometry it was just pulled out of.
    void WarpTo(const MainThread::Token&, RE::Actor* actor, const RE::NiPoint3& pos);

    // ---- The escort -------------------------------------------------

    // Watches a group of placed actors and moves the ones that aren't
    // travelling. One per encounter; beats typically hold a file-static
    // and Clear() it at teardown.
    //
    // Not thread-safe by itself — main-thread-only like the rest of the
    // module, driven from a single Run lambda.
    class Escort
    {
    public:
        explicit Escort(std::string label);

        // Begin a run with the caller's unused-but-validated positions,
        // best-first. They should be spread out: two runner-ups fifty
        // units apart are one option, not two. Replaces any previous
        // list and clears all tracking.
        void Begin(std::vector<RE::NiPoint3> fallbacks);

        // Register an actor at the position it was actually placed, so
        // the first check has a baseline that isn't a guess.
        void Track(RE::Actor* actor, const RE::NiPoint3& placedAt);

        // PLUGIN THREAD. Advance the check clock and report whether a
        // check is now due; callers hop to the main thread and call
        // Update per actor only when this returns true.
        //
        // Split out from Update precisely so the waiting is free. The
        // clock is one addition and one comparison against
        // checkIntervalSeconds — no engine access, nothing that needs
        // the main thread — and folding it into Update would have meant
        // marshalling a main-thread task every tick just to decide not
        // to do anything, which at a 250 ms tick is ~24 wasted hops per
        // check.
        //
        // One clock for the whole group rather than one per actor: a
        // warp only ever happens during a check, so an actor that was
        // just moved already gets a full interval before it is judged
        // again.
        bool DueForCheck(double elapsedSeconds, const Options& opts = {});

        // Drop one actor's tracking — call when it dies or is removed,
        // so a recycled FormID can't inherit a spent fallback cursor.
        void Forget(RE::FormID actorId);

        // Drop everything, including the fallback list.
        void Clear();

        // MAIN THREAD. Evaluate one actor and move it if it has
        // stalled. `goal` is where it is trying to get to. Call only
        // when DueForCheck has returned true — this does the work
        // unconditionally.
        Outcome Update(const MainThread::Token& token,
                       RE::Actor* actor,
                       const RE::NiPoint3& goal,
                       const Options& opts = {});

        std::size_t FallbackCount() const
        {
            return m_fallbacks.size();
        }

    private:
        struct Track_
        {
            RE::NiPoint3 lastPos{};
            int closeInSteps = 0;
            bool stranded = false;
        };

        std::string m_label;
        std::vector<RE::NiPoint3> m_fallbacks;
        // Shared across actors, not per-actor. Each fallback is handed
        // out once: they are distinct PLACES, and sending three actors
        // to the same one stacks them on a single point where they shove
        // each other and none of them can walk.
        std::size_t m_nextFallback = 0;
        std::unordered_map<RE::FormID, Track_> m_tracks;
        // Written by DueForCheck on the plugin thread; never read by
        // Update. The two touch disjoint state, and the caller
        // serializes them anyway by blocking on its main-thread hop.
        double m_sinceCheck = 0.0;
    };
} // namespace NarrativeEngine::StuckRecovery
