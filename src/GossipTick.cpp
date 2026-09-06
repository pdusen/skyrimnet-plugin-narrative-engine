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
        // Plugin-thread only, and session-local — but DERIVED from world
        // state rather than re-based onto the game clock at load. What a
        // load must not restore is a schedule belonging to a different
        // world; what it must not discard is how long the incoming world
        // has gone without a tick. GossipState::simGameDay is persisted
        // and is exactly that record, so OnSessionStart reads it and the
        // schedule resumes from where the saved world left off.
        std::mutex g_mutex;
        double g_secondsSinceCheck = 0.0;
        // The game day the next tick is due for. Negative until the first
        // check seeds it from the session anchor below.
        double g_nextDueGameDay = -1.0;
        // The simulation clock of the world this session started with,
        // captured at OnSessionStart before any tick can move it off it.
        // Negative when that world has never run a tick.
        double g_sessionAnchorGameDay = -1.0;

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

            // 0. Stamp the clock BEFORE the harvest. A rumor seeded during
            // this tick dates itself from the simulation clock, so setting
            // the horizon afterwards would date every rumor one whole
            // interval in the past.
            GossipSim::SetHorizon(gt, asOf);

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

            // 5. One publication point, at the end of the job and nowhere
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
        // Read before taking our own lock — the accessor takes GossipSim's.
        const double anchor = GossipSim::LastSimulatedGameDay();

        std::scoped_lock lock(g_mutex);
        g_secondsSinceCheck = 0.0;
        // Re-derived rather than preserved OR blanked. Preserving it would
        // owe a day-3 schedule against a day-200 world. Blanking it — what
        // this used to do — restarted the entire interval on every load,
        // so a player whose sessions were shorter than one interval of
        // unpaused play never saw a single tick no matter how many in-game
        // days they played. The anchor gives both cases the right answer.
        g_nextDueGameDay = -1.0;
        g_sessionAnchorGameDay = anchor;

        // Logged HERE as well as at the first Poll, because Poll returns
        // early while the graph is not ready and would then never say
        // anything at all — leaving a gossip system that is scheduled to do
        // nothing indistinguishable from one that was never scheduled.
        if (anchor < 0.0) {
            logger::info("GossipTick: session start — no prior tick to anchor to; a full interval will be waited out");
        } else {
            logger::info("GossipTick: session start — schedule anchored at day {:.3f}", anchor);
        }
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
                // First check of the session.
                if (g_sessionAnchorGameDay < 0.0 || now < g_sessionAnchorGameDay) {
                    // Either this world has never run a tick, or its clock
                    // sits behind the anchor's — an older save, a console
                    // time change. Nothing is owed, and the first tick is
                    // due one full interval from now rather than
                    // immediately, which would sweep a world the player has
                    // not touched yet.
                    g_nextDueGameDay = now + interval;
                    logger::debug("GossipTick: no prior tick to resume from; first tick due at day {:.3f} (now {:.3f})",
                                  g_nextDueGameDay,
                                  now);
                    return;
                }
                // Resume the saved world's schedule. When more than one
                // interval has gone by since the last tick the boundary is
                // owed exactly ONCE, stamped for the present rather than
                // replayed once per missed interval: re-harvesting the same
                // memory corpus N times finds the same memories N times,
                // and one tick stamped `now` still drains every carrier
                // step in between, because Advance processes each queued
                // event at its own due time rather than at the horizon.
                g_nextDueGameDay = std::max(g_sessionAnchorGameDay + interval, now);
                logger::debug(
                    "GossipTick: resuming schedule; last tick day {:.3f}, next due day {:.3f} (now {:.3f}, interval "
                    "{:.3f}d)",
                    g_sessionAnchorGameDay,
                    g_nextDueGameDay,
                    now,
                    interval);
                // Deliberately no early return: a resumed schedule can
                // already be due, and the loop below is what fires it.
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
            logger::warn("GossipTick: dropped {} scheduled tick(s) beyond the {}-tick backlog cap",
                         dropped,
                         kMaxOutstandingTicks);
        }
    }
} // namespace NarrativeEngine::GossipTick
