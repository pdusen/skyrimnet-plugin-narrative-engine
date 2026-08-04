#include <GossipSeeder.h>

#include <EventLogUtil.h>
#include <GossipGraph.h>
#include <GossipLog.h>
#include <GossipSim.h>
#include <logger.h>
#include <Settings.h>

#include <algorithm>
#include <format>
#include <random>
#include <vector>

namespace NarrativeEngine::GossipSeeder
{
    namespace
    {
        struct Slice
        {
            const char* label;
            int weight; // share of the total seed count
            int minResidents;
            int maxResidents;
            float notabilityLo;
            float notabilityHi;
        };

        // Weights are relative and sum to 12, which is the default seed
        // count, so the default run is exactly the mix documented in the
        // header.
        constexpr Slice kSlices[] = {
            {"large-settlement", 4, 40, 100000, 0.90f, 1.00f},
            {"mid-settlement", 3, 6, 20, 0.50f, 0.70f},
            {"small-settlement", 3, 1, 5, 0.50f, 0.70f},
            {"low-notability", 2, 1, 100000, 0.20f, 0.30f},
        };

        std::vector<RE::FormID> SettlementsInRange(int lo, int hi)
        {
            std::vector<RE::FormID> out;
            for (const auto loc : GossipGraph::Settlements()) {
                const auto n = static_cast<int>(GossipGraph::SettlementMembers(loc).size());
                if (n >= lo && n <= hi) {
                    out.push_back(loc);
                }
            }
            std::sort(out.begin(), out.end());
            return out;
        }

        // Session state for the periodic seeder.
        std::mt19937 g_rng{1337};
        int g_seededTotal = 0;
        double g_gameHoursSinceSeed = 0.0;
        double g_lastGameDaySample = -1.0;
        std::size_t g_nextSlice = 0;

        // Plant one rumor from `slice`. Returns true if it took.
        bool SeedOne(const Slice& slice)
        {
            auto candidates = SettlementsInRange(slice.minResidents, slice.maxResidents);
            if (candidates.empty()) {
                return false;
            }
            std::uniform_int_distribution<std::size_t> pickLoc(0, candidates.size() - 1);
            const auto loc = candidates[pickLoc(g_rng)];
            const auto& members = GossipGraph::SettlementMembers(loc);
            if (members.empty()) {
                return false;
            }
            std::uniform_int_distribution<std::size_t> pickNpc(0, members.size() - 1);
            const auto origin = members[pickNpc(g_rng)];
            std::uniform_real_distribution<float> pickNote(slice.notabilityLo, slice.notabilityHi);
            return GossipSim::SeedRumor(origin, pickNote(g_rng), slice.label) != 0;
        }
    } // namespace

    void OnSessionStart()
    {
        const auto& cfg = Settings::Get();
        if (!cfg.gossipEnabled || !cfg.gossipSeedStubsOnLoad) {
            return;
        }
        if (!GossipGraph::IsReady()) {
            logger::warn("GossipSeeder: graph not ready; nothing seeded");
            return;
        }

        // Do not double-seed a save that already carries live rumors —
        // reloading mid-run should resume, not restart.
        if (GossipSim::GetStats().liveRumors > 0) {
            logger::info("GossipSeeder: save already has live rumors; skipping stub seeding");
            return;
        }

        g_rng.seed(cfg.gossipRandomSeed != 0 ? static_cast<std::uint32_t>(cfg.gossipRandomSeed)
                                             : std::random_device{}());
        g_seededTotal = 0;
        g_gameHoursSinceSeed = 0.0;
        g_lastGameDaySample = -1.0;
        g_nextSlice = 0;

        const int total = std::max(1, cfg.gossipStubSeedCount);
        int weightSum = 0;
        for (const auto& s : kSlices) {
            weightSum += s.weight;
        }

        int seeded = 0;
        int attempted = 0;
        for (const auto& slice : kSlices) {
            const int want = std::max(1, (total * slice.weight) / std::max(1, weightSum));
            if (SettlementsInRange(slice.minResidents, slice.maxResidents).empty()) {
                logger::warn("GossipSeeder: no settlement with {}-{} residents for slice '{}'",
                             slice.minResidents,
                             slice.maxResidents,
                             slice.label);
                continue;
            }
            for (int i = 0; i < want; ++i) {
                ++attempted;
                if (SeedOne(slice)) {
                    ++seeded;
                    ++g_seededTotal;
                }
            }
        }

        const auto summary = std::format("seeder planted {}/{} stub rumors across {} settlements "
                                         "(rngSeed={}, participants={}, reseed every {}h up to {})",
                                         seeded,
                                         attempted,
                                         GossipGraph::Settlements().size(),
                                         cfg.gossipRandomSeed,
                                         GossipGraph::ParticipantCount(),
                                         cfg.gossipStubSeedIntervalGameHours,
                                         cfg.gossipStubSeedMaxTotal);
        GossipLog::Note(summary);
        logger::info("GossipSeeder: {}", summary);
    }

    void Poll(const PluginThread::Token&, double)
    {
        const auto& cfg = Settings::Get();
        if (!cfg.gossipEnabled || !cfg.gossipSeedStubsOnLoad || cfg.gossipStubSeedIntervalGameHours <= 0) {
            return;
        }
        if (!GossipGraph::IsReady() || g_seededTotal >= cfg.gossipStubSeedMaxTotal) {
            return;
        }

        // Paced on in-world time, so a validation run seeds at the same
        // rate whether the player is idling, waiting, or running at an
        // accelerated timescale.
        const double nowDay = NarrativeEngine::EventLogUtil::NowGameTimeSeconds() / 86400.0;
        if (g_lastGameDaySample < 0.0) {
            g_lastGameDaySample = nowDay;
            return;
        }
        const double deltaHours = (nowDay - g_lastGameDaySample) * 24.0;
        g_lastGameDaySample = nowDay;
        if (deltaHours <= 0.0) {
            return;
        }
        // Clamp for the same reason GossipSim clamps: a console time
        // jump should not plant a hundred rumors at once.
        g_gameHoursSinceSeed += std::min(deltaHours, 72.0);

        const double interval = static_cast<double>(cfg.gossipStubSeedIntervalGameHours);
        while (g_gameHoursSinceSeed >= interval && g_seededTotal < cfg.gossipStubSeedMaxTotal) {
            g_gameHoursSinceSeed -= interval;
            // Round-robin the slices so the sample stays stratified
            // across a long run instead of drifting toward whichever
            // slice happens to win the die rolls.
            const auto& slice = kSlices[g_nextSlice % std::size(kSlices)];
            ++g_nextSlice;
            if (SeedOne(slice)) {
                ++g_seededTotal;
            }
        }
    }
} // namespace NarrativeEngine::GossipSeeder
