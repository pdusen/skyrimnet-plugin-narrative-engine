#include <PlotState.h>

#include <logger.h>
#include <Settings.h>

#include <algorithm>
#include <atomic>
#include <chrono>

namespace NarrativeEngine
{
    const PlotModel::Plot* PlotState::FindPlot(std::uint32_t id) const noexcept
    {
        const auto it = std::find_if(plots.begin(), plots.end(), [id](const PlotModel::Plot& p) { return p.id == id; });
        return it == plots.end() ? nullptr : &*it;
    }

    PlotModel::Plot* PlotState::FindPlot(std::uint32_t id) noexcept
    {
        return const_cast<PlotModel::Plot*>(static_cast<const PlotState*>(this)->FindPlot(id));
    }

    std::size_t PlotState::ActivePlotCount() const noexcept
    {
        return static_cast<std::size_t>(
            std::count_if(plots.begin(), plots.end(), [](const PlotModel::Plot& p) { return !p.IsTerminal(); }));
    }
} // namespace NarrativeEngine

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
    } // namespace

    void Initialize()
    {
        const auto& cfg = Settings::Get();

        g_live = PlotState{};

        // 0 means "nondeterministic": derive from the clock so two runs
        // differ. Any other value reproduces a run exactly, which is
        // what the offline harness and any bug report both want.
        if (cfg.plotRandomSeed != 0) {
            g_live.rngState = static_cast<std::uint64_t>(cfg.plotRandomSeed);
        } else {
            g_live.rngState = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        }

        g_published.store(std::make_shared<const PlotState>(g_live));

        logger::info("Plots: state initialised (enabled={}, seed={})", cfg.plotsEnabled, cfg.plotRandomSeed);
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
        plot.objectiveType = PlotModel::StepType::Acquire;
        plot.objectiveTarget = 0xDEAD00FF;
        plot.objectiveTargetName = "Amulet of Kings";
        plot.bornOnGameDay = gameDay;

        const auto addStep = [&plot](PlotModel::StepType type, RE::FormID target, const char* targetName) {
            PlotModel::Step step;
            step.type = type;
            step.target = target;
            step.targetName = targetName;
            plot.plan.push_back(std::move(step));
        };

        addStep(PlotModel::StepType::Locate, 0xDEAD0010, "the Broker");
        addStep(PlotModel::StepType::Surveil, 0xDEAD0011, "the Estate");
        addStep(plot.objectiveType, plot.objectiveTarget, plot.objectiveTargetName.c_str());

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
