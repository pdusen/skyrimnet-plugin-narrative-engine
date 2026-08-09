#include <GossipHarvest.h>

#include <GossipDispatch.h>
#include <GossipThread.h>

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
#include <random>
#include <string>
#include <vector>

namespace NarrativeEngine::GossipHarvest
{
    namespace
    {
        // Decides the order the candidate pool is walked in, which decides
        // which memory seeds. Separate from GossipSim's generator so that
        // seeding order does not shift whenever transmission draws change,
        // but seeded from the same iGossipRandomSeed so a run stays
        // reproducible end to end.
        std::mt19937 g_rng{7919};
        // Whether the "cannot sweep yet" note has already been emitted for
        // the current outage; reset the moment a sweep succeeds.
        bool g_deferredLogged = false;
        // Session counters live in GossipState so the dashboard reads
        // them from the same image as the rumors they produced. A sweep
        // count and a rumor count caught at different instants cannot be
        // reconciled by whoever is looking at them.
        auto& g_stats = GossipSim::MutableState().harvest;

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
                // `nowGameSeconds` is the TICK'S HORIZON, not the current
                // clock. A negative age therefore means the memory was
                // written after the moment this tick is simulating, and is
                // discarded for being in the future rather than for being
                // stale — the next tick will pick it up.
                //
                // The two share a counter but not a trace message, because
                // "the harvest window is too short" and "the queue ran
                // late" are diagnosed completely differently.
                const double ageDays = (nowGameSeconds - gameTimeIt->get<double>()) / 86400.0;
                if (ageDays < 0.0) {
                    ++sweep.rejectedTooOld;
                    GossipLog::Memory(id, actor.npc, importance, std::format("after-horizon ({:.1f}d)", -ageDays));
                    continue;
                }
                if (ageDays > cfg.gossipHarvestWindowDays) {
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
            g_stats.rejectedIsolated += sweep.rejectedIsolated;
            g_stats.rejectedNotParticipant += sweep.rejectedNotParticipant;
            g_stats.rejectedDiary += sweep.rejectedDiary;
            g_stats.rejectedNoContent += sweep.rejectedNoContent;
            g_stats.rejectedNoGameTime += sweep.rejectedNoGameTime;
            g_stats.rejectedOwnOutput += sweep.rejectedOwnOutput;
        }

        // Returns false when the sweep could not run at all. The caller
        // uses that to leave the accumulator alone, so a sweep that never
        // happened is retried rather than silently counted as done.
        // The "Locked" suffix is gone with the lock: this now runs on the
        // gossip thread and owns everything it touches for the duration.
        bool RunSweepImpl(const GossipThread::Token& gt,
                          double nowGameDay,
                          const GossipDispatch::CancellationHandle& cancel)
        {
            const auto& cfg = Settings::Get();
            if (!GossipGraph::IsReady() || !SkyrimNetAPI::IsMemorySystemReady()) {
                // Logged once per outage rather than every poll: this fires
                // every two seconds while it lasts. Before this said
                // anything, a sweep that fired and bailed here was
                // indistinguishable in the trace from one that never fired,
                // which is exactly the ambiguity that made a missing sweep
                // hard to account for.
                if (!g_deferredLogged) {
                    g_deferredLogged = true;
                    GossipLog::Note(std::format("harvest: deferred — graph ready={}, memory system ready={}",
                                                GossipGraph::IsReady(),
                                                SkyrimNetAPI::IsMemorySystemReady()));
                }
                return false;
            }
            g_deferredLogged = false;
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
                return true;
            }

            std::vector<Candidate> candidates;
            for (const auto& a : actors) {
                CollectFrom(a, cfg, nowGameDay * 86400.0, sweep, candidates);
            }
            sweep.candidates = candidates.size();

