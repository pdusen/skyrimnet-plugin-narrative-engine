#include <PlotState.h>

#include <logger.h>
#include <Settings.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>

namespace NarrativeEngine::Plots
{
    namespace
    {
        // The live state. Reachable only through MutableState, which
        // demands a PlotThread::Token, so this needs no mutex.
        PlotState g_live;

        // The published snapshot. Written only by PublishSnapshot (plot
        // worker) and by Initialize; read by anyone. std::atomic on a
        // shared_ptr rather than a mutex so a reader never waits on the
        // worker and the worker never waits on a reader.
        std::atomic<std::shared_ptr<const PlotState>> g_published{std::make_shared<const PlotState>()};

        // Staged by a load, installed by the plot worker. The only
        // shared mutable thing in this subsystem, and it is guarded
        // because its two ends are genuinely different threads.
        std::mutex g_pendingMutex;
        std::optional<PlotState> g_pending;
    } // namespace

    namespace
    {
        // What a state with no history of its own starts from.
        //
        // iPlotRandomSeed = 0 means "nondeterministic": derive from the
        // clock so two runs differ. Any other value reproduces a run
        // exactly, which is what the offline harness and any bug report
        // both want.
        std::uint64_t FreshSeed()
        {
            const auto& cfg = Settings::Get();
            if (cfg.plotRandomSeed != 0) {
                return static_cast<std::uint64_t>(cfg.plotRandomSeed);
            }
            return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        }
    } // namespace

    void Initialize()
    {
        g_live = PlotState{};
        g_live.rngState = FreshSeed();
        g_published.store(std::make_shared<const PlotState>(g_live));

        logger::info("Plots: state initialised (enabled={}, seed={})",
                     Settings::Get().plotsEnabled,
                     Settings::Get().plotRandomSeed);
    }

    void StageLoadedState(PlotState state)
    {
        // A STATE WITH NO GENERATOR GETS A FRESH ONE.
        //
        // Initialize seeds the clock once at startup, and this then
        // threw that away on every single load: a discarded co-save and
        // a revert both stage a default-constructed PlotState, whose
        // rngState is 0, and TakePendingState installs it wholesale. So
        // every session began from the same generator and replayed the
        // same sequence -- across four runs the third plot was
        // masterminded by Korir in four of four, the first by Weylin in
        // three of four. The seed was never static by configuration; it
        // was being overwritten.
        //
        // A state RESTORED from a co-save carries a generator that has
        // already been drawn from, and keeps it: resuming a save should
        // continue its sequence rather than start a new one.
        if (state.rngState == 0) {
            state.rngState = FreshSeed();
        }
        {
            std::scoped_lock lock(g_pendingMutex);
            g_pending = state;
        }
        // Publish immediately. Between a load and the plot worker's next
        // unit of work there may be minutes of play, and during that
        // window the dashboard and the co-save must show the world the
        // player actually loaded rather than the one they left.
        g_published.store(std::make_shared<const PlotState>(std::move(state)));
    }

    bool TakePendingState(const PlotThread::Token&)
    {
        std::optional<PlotState> pending;
        {
            std::scoped_lock lock(g_pendingMutex);
            pending.swap(g_pending);
        }
        if (!pending.has_value()) {
            return false;
        }
        g_live = std::move(*pending);
        logger::info("Plots: installed loaded state ({} plot(s))", g_live.plots.size());
        return true;
    }

    PlotState& MutableState(const PlotThread::Token&)
    {
        return g_live;
    }

    std::shared_ptr<const PlotState> Snapshot()
    {
        return g_published.load();
    }

    void PublishSnapshot(const PlotThread::Token&)
    {
        g_published.store(std::make_shared<const PlotState>(g_live));
    }

    std::uint64_t NextRandom(PlotState& state) noexcept
    {
        // splitmix64. Chosen over std::mt19937 because the whole
        // generator has to fit in the snapshot and survive the co-save:
        // one 64-bit field does, a 2.5 KB Mersenne state copied on every
        // publish does not.
        std::uint64_t z = (state.rngState += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    std::uint32_t SeedDebugPlot(const PlotThread::Token& pt, double gameDay)
    {
        PlotState& state = MutableState(pt);
        if (state.ActivePlotCount() >= static_cast<std::size_t>(std::max(1, Settings::Get().plotMaxConcurrent))) {
            logger::warn("Plots::SeedDebugPlot: budget full; not seeding");
            return 0;
        }

        PlotModel::Plot plot;
        plot.id = state.nextPlotId++;
        // Fabricated identities. This function exists precisely so the
        // earlier steps can run with no world loaded, so it must not
        // reach for a TESForm to name anyone.
        plot.mastermind = 0xDEAD0001;
        plot.mastermindName = "Debug Mastermind";
        plot.ambition = "A hand-seeded plot, standing in until plot birth exists.";
        plot.scheme = "Get the Amulet of Kings out of the Broker's hands and into hers.";
        plot.bornOnGameDay = gameDay;

        const auto addStep = [&plot](PlotModel::StepType type, const char* description) {
            PlotModel::Step step;
            step.type = type;
            step.description = description;
            plot.plan.push_back(std::move(step));
        };

        addStep(PlotModel::StepType::Locate, "Find out which of the Broker's rooms the amulet is kept in.");
        addStep(PlotModel::StepType::Surveil, "Watch the estate until the household's evening routine is clear.");
        addStep(PlotModel::StepType::Acquire, "Take the amulet during the hour the study stands empty.");

        auto& row = state.occupancy[plot.mastermind];
        row.plotId = plot.id;
        row.role = PlotModel::Role::Mastermind;

        const std::uint32_t id = plot.id;
        state.plots.push_back(std::move(plot));
        ++state.counters.plotsBorn;

        logger::info("Plots::SeedDebugPlot: seeded plot {} on game day {:.2f}", id, gameDay);
        return id;
    }

    double NextUniform(PlotState& state) noexcept
    {
        // Top 53 bits, which is exactly the mantissa a double can hold.
        // Taking the low bits instead would waste the better-mixed half
        // of the word.
        return static_cast<double>(NextRandom(state) >> 11) * (1.0 / 9007199254740992.0);
    }
} // namespace NarrativeEngine::Plots
