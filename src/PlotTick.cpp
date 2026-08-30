#include <PlotTick.h>

#include <EngineUtils.h>
#include <logger.h>
#include <PlotDispatch.h>
#include <PlotSchedule.h>
#include <PlotState.h>
#include <Settings.h>

#include <algorithm>

namespace NarrativeEngine::PlotTick
{
    namespace
    {
        // Plugin-thread only. Poll() is called from PollOnPluginThread,
        // which is itself serialised through AsyncDispatch, so this
        // needs no synchronisation of its own.
        double g_lastFiredGameHours = 0.0;
        bool g_needsRebase = true;

        // Run one unit of plot work, as of `asOfGameHours`.
        //
        // Cancellation is checked at every operation boundary, not only
        // before the publish. A tick that keeps running past a load
        // writes memories into SkyrimNet's database — which lives
        // outside our co-save and is not rolled back by loading an
        // earlier game — and from step 15 will mutate inventories and
        // relationship ranks too. Discarding results at the end would
        // disown those writes without preventing them.
        void RunTick(const PlotThread::Token& pt, const PlotDispatch::CancellationHandle& cancel, double asOfGameHours)
        {
            if (cancel && cancel->IsCancelled()) {
                return;
            }

            // A load that landed while this job sat in the queue takes
            // effect here, before anything reads live state.
            Plots::TakePendingState(pt);

            if (cancel && cancel->IsCancelled()) {
                return;
            }

            PlotState& state = Plots::MutableState(pt);
            state.simGameDay = asOfGameHours / 24.0;
            ++state.counters.ticksRun;

            // Steps 5 and 6 fill in the body: birth against the budget,
            // step dispatch and casting, the progress race, adaptation,
            // and reaping. Until then a tick is its bookkeeping.

            if (cancel && cancel->IsCancelled()) {
                return;
            }

            if (Settings::Get().plotLogEnabled) {
                logger::debug("PlotTick: tick #{} as of game day {:.3f} ({} plot(s))",
                              state.counters.ticksRun,
                              state.simGameDay,
                              state.plots.size());
            }

            // Published at the END of the unit of work, never during one.
            // A snapshot taken mid-tick would show a half-advanced
            // simulation: some plots stepped to the new game day and
            // some not.
            Plots::PublishSnapshot(pt);
        }

        void Enqueue(double asOfGameHours)
        {
            PlotDispatch::EnqueueCancellableWork(
                [asOfGameHours](const PlotThread::Token& pt, const PlotDispatch::CancellationHandle& cancel) {
                    RunTick(pt, cancel, asOfGameHours);
                });
        }
    } // namespace

    void Initialize()
    {
        g_lastFiredGameHours = 0.0;
        g_needsRebase = true;
    }

    void OnSessionStart()
    {
        // Re-based on the next poll rather than here: this runs on the
        // main thread at kNewGame / kPostLoadGame, and the clock reading
        // that matters is the one the plugin thread sees when it next
        // looks. Marking it is enough, and it keeps the game-clock read
        // on one thread.
        g_needsRebase = true;
    }

    void Poll(const PluginThread::Token&)
    {
        const auto& cfg = Settings::Get();
        if (!cfg.plotsEnabled) {
            return;
        }

        const double nowGameHours = EngineUtils::GetCurrentGameHours();

        // Before the Calendar singleton exists the reading is 0.0, which
        // is not a real clock value — treating it as one would make the
        // first genuine reading look like an enormous backlog.
        if (nowGameHours <= 0.0) {
            return;
        }

        if (g_needsRebase) {
            g_lastFiredGameHours = nowGameHours;
            g_needsRebase = false;
            return;
        }

        const auto decision = PlotSchedule::Advance(g_lastFiredGameHours,
                                                    nowGameHours,
                                                    static_cast<double>(cfg.plotTickIntervalGameHours),
                                                    static_cast<std::size_t>(std::max(1, cfg.plotMaxOutstandingTicks)),
                                                    PlotDispatch::OutstandingCount());

        g_lastFiredGameHours = decision.newLastFiredGameHours;

        if (decision.rebased) {
            logger::info("PlotTick: game clock moved backwards; schedule re-based to {:.3f}h", nowGameHours);
            return;
        }
        if (decision.skipped > 0) {
            logger::warn("PlotTick: outstanding cap reached; advancing schedule past {} tick(s) without running them",
                         decision.skipped);
        }
        for (const double stamp : decision.stamps) {
            Enqueue(stamp);
        }
    }

    void ForceTicks(std::size_t count)
    {
        if (count == 0) {
            return;
        }
        const double nowGameHours = EngineUtils::GetCurrentGameHours();
        const double interval = std::max(0.001, static_cast<double>(Settings::Get().plotTickIntervalGameHours));

        // Stamped as if the schedule had produced them, so a forced tick
        // is indistinguishable from a scheduled one once it is running.
        // The schedule itself is advanced to match, or the next real
        // poll would re-run the same in-world time.
        for (std::size_t i = 1; i <= count; ++i) {
            Enqueue(nowGameHours + interval * static_cast<double>(i));
        }
        g_lastFiredGameHours = nowGameHours + interval * static_cast<double>(count);
        g_needsRebase = false;

        logger::info("PlotTick: forced {} tick(s)", count);
    }
} // namespace NarrativeEngine::PlotTick
