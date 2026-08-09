#include <GossipTick.h>

#include <EventLogUtil.h>
#include <GossipDispatch.h>
#include <GossipGraph.h>
#include <GossipHarvest.h>
#include <GossipLog.h>
#include <GossipSim.h>
#include <logger.h>
#include <Settings.h>

#include <algorithm>
#include <format>
#include <mutex>
#include <vector>

namespace NarrativeEngine::GossipTick
{
    namespace
    {
        // Plugin-thread only. This is scheduler state, not world state —
        // it deliberately does NOT live in GossipState, because it
        // describes when work should run rather than what the world looks
        // like, and a load must not restore a stale schedule.
        std::mutex g_mutex;
        double g_secondsSinceCheck = 0.0;
        // The game day the next tick is due for. Negative until the first
        // check seeds it from the clock.
        double g_nextDueGameDay = -1.0;

        double NowGameDay()
        {
            return EventLogUtil::NowGameTimeSeconds() / 86400.0;
        }

        // The whole of gossip, for one beat of in-world time.
        void RunTick(const GossipThread::Token& gt, double asOf, const GossipDispatch::CancellationHandle& cancel)
        {
            // A load or revert stages state rather than writing live, so
            // adopt before anything reads it. A sweep against the outgoing
            // world would claim memories the incoming one has no rumors
            // for.
            GossipSim::AdoptPendingState();

            if (cancel && cancel->IsCancelled()) {
                return;
            }

            // 1-2. Harvest, evaluate, compose, seed. Returns false when
            // the graph or SkyrimNet's memory system is not ready yet, in
            // which case the boundary stays owed rather than being spent
            // on an attempt that did nothing.
            const bool swept = GossipHarvest::RunSweep(gt, asOf, cancel);

            if (cancel && cancel->IsCancelled()) {
                return;
            }

            // 3-4. Advance the world to the horizon and prune.
            GossipSim::Advance(gt, asOf, cancel);

            if (cancel && cancel->IsCancelled()) {
                return;
            }

            // 5. The trace, written from the same thread that generated
            // it, so its order is execution order.
            GossipLog::Flush();

            // 6. One publication point, at the end of the job and nowhere
            // else. A snapshot taken mid-drain would show a half-advanced
            // simulation — some carriers stepped to the new game day and
            // some not, transmission counts that do not match the carrier
            // set they came from. What a reader needs is a series of
            // consistent states, and a completed tick is exactly that.
            GossipSim::PublishSnapshot();

            if (!swept) {
                logger::debug("GossipTick: tick at day {:.3f} could not sweep; boundary stays owed", asOf);
            }
        }
    } // namespace

    void Initialize()
    {
        const auto& cfg = Settings::Get();
        logger::info("GossipTick: initialized (every {}h of game time, checked every {}s of unpaused play)",
                     cfg.gossipHarvestIntervalGameHours,
                     cfg.gossipTickIntervalSeconds);
    }

    void OnSessionStart()
    {
        std::scoped_lock lock(g_mutex);
        g_secondsSinceCheck = 0.0;
        // Re-based rather than preserved: a day-200 save loaded after a
        // day-3 one would otherwise owe 197 days of ticks.
        g_nextDueGameDay = -1.0;
    }

    void Poll(const PluginThread::Token&, double unpausedElapsedSeconds)
    {
        const auto& cfg = Settings::Get();
        if (!cfg.gossipEnabled || !GossipGraph::IsReady()) {
            return;
        }

        // Every scheduled time that has gone by and not yet been
        // enqueued. Collected under the lock, dispatched outside it.
        std::vector<double> due;
        {
            std::scoped_lock lock(g_mutex);

            g_secondsSinceCheck += unpausedElapsedSeconds;
            const double checkEvery = static_cast<double>(std::max(1, cfg.gossipTickIntervalSeconds));
            if (g_secondsSinceCheck < checkEvery) {
                return;
            }
            g_secondsSinceCheck -= checkEvery;

            const double now = NowGameDay();
            const double interval = std::max(0.1f, cfg.gossipHarvestIntervalGameHours) / 24.0;

            if (g_nextDueGameDay < 0.0) {
                // First check of the session. The first tick is due one
                // full interval from now — not immediately, which would
                // sweep a world the player has not touched yet.
                g_nextDueGameDay = now + interval;
                return;
            }
            if (now < g_nextDueGameDay - interval) {
                // Clock went backwards further than drift explains: a load
                // of an older save, or a console time change. Re-base.
                g_nextDueGameDay = now + interval;
                return;
            }

            while (g_nextDueGameDay <= now) {
                due.push_back(g_nextDueGameDay);
                g_nextDueGameDay += interval;
            }
        }

        // The backlog cap is read from GossipDispatch rather than kept
        // here. A second counter would have to be decremented from the
        // job's exit path, and a tick CANCELLED WHILE STILL QUEUED never
        // reaches its exit path — so the count would creep upward across
        // loads until the cap silenced gossip permanently. The dispatcher
        // already releases handles on every exit, cancelled ones
        // included, so there is exactly one place that knows the answer.
        std::size_t dropped = 0;
        for (const double asOf : due) {
            if (GossipDispatch::OutstandingCount() >= kMaxOutstandingTicks) {
                // The schedule has already advanced past these; dropping
                // them is deliberate. There is no value in harvesting the
                // same memory corpus six times in a row, and a console
                // time jump must not be able to queue a year of
                // simulation.
                ++dropped;
                continue;
            }
            GossipDispatch::EnqueueCancellableWork(
                [asOf](const GossipThread::Token& gt, const GossipDispatch::CancellationHandle& cancel) {
                    RunTick(gt, asOf, cancel);
                });
        }
        if (dropped > 0) {
            GossipLog::Note(std::format(
                "schedule: dropped {} tick(s) beyond the {}-tick backlog cap", dropped, kMaxOutstandingTicks));
        }
    }
} // namespace NarrativeEngine::GossipTick
