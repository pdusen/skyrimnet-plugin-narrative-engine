#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <PlotModel.h>
#include <PlotThread.h>

#include <RE/Skyrim.h>

// PlotState — every mutable field the plot simulation owns, in one
// copyable struct, plus the two accessors that reach it.
//
// The same argument GossipState makes, for the same three payoffs:
//
//   1. ONE OWNER. The plot worker holds the live instance and nothing
//      else touches it, which is what lets every plot module carry no
//      mutex.
//   2. ONE SNAPSHOT. A shared_ptr<const PlotState> published at the end
//      of a tick is a consistent image at one instant. A read taken
//      mid-tick would show some plots stepped and some not.
//   3. ONE SAVE FORMAT. The co-save is written from the snapshot rather
//      than from a parallel description that can drift away from it.
//
// Everything here must stay CHEAPLY COPYABLE: no std::function, no
// owning pointers, no self-referential handles. A tick copies this
// wholesale to publish.
//
// Note the RNG *is* here, unlike gossip's, and it is a plain 64-bit
// integer rather than a generator object for exactly that reason. Plot
// outcomes need to be reproducible from a seed — the offline harness and
// any bug report both want it — and a saved-and-restored stream position
// is what makes a reloaded save continue the same run rather than
// silently reroll it.
namespace NarrativeEngine::PlotModel
{
    enum class Role : std::uint8_t
    {
        Mastermind,
        Actor
    };

    // One NPC's engagement and cooldowns.
    //
    // At most ONE engagement: masterminding a plot and acting in another
    // are both occupancy, so "one plot at a time or one step at a time"
    // is a single uniqueness constraint rather than two. `plotId == 0`
    // means free.
    //
    // Two SEPARATE cooldown stamps, because the roles recur at
    // completely different rates — a plot ends every week or two, a step
    // every day or two — and one value covering both would either bench
    // masterminds far too long or fail to spread step work at all.
    //
    // Occupancy and cooldown share a row on purpose: casting asks one
    // question ("can this NPC take this role now?") and one lookup
    // cannot disagree with itself the way two structures can.
    struct Occupancy
    {
        std::uint32_t plotId = 0;
        Role role = Role::Mastermind;
        double mastermindAvailableAtGameDay = 0.0;
        double actorAvailableAtGameDay = 0.0;

        [[nodiscard]] bool IsEngaged() const noexcept
        {
            return plotId != 0;
        }
    };

    // Session-scoped tallies. Diagnostic, and the first thing the
    // dashboard's budget header reads.
    struct Counters
    {
        std::uint32_t plotsBorn = 0;
        std::uint32_t plotsSucceeded = 0;
        std::uint32_t plotsFailed = 0;
        std::uint32_t stepsDispatched = 0;
        std::uint32_t stepsSucceeded = 0;
        std::uint32_t stepsTimedOut = 0;
        std::uint32_t stepsCaught = 0;
        std::uint32_t adaptations = 0;
        std::uint32_t ticksRun = 0;
    };
} // namespace NarrativeEngine::PlotModel

namespace NarrativeEngine
{
    struct PlotState
    {
        // Active and not-yet-reaped plots alike. A terminal plot stays
        // here for fPlotTerminalRetentionDays so the dashboard can still
        // answer "why did that one fail".
        std::vector<PlotModel::Plot> plots;

        std::unordered_map<RE::FormID, PlotModel::Occupancy> occupancy;

        std::uint32_t nextPlotId = 1;

        PlotModel::Counters counters;

        // The game time of the last tick that ran, as a game-day value.
        // The scheduler's own last-fired stamp lives with the scheduler;
        // this is the simulation's clock and is what step deadlines are
        // measured against.
        double simGameDay = 0.0;

        // splitmix64 state. A plain integer rather than a std::mt19937
        // so that publishing a snapshot stays a cheap copy and the
        // stream position round-trips through the co-save as one field.
        //
        // ZERO MEANS UNSEEDED, and Plots::StageLoadedState reads it that
        // way: a state that arrives with 0 gets a fresh generator. That
        // is what a default-constructed PlotState is -- the one staged
        // when a co-save is discarded or a game is reverted -- and
        // installing it verbatim used to overwrite the seed Initialize
        // had drawn, so every session replayed one sequence.
        std::uint64_t rngState = 0;

        // --- Reserved for Phase D ------------------------------------
        //
        // Player delegation is out of scope for this phase, but its
        // fields are declared now so that adding it is not a co-save
        // migration. Nothing in phases A-C writes or reads them.
        //
        // An offer is a step the simulation would rather hand to the
        // player, waiting on the Director to fire the errand beat. The
        // beat side reads offers from the PUBLISHED SNAPSHOT and claims
        // one by enqueuing back onto the plot worker, so the pool needs
        // no lock of its own.
        struct PendingOffer
        {
            std::uint32_t plotId = 0;
            std::size_t stepIndex = 0;
            double expiresOnGameDay = 0.0;
        };