            std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
                return a.importance > b.importance;
            });

            // Take the best `attempts` candidates by importance, then hand
            // them over in a RANDOM order.
            //
            // Both halves matter. Ranking by importance decides WHICH
            // memories are worth an evaluation at all; shuffling decides
            // which of them actually seeds, and that has to be random —
            // walking the pool in importance order would seed the top
            // candidate every time the evaluator accepted it, which is a
            // pool size of one wearing a disguise.
            //
            // Shuffled-then-walked is equivalent to evaluating the whole
            // pool and picking an accepted one at random: within a uniform
            // permutation the accepted candidates are in uniform relative
            // order, so the first one reached is uniform over them. It just
            // costs the evaluations actually needed to find it rather than
            // one per candidate.
            const auto attempts = static_cast<std::size_t>(std::max(1, cfg.gossipEvalAttemptsPerHarvest));
            std::vector<GossipContent::Candidate> pool;
            pool.reserve(attempts);
            for (const auto& c : candidates) {
                if (pool.size() >= attempts) {
                    break;
                }
                // An origin whose contacts are almost all unreachable will
                // spend its whole quota on people who are away and reach
                // nobody. Checked HERE rather than at qualification: it is
                // the most expensive test in the pipeline (a life-state
                // read per contact) and only the candidates in line for an
                // evaluation need it. Failing it moves on to the next
                // candidate rather than ending the sweep.
                if (const float share = GossipSim::AvailableContactShare(c.owner);
                    share < cfg.gossipMinAvailableContactShare) {
                    ++sweep.rejectedIsolated;
                    GossipLog::Memory(c.memoryId,
                                      c.owner,
                                      c.importance,
                                      std::format("isolated (only {:.0f}% of contacts reachable)", share * 100.0f));
                    continue;
                }

                // Two accounts of the same happening must not both take pool
                // slots: whichever the shuffle reaches second is guaranteed
                // to be skipped once the first claims the events, so it
                // would occupy an attempt and buy nothing. The persistent
                // ledger is still re-checked per step inside the walk — this
                // is only about not wasting slots within one sweep.
                const bool overlapsPool = std::any_of(pool.begin(), pool.end(), [&](const auto& queued) {
                    return std::any_of(c.eventIds.begin(), c.eventIds.end(), [&](std::int64_t id) {
                        return std::find(queued.eventIds.begin(), queued.eventIds.end(), id) != queued.eventIds.end();
                    });
                });
                if (overlapsPool || GossipClaims::AreEventsClaimed(c.eventIds)) {
                    ++sweep.rejectedSameEvent;
                    GossipLog::Memory(c.memoryId, c.owner, c.importance, "same-event (this sweep)");
                    continue;
                }

                const auto* p = GossipGraph::Find(c.owner);
                const auto& locName = GossipGraph::LocationName(p ? (p->settlement ? p->settlement : p->hold) : 0);
                pool.push_back(
                    GossipContent::Candidate{c.memoryId, c.owner, c.importance, c.text, locName, c.eventIds});
            }

            if (!pool.empty()) {
                std::shuffle(pool.begin(), pool.end(), g_rng);
                sweep.sentForGeneration = pool.size();
                // Nothing is claimed yet. The walk claims each candidate
                // immediately before its own evaluation and settles that
                // claim on every path out, so a rumor can never exist
                // without a claim behind it while the candidates the walk
                // never reaches are never claimed at all.
                GossipContent::RequestRumors(
                    gt, std::move(pool), std::max(1, cfg.gossipMaxSeedsPerHarvest), nowGameDay, cancel);
            }

            if (candidates.size() > attempts) {
                // Named rather than dropped quietly: a sweep that leaves
                // material on the table is the signal that
                // iGossipEvalAttemptsPerHarvest is the binding constraint on
                // what gets considered, not the qualification rules.
                GossipLog::Note(std::format("harvest: {} candidate(s) beyond the evaluation pool this sweep (pool {})",
                                            candidates.size() - attempts,
                                            attempts));
            }

            GossipLog::Harvest(sweep);
            Accumulate(sweep);
            return true;
        }
    } // namespace

    void Initialize()
    {
        const auto& cfg = Settings::Get();
        if (cfg.gossipRandomSeed != 0) {
            g_rng.seed(static_cast<std::uint32_t>(cfg.gossipRandomSeed));
        } else {
            std::random_device rd;
            g_rng.seed(rd());
        }
        logger::info("GossipHarvest: initialized (enabled={}, every {}h, window {}d, minImportance {}, pool {})",
                     cfg.gossipHarvestEnabled,
                     cfg.gossipHarvestIntervalGameHours,
                     cfg.gossipHarvestWindowDays,
                     cfg.gossipMinMemoryImportance,
                     cfg.gossipEvalAttemptsPerHarvest);
    }

    void OnSessionStart()
    {
        // No lock. Harvest's only remaining shared state is the session
        // counter block inside GossipState, and this runs at kNewGame /
        // kPostLoadGame on the main thread — before Tick has restarted,
        // so no tick can be in flight.
        //
        // Taking a lock here would be actively harmful now that a sweep
        // blocks on two LLM calls: loading a game would hitch behind a
        // model round trip.
        g_stats = {};
    }

    bool RunSweep(const GossipThread::Token& gt, double asOfGameDay, const GossipDispatch::CancellationHandle& cancel)
    {
        const auto& cfg = Settings::Get();
        if (!cfg.gossipEnabled || !cfg.gossipHarvestEnabled) {
            return false;
        }

        // No accumulator, no owed-sweep arithmetic, no clamp. The
        // scheduler decides WHEN a sweep is due and hands this the game
        // time it is due FOR; this function's only job is to run one
        // sweep against that horizon.
        //
        // The owed-sweep backlog that used to live here now lives in the
        // job queue: two crossed interval boundaries enqueue two stamped
        // ticks, and the queue runs them in schedule order.
        // Deliberately unlocked. This call blocks for as long as two LLM
        // round trips, and it is only ever entered from the gossip
        // thread, so a mutex here would buy nothing and could only ever
        // stall whoever else took it.
        return RunSweepImpl(gt, asOfGameDay, cancel);
    }

    Stats GetStats(const GossipState& st)
    {
        const auto& h = st.harvest;
        Stats out;
        out.sweeps = h.sweeps;
        out.actorsRanked = h.actorsRanked;
        out.memoriesExamined = h.memoriesExamined;
        out.sentForGeneration = h.sentForGeneration;
        out.rejectedTooOld = h.rejectedTooOld;
        out.rejectedLowImportance = h.rejectedLowImportance;
        out.rejectedNotParticipant = h.rejectedNotParticipant;
        out.rejectedClaimed = h.rejectedClaimed;
        out.rejectedDiary = h.rejectedDiary;
        out.rejectedNoContent = h.rejectedNoContent;
        out.rejectedNoGameTime = h.rejectedNoGameTime;
        out.rejectedOwnOutput = h.rejectedOwnOutput;
        out.rejectedSameEvent = h.rejectedSameEvent;
        out.rejectedIsolated = h.rejectedIsolated;
        return out;
    }
} // namespace NarrativeEngine::GossipHarvest
