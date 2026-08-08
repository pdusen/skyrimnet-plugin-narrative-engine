#include <GossipHarvest.h>

#include <EventLogUtil.h>
#include <GossipClaims.h>
#include <GossipContent.h>
#include <GossipGraph.h>
#include <GossipLog.h>
#include <GossipSim.h>
#include <logger.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace NarrativeEngine::GossipHarvest
{
    namespace
    {
        std::mutex g_mutex;
        double g_lastGameDaySample = -1.0;
        double g_gameHoursSinceSweep = 0.0;
        Stats g_stats;

        // A memory's timestamp is in the same cumulative game-seconds units
        // EventLogUtil::NowGameTimeSeconds returns, so ages are a plain
        // subtraction.
        double NowGameDay()
        {
            return EventLogUtil::NowGameTimeSeconds() / 86400.0;
        }

        struct Candidate
        {
            std::int64_t memoryId = 0;
            // The TESNPC base form — the graph's key, and what SeedRumor
            // and every log line expect.
            RE::FormID owner = 0;
            float importance = 0.0f;
            std::string text;
            // The happening this memory is an account of, as SkyrimNet's
            // own event ids. Claimed alongside the memory so no other
            // witness's account of the same happening can seed a second
            // rumor about it.
            std::vector<std::int64_t> eventIds;
        };

        std::vector<std::int64_t> ReadEventIds(const nlohmann::json& row)
        {
            std::vector<std::int64_t> out;
            const auto it = row.find("related_event_ids");
            if (it == row.end() || !it->is_array()) {
                return out;
            }
            out.reserve(it->size());
            for (const auto& e : *it) {
                if (e.is_number_integer() || e.is_number_unsigned()) {
                    out.push_back(e.get<std::int64_t>());
                }
            }
            return out;
        }

        // One ranked actor, carrying both halves of its identity.
        //
        // These are two different FormIDs for the same person and they are
        // not interchangeable. SkyrimNet speaks placed-reference ids;
        // GossipGraph is keyed by TESNPC base forms. Conflating them is
        // what made the first in-game harvest reject all 131 active actors
        // as not-participants: every engagement row was looked up in a map
        // that does not contain reference ids at all.
        struct RankedActor
        {
            RE::FormID actorRef = 0; // for SkyrimNet calls
            RE::FormID npc = 0;      // for the graph
        };

        // Engagement row -> participant. The prebuilt index answers almost
        // every row without touching the engine; the fallback covers
        // participants whose LCUN row carried no refID, and any actor
        // SkyrimNet reports under a reference the graph never saw.
        //
        // Both calls are safe here: LookupByID takes the engine's own
        // read-write lock over the all-forms map, and GetActorBase is a
        // field read.
        const GossipGraph::Participant* ResolveEngagementRow(RE::FormID actorRef)
        {
            if (const auto* p = GossipGraph::FindByActorRef(actorRef)) {
                return p;
            }
            if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorRef)) {
                if (auto* base = actor->GetActorBase()) {
                    return GossipGraph::Find(base->GetFormID());
                }
            }
            return nullptr;
        }

        // Stage 1: rank every active actor by how much important material
        // they have accumulated lately.
        std::vector<RankedActor> RankActors(const Settings::Config& cfg, GossipLog::HarvestStats& sweep)
        {
            const double windowSeconds = static_cast<double>(cfg.gossipHarvestWindowDays) * 86400.0;
            const auto raw = SkyrimNetAPI::GetActorEngagement(0,
                                                              /*excludePlayer*/ true,
                                                              /*playerEventsOnly*/ false,
                                                              /*shortWindow*/ 86400.0,
                                                              /*mediumWindow*/ windowSeconds);
            std::vector<std::pair<double, RankedActor>> ranked;
            try {
                const auto j = nlohmann::json::parse(raw, nullptr, false);
                if (!j.is_array()) {
                    return {};
                }
                // maxCount=0 returns EVERY actor with any activity, which can
                // be hundreds of rows. Parsed once per sweep, never per
                // candidate.
                sweep.actorsSeen = j.size();
                ranked.reserve(j.size());
                for (const auto& row : j) {
                    // SkyrimNet reports the placed reference, never the
                    // base form. Resolve before asking the graph anything.
                    const auto actorRef = static_cast<RE::FormID>(row.value("formId", 0));
                    const auto* p = actorRef ? ResolveEngagementRow(actorRef) : nullptr;
                    if (!p) {
                        // Not a graph participant: nowhere to seed from.
                        // Counted rather than dropped silently — a sweep
                        // that finds nothing because the graph is small
                        // reads very differently from one where the
                        // memories themselves did not qualify.
                        ++sweep.rejectedNotParticipant;
                        continue;
                    }
                    ranked.emplace_back(row.value("recentMemoryImportanceMedium", 0.0), RankedActor{actorRef, p->npc});
                }
            } catch (...) {
                logger::warn("GossipHarvest: could not parse GetActorEngagement response");
                return {};
            }

            // Sort on the score alone. Ties keep their relative order via
            // stable_sort, so a sweep that finds several actors at the same
            // engagement score picks the same ones each time rather than
            // shuffling on an unordered-map walk.
            std::stable_sort(
                ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            const auto keep = static_cast<std::size_t>(std::max(1, cfg.gossipHarvestActorSampleSize));
            if (ranked.size() > keep) {
                ranked.resize(keep);
            }
            std::vector<RankedActor> out;
            out.reserve(ranked.size());
            for (const auto& [score, actor] : ranked) {
                out.push_back(actor);
            }
            return out;
        }

        // Stage 2: qualify this actor's recent memories.
        void CollectFrom(const RankedActor& actor,
                         const Settings::Config& cfg,
                         double nowGameSeconds,
                         GossipLog::HarvestStats& sweep,
                         std::vector<Candidate>& out)
        {
            // SkyrimNet call: reference id.
            const auto raw =
                SkyrimNetAPI::GetMemoriesForActor(actor.actorRef, std::max(1, cfg.gossipHarvestMemoriesPerActor), "");
            nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
            if (!j.is_array()) {
                return;
            }
            // One raw row per session, verbatim, when debugging. The field
            // names on this endpoint are not what its header documents and
            // are not otherwise inspectable from a running game; dumping a
            // row is the only way to settle what it actually returns
            // without guessing. Costs one log line per session.
            if (static bool dumped = false; !dumped && Settings::Get().debugMode && !j.empty()) {
                dumped = true;
                logger::info("GossipHarvest: raw memory row as returned by "
                             "GetMemoriesForActor -> {}",
                             j.front().dump());
            }
            for (const auto& m : j) {
                ++sweep.memoriesExamined;
                if (!m.is_object()) {
                    continue;
                }
                const auto id = static_cast<std::int64_t>(m.value("id", 0));
                if (id == 0) {
                    continue;
                }

                // FIELD NAMES: see the note in the header. These come from
                // what the endpoint returns, NOT from its doc comment.
                //
                // `importance_score` is the raw score. The row also carries
                // `decayed_importance`, which is far lower (0.05 against a
                // raw 0.92 on a two-month-old memory) and decays on the same
                // real-world clock as age_hours below — so it would import
                // the same bug through a different door.
                const auto importance = static_cast<float>(m.value("importance_score", 0.0));
                auto content = m.value("content", std::string{});

                // IN-WORLD age, from `game_time`.
                //
                // NOT `age_hours`. That field is real-world elapsed time
                // since the row was written: a raw row dump showed
                // age_hours=1407.0 against creation_time 2026-06-10 for a
                // harvest run on 2026-08-08, which is 58.6 real days — while
                // the same memory was under half a game-day old. A save
                // picked up after a two-month break would have every memory
                // in it read as ancient, which is precisely what happened.
                //
                // `game_time` is game-seconds since game start, the same
                // base EventLogUtil::NowGameTimeSeconds returns
                // (Calendar::GetDaysPassed * 86400) — so this is a plain
                // subtraction in one consistent clock.
                //
                // Guard the missing-field case explicitly rather than
                // letting it default to 0: a 0 would read as "written at
                // game start", i.e. maximally old, and silently reject
                // everything. That failure mode has already cost two test
                // runs on this endpoint.
                const auto gameTimeIt = m.find("game_time");
                if (gameTimeIt == m.end() || !gameTimeIt->is_number()) {
                    ++sweep.rejectedNoGameTime;
                    GossipLog::Memory(id, actor.npc, importance, "no-game-time");
                    continue;
                }
                const double ageDays = (nowGameSeconds - gameTimeIt->get<double>()) / 86400.0;
                if (ageDays > cfg.gossipHarvestWindowDays || ageDays < 0.0) {
                    ++sweep.rejectedTooOld;
                    GossipLog::Memory(id, actor.npc, importance, std::format("too-old ({:.1f}d)", ageDays));
                    continue;
                }
                if (importance < cfg.gossipMinMemoryImportance) {
                    ++sweep.rejectedLowImportance;
                    GossipLog::Memory(id, actor.npc, importance, "low-importance");
                    continue;
                }
                // Read for the trace only. There is no type allowlist any
                // more: it existed solely to keep gossip out of its own
                // input, back when `tags` was believed absent from the
                // response, and the tag check below does that job exactly.
                // Kept as a rejected/accepted annotation because knowing
                // WHICH types are seeding is worth a log column — on a real
                // save KNOWLEDGE is both the largest bucket and the
                // highest-scoring one, which is what the allowlist was
                // silently discarding.
                const auto type = m.value("type", std::string{});
                // The DIRECT feedback-loop guard.
                //
                // The design was built on "there is NO tags field, so the
                // ["gossip"] tag written at AddMemory time cannot be filtered
                // on at read time" — which is why the type allowlist was made
                // to carry that job. The raw row dump falsifies it: `tags` is
                // right there on the row. Checking it is exact where the type
                // rule is a proxy, and it keeps holding if the allowlist is
                // ever widened to admit KNOWLEDGE.
                if (const auto tagsIt = m.find("tags"); tagsIt != m.end() && tagsIt->is_array()) {
                    const bool ours = std::any_of(tagsIt->begin(), tagsIt->end(), [](const auto& t) {
                        return t.is_string() && t.template get_ref<const std::string&>() == "gossip";
                    });
                    if (ours) {
                        ++sweep.rejectedOwnOutput;
                        GossipLog::Memory(id, actor.npc, importance, "own-gossip");
                        continue;
                    }
                }
                // SkyrimNet folds diary entries into the same table this
                // endpoint queries, typed EXPERIENCE — which is one of our
                // source types, so they qualify on every other rule. There
                // is no server-side flag; the content prefix is the only
                // discriminator, exactly as the letter and visit composers
                // found. A diary entry is a character's own written summary,
                // not an event someone witnessed, and gossip sourced from
                // one reads as a rumor about somebody's journal.
                if (content.rfind("Diary Entry:", 0) == 0) {
                    ++sweep.rejectedDiary;
                    GossipLog::Memory(id, actor.npc, importance, "diary-entry");
                    continue;
                }
                if (content.empty()) {
                    // Nothing for the seed prompt to work from.
                    ++sweep.rejectedNoContent;
                    GossipLog::Memory(id, actor.npc, importance, "no-content");
                    continue;
                }
                if (GossipClaims::IsClaimed(id)) {
                    ++sweep.rejectedClaimed;
                    GossipLog::Memory(id, actor.npc, importance, "claimed");
                    continue;
                }
                // Somebody else's account of the same happening has already
                // become a rumor. SkyrimNet writes a separate memory to
                // every actor present, each with its own id but sharing
                // related_event_ids, so without this three witnesses to one
                // confrontation produce three rumors about it.
                auto eventIds = ReadEventIds(m);
                if (GossipClaims::AreEventsClaimed(eventIds)) {
                    ++sweep.rejectedSameEvent;
                    GossipLog::Memory(id, actor.npc, importance, "same-event");
                    continue;
                }
                GossipLog::Memory(id, actor.npc, importance, std::format("candidate ({})", type));
                out.push_back(Candidate{id, actor.npc, importance, std::move(content), std::move(eventIds)});
            }
        }

        // Session totals. The log carries per-sweep numbers; these are the
        // "how has this session behaved overall" view.
        void Accumulate(const GossipLog::HarvestStats& sweep)
        {
            g_stats.actorsRanked += sweep.actorsSampled;
            g_stats.memoriesExamined += sweep.memoriesExamined;
            g_stats.sentForGeneration += sweep.sentForGeneration;
            g_stats.rejectedTooOld += sweep.rejectedTooOld;
            g_stats.rejectedLowImportance += sweep.rejectedLowImportance;
            g_stats.rejectedClaimed += sweep.rejectedClaimed;
            g_stats.rejectedSameEvent += sweep.rejectedSameEvent;
            g_stats.rejectedNotParticipant += sweep.rejectedNotParticipant;
            g_stats.rejectedDiary += sweep.rejectedDiary;
            g_stats.rejectedNoContent += sweep.rejectedNoContent;
            g_stats.rejectedNoGameTime += sweep.rejectedNoGameTime;
            g_stats.rejectedOwnOutput += sweep.rejectedOwnOutput;
        }

        void RunSweepLocked(double nowGameDay)
        {
            const auto& cfg = Settings::Get();
            if (!GossipGraph::IsReady() || !SkyrimNetAPI::IsMemorySystemReady()) {
                return;
            }
            ++g_stats.sweeps;

            // Counted for THIS sweep, then folded into the session totals
            // at the end. Mixing the two scales in one line is what made
            // the first version of this log unreadable: a per-sweep actor
            // count next to a since-session-start rejection count cannot
            // be read as a rate, and the rate is the whole question.
            GossipLog::HarvestStats sweep;

            const auto actors = RankActors(cfg, sweep);
            sweep.actorsSampled = actors.size();
            if (actors.empty()) {
                GossipLog::Harvest(sweep);
                Accumulate(sweep);
                return;
            }

            std::vector<Candidate> candidates;
            for (const auto& a : actors) {
                CollectFrom(a, cfg, nowGameDay * 86400.0, sweep, candidates);
            }
            sweep.candidates = candidates.size();

            std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
                return a.importance > b.importance;
            });

            const auto want = static_cast<std::size_t>(std::max(1, cfg.gossipMaxSeedsPerHarvest));
            for (const auto& c : candidates) {
                if (sweep.sentForGeneration >= want) {
                    break;
                }
                // Re-check the events here, not just at qualification.
                // Nothing is claimed until this loop runs, so two accounts
                // of the same happening can both have passed the gate
                // above; the first one through claims the events and the
                // second must see that.
                if (GossipClaims::AreEventsClaimed(c.eventIds)) {
                    ++sweep.rejectedSameEvent;
                    GossipLog::Memory(c.memoryId, c.owner, c.importance, "same-event (this sweep)");
                    continue;
                }
                // Claim BEFORE generating. The LLM call is async and can
                // fail or be refused, at which point GossipContent releases
                // the claim — so a rumor can never exist without a claim
                // behind it, and a refused memory is handed straight back.
                // The related events are claimed with it and come back with
                // it on release.
                GossipClaims::Claim(c.memoryId, c.eventIds, nowGameDay);
                const auto* p = GossipGraph::Find(c.owner);
                const auto& locName = GossipGraph::LocationName(p ? (p->settlement ? p->settlement : p->hold) : 0);
                GossipContent::RequestBands(c.memoryId, c.owner, c.text, locName, c.importance);
                ++sweep.sentForGeneration;
            }
            if (candidates.size() > want) {
                // Named rather than dropped quietly: a sweep that leaves
                // material on the table is the signal that
                // iGossipMaxSeedsPerHarvest is the binding constraint on
                // the seeding rate, not the qualification rules.
                GossipLog::Note(std::format(
                    "harvest: {} candidate(s) not seeded this sweep (cap {})", candidates.size() - want, want));
            }

            GossipLog::Harvest(sweep);
            Accumulate(sweep);
        }
    } // namespace

    void Initialize()
    {
        const auto& cfg = Settings::Get();
        logger::info("GossipHarvest: initialized (enabled={}, every {}h, window {}d, minImportance {})",
                     cfg.gossipHarvestEnabled,
                     cfg.gossipHarvestIntervalGameHours,
                     cfg.gossipHarvestWindowDays,
                     cfg.gossipMinMemoryImportance);
    }

    void OnSessionStart()
    {
        std::scoped_lock lock(g_mutex);
        g_lastGameDaySample = -1.0;
        g_gameHoursSinceSweep = 0.0;
        g_stats = {};
    }

    void Poll(const PluginThread::Token&, double)
    {
        const auto& cfg = Settings::Get();
        if (!cfg.gossipEnabled || !cfg.gossipHarvestEnabled) {
            return;
        }

        std::scoped_lock lock(g_mutex);
        const double now = NowGameDay();
        if (g_lastGameDaySample < 0.0) {
            g_lastGameDaySample = now;
            return;
        }
        const double deltaHours = (now - g_lastGameDaySample) * 24.0;
        g_lastGameDaySample = now;
        if (deltaHours <= 0.0) {
            return;
        }
        // Clamped for the same reason GossipSim clamps: a console time jump
        // should not trigger a burst of sweeps.
        g_gameHoursSinceSweep += std::min(deltaHours, 72.0);

        const double interval = std::max(0.1f, cfg.gossipHarvestIntervalGameHours);
        if (g_gameHoursSinceSweep < interval) {
            return;
        }
        // Subtract rather than zero so overshoot rolls forward, but never
        // run more than one sweep per poll.
        g_gameHoursSinceSweep = std::min(g_gameHoursSinceSweep - interval, interval);
        RunSweepLocked(now);
    }

    Stats GetStats()
    {
        std::scoped_lock lock(g_mutex);
        return g_stats;
    }
} // namespace NarrativeEngine::GossipHarvest