        std::vector<PendingOffer> offers;

        // --- Convenience ---------------------------------------------

        // Defined inline, and deliberately so: these are operations on
        // the struct's own data with no engine, no settings and no
        // logging behind them. Putting them in the .cpp would drag the
        // plugin's plumbing into every probe that wants to exercise the
        // co-save format, which is exactly the coupling this subsystem
        // is factored to avoid.
        [[nodiscard]] const PlotModel::Plot* FindPlot(std::uint32_t id) const noexcept
        {
            const auto it =
                std::find_if(plots.begin(), plots.end(), [id](const PlotModel::Plot& p) { return p.id == id; });
            return it == plots.end() ? nullptr : &*it;
        }

        [[nodiscard]] PlotModel::Plot* FindPlot(std::uint32_t id) noexcept
        {
            return const_cast<PlotModel::Plot*>(static_cast<const PlotState*>(this)->FindPlot(id));
        }

        // Plots holding a budget slot: active ones only. Terminal plots
        // awaiting reaping do not count against the budget, which is
        // what lets a finished plot free its slot immediately while
        // staying visible on the dashboard.
        [[nodiscard]] std::size_t ActivePlotCount() const noexcept
        {
            return static_cast<std::size_t>(
                std::count_if(plots.begin(), plots.end(), [](const PlotModel::Plot& p) { return !p.IsTerminal(); }));
        }
    };
} // namespace NarrativeEngine

// The live state, the published snapshot, and the publish.
//
// Named `Plots` rather than `PlotState` so the namespace and the struct
// do not collide; the gossip equivalents live on GossipSim for the same
// reason.
namespace NarrativeEngine::Plots
{
    // Seeds the RNG from iPlotRandomSeed and clears the published
    // snapshot to an empty state. Call at kDataLoaded.
    void Initialize();

    // The live state.
    //
    // Gated on PlotThread::Token, which is the whole of this
    // subsystem's safety argument: the plot worker is the only thread
    // that can produce one, so it is the only thread that can reach
    // this, so the simulation needs no mutex. AsyncDispatch, the beat
    // workers, the main thread and SKSE's serialisation thread have no
    // way to call it — a compile error, not a rule.
    //
    // Everything they need instead: Snapshot() to read, and enqueuing
    // work onto PlotDispatch to write.
    PlotState& MutableState(const PlotThread::Token&);

    // The last published image of the whole plot world.
    //
    // Immutable and shared: every outside reader — the dashboard, the
    // co-save, the errand beat's availability check — loads this pointer
    // and reads it with no lock and no wait. Never null; before the
    // first publish it is an empty state, which reads correctly as
    // "nothing has happened yet".
    std::shared_ptr<const PlotState> Snapshot();

    // Copy the live state into a new published snapshot. Called at the
    // END of a unit of plot work, never during one — a snapshot taken
    // mid-tick would show a half-advanced simulation.
    void PublishSnapshot(const PlotThread::Token&);

    // Install state read from a co-save.
    //
    // Called on SKSE's SERIALISATION thread, which holds no plot token
    // and must not touch live state — so this parks the state in a
    // mutex-guarded pending slot and republishes the snapshot
    // immediately, and the plot worker installs it at the top of its
    // next unit of work. Readers therefore see the loaded world at
    // once, and the live state changes owner only on the thread that
    // owns it. Gossip's PendingState does the same thing for the same
    // reason.
    void StageLoadedState(PlotState state);

    // Move any staged state into live. Called by the plot worker at the
    // start of a unit of work, before anything reads live state.
    // Returns true if a load was applied.
    bool TakePendingState(const PlotThread::Token&);

    // splitmix64. Deterministic given the stream position in
    // PlotState::rngState, which is what makes a seeded run repeatable
    // across sessions and through a save/load.
    std::uint64_t NextRandom(PlotState& state) noexcept;

    // Uniform in [0, 1).
    double NextUniform(PlotState& state) noexcept;

    // Put one hand-built plot into the live state, for the steps that
    // need something to act on before plot birth exists. Names are
    // literals rather than engine reads, so this works with no world
    // loaded. Returns the new plot's id, or 0 if the budget is full.
    //
    // Superseded by step 6's stub plan table and then by step 11's real
    // birth call; kept until then because steps 3 and 4 have to move a
    // plot through a save and a tick to verify anything.
    //
    // NOTE ON THE TRIGGER. Step 2 of the phase doc calls for a console
    // command here. There is no console-command REGISTRATION surface in
    // this codebase — ConsoleCommand only issues commands into the
    // engine — so the manual trigger is a dashboard bridge action
    // instead, which is how every other debug affordance in this plugin
    // works (see DashboardUIManager's ne_dispatchAction). It is wired up
    // in step 8 along with the tab; until then this is called from
    // probes, which is all steps 3-6 need.
    std::uint32_t SeedDebugPlot(const PlotThread::Token&, double gameDay);
} // namespace NarrativeEngine::Plots
