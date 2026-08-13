#include <GossipHarvest.h>

#include <GossipDispatch.h>
#include <GossipThread.h>

#include <EventLogUtil.h>
#include <GossipClaims.h>
#include <GossipContent.h>
#include <GossipGraph.h>
#include <GossipLog.h>
#include <GossipSim.h>
#include <JsonUtils.h>
#include <logger.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
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
        // SkyrimNet API version that first exported PublicQueryMemoriesForActor.
        // Below it there is no way to keep gossip out of its own input at the
        // point that matters — before truncation — so the harvest does not run.
        constexpr int kMinApiVersion = 10;
        // Whether the "cannot sweep yet" note has already been emitted for
        // the current outage; reset the moment a sweep succeeds.
        bool g_deferredLogged = false;

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

        // The filter this harvest asks SkyrimNet to apply, instead of
        // applying it itself.
        //
        // All of it ran client-side until SkyrimNet API v10, and running it
        // client-side is what starved the harvest. The old endpoint ranks an
        // actor's WHOLE store and truncates at maxCount, so a filter applied
        // to the result only ever filtered what survived the cut — and
        // gossip's own writebacks are always the newest rows, so under
        // recency ordering they took every slot. Measured on a real save: 0
        // usable candidates out of 10 for an actor whose full corpus held 78.
        // The workarounds were to widen the fetch to 200 rows and bias the
        // ranking with a semantic contextQuery. Neither is needed once the
        // filter runs in SQL ahead of the truncation, and both are gone.
        MemoryQuery BuildQuery(const Settings::Config& cfg, double nowGameSeconds)
        {
            MemoryQuery q;
            q.maxCount = std::max(1, cfg.gossipHarvestMemoriesPerActor);

            // The feedback-loop guard, and the one filter gossip cannot run
            // without. Gossip writes memories; if those could seed rumors, a
            // rumor reaching twenty people would write forty memories, which
            // would become forty rumors, without bound.
            //
            // `ne_gossip`, NOT `gossip`. SkyrimNet's own tagger applies
            // `gossip` as a TOPICAL label to memories merely ABOUT
            // gossiping — 56 rows in 1602 on a real save, every one a real
            // memory, and exactly the social-drama material most worth
            // spreading. A tag nobody else writes cannot collide.
            q.excludeTags = {kOwnOutputTag};

            // Gossip is news. Both bounds are game-seconds, the same base the
            // row's `game_time` field and EventLogUtil::NowGameTimeSeconds
            // report, so no unit conversion is implied anywhere.
            //
            // The upper bound is the TICK'S HORIZON rather than the current
            // clock, which is what lets a tick that ran late still be an
            // exact tick: a memory written after the moment this tick
            // simulates belongs to the next one, and the query never sees it.
            q.gameTimeAfter = nowGameSeconds - static_cast<double>(cfg.gossipHarvestWindowDays) * 86400.0;
            q.gameTimeBefore = nowGameSeconds;

            q.minImportance = cfg.gossipMinMemoryImportance;

            // Newest first, and NOT Relevance.
            //
            // Relevance is the old semantic mode, and it can only return
            // memories already present in SkyrimNet's vector index; the SQL
            // orders see every eligible row. Completeness is what matters
            // here — a farmer in Rorikstead with one notable memory has to
            // be able to seed from it, and an embedding that has not been
            // generated yet would make them invisible. The semantic
            // contextQuery this used to pass existed only to dodge the
            // recency crowding above, and has nothing left to do.
            //
            // Recency rather than importance, because the importance floor
            // is already a filter: every row that comes back has cleared it,
            // so ordering by it again only decides WHICH qualifying memories
            // get dropped at the cut — and dropping the fresh ones is the
            // wrong answer twice over. Gossip is news, so a thing that
            // happened yesterday is better material than a slightly higher-
            // scoring one from forty days ago; and an actor's most important
            // memories are a nearly static set, so importance ordering hands
            // back the same rows sweep after sweep, with anything already
            // claimed occupying a slot until it ages out of the window
            // entirely. Under recency the window rotates itself.
            //
            // Picking the best of what comes back is the caller's job and it
            // already does it: RunSweepImpl ranks each owner's candidates by
            // importance, then fills the evaluation pool round-robin across
            // owners. So this cut chooses what an owner can offer, and that
            // ranking chooses what they do offer.
            q.orderBy = MemoryOrder::GameTimeDesc;
            return q;
        }

        // Why `row` should not have been returned at all, or "" if it
        // satisfies everything BuildQuery asked for.
        //
        // NOT a pruning pass — the query already excluded all of this, so
        // every one of these is dead weight in the normal case. It exists
        // because the failure it catches is silent and unbounded: if the
        // server-side tag exclusion ever stops holding, gossip re-seeds from
        // its own output and the only symptom is a mill that will not stop.
        // Cheap enough (three field reads) to be worth carrying permanently.
        std::string FilterViolation(const nlohmann::json& row, const Settings::Config& cfg, double nowGameSeconds)
        {
            if (const auto tagsIt = row.find("tags"); tagsIt != row.end() && tagsIt->is_array()) {
                const bool ours = std::any_of(tagsIt->begin(), tagsIt->end(), [](const auto& t) {
                    return t.is_string() && t.template get_ref<const std::string&>() == kOwnOutputTag;
                });
                if (ours) {
                    return "own-gossip";
                }
            }
            // A row with no `game_time` cannot satisfy a bounded range, so
            // its presence is the same failure as a row outside one. Guarded
            // explicitly rather than left to value()'s default of 0, which
            // would read as "written at game start" and look merely old.
            const auto gameTimeIt = row.find("game_time");
            if (gameTimeIt == row.end() || !gameTimeIt->is_number()) {
                return "no-game-time";
            }
            const double ageDays = (nowGameSeconds - gameTimeIt->get<double>()) / 86400.0;
            if (ageDays < 0.0) {
                return std::format("after-horizon ({:.1f}d)", -ageDays);
            }
            if (ageDays > cfg.gossipHarvestWindowDays) {
                return std::format("too-old ({:.1f}d)", ageDays);
            }
            if (row.value("importance_score", 0.0) < static_cast<double>(cfg.gossipMinMemoryImportance)) {
                return "low-importance";
            }
            return {};
        }

        // Stage 2: qualify this actor's recent memories.
        void CollectFrom(const GossipThread::Token& gt,
                         const GossipGraph::Participant& actor,
                         const Settings::Config& cfg,
                         double nowGameSeconds,
                         GossipLog::HarvestStats& sweep,
                         std::vector<Candidate>& out)
        {
            // SkyrimNet call: reference id.
            const auto raw = SkyrimNetAPI::QueryMemoriesForActor(actor.actorRef, BuildQuery(cfg, nowGameSeconds));
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
                             "QueryMemoriesForActor -> {}",
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
                // `importance_score` is the raw score, and the field the
                // query's minImportance bound is applied to. The row also
                // carries `decayed_importance`, which is far lower (0.05
                // against a raw 0.92 on a two-month-old memory) and decays
                // on the same real-world clock `age_hours` uses — so reading
                // that one here would put a real-world quantity back into a
                // pipeline that is otherwise entirely in-world.
                const auto importance = static_cast<float>(m.value("importance_score", 0.0));
                auto content = JsonUtils::StringOr(m, "content");

                // Everything above was asked of SkyrimNet in the query, so
                // this only ever fires when the server-side filter did NOT
                // hold. Loud, and counted separately from the rejections
                // that are genuinely ours to make: the tag exclusion is the
                // feedback-loop guard, and a silent failure of it is a mill
                // that will not stop.
                if (const auto reason = FilterViolation(m, cfg, nowGameSeconds); !reason.empty()) {
                    ++sweep.rejectedUnfiltered;
                    GossipLog::Memory(id, actor.npc, importance, std::format("unfiltered ({})", reason));
                    logger::warn("GossipHarvest: memory {} was returned despite the query excluding it "
                                 "({}); SkyrimNet's server-side filter did not hold",
                                 id,
                                 reason);
                    continue;
                }

                // Read for the trace only. There is no type allowlist any
                // more: it existed solely to keep gossip out of its own
                // input, back when `tags` was believed absent from the
                // response, and the tag check above does that job exactly.
                // Kept as a rejected/accepted annotation because knowing
                // WHICH types are seeding is worth a log column — on a real
                // save KNOWLEDGE is both the largest bucket and the
                // highest-scoring one, which is what the allowlist was
                // silently discarding.
                const auto type = JsonUtils::StringOr(m, "type");
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
                if (GossipClaims::IsClaimed(gt, id)) {
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
                if (GossipClaims::AreEventsClaimed(gt, eventIds)) {
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
        void Accumulate(const GossipThread::Token& gt, const GossipLog::HarvestStats& sweep)
        {
            auto& g_stats = GossipSim::MutableState(gt).harvest;
            g_stats.actorsExamined += sweep.bucketPopulation;
            g_stats.memoriesExamined += sweep.memoriesExamined;
            g_stats.sentForGeneration += sweep.sentForGeneration;
            g_stats.rejectedUnfiltered += sweep.rejectedUnfiltered;
            g_stats.rejectedClaimed += sweep.rejectedClaimed;
            g_stats.rejectedSameEvent += sweep.rejectedSameEvent;
            g_stats.rejectedIsolated += sweep.rejectedIsolated;
            g_stats.rejectedDiary += sweep.rejectedDiary;
            g_stats.rejectedNoContent += sweep.rejectedNoContent;
        }

        // Returns false when the sweep could not run at all. The caller
        // uses that to leave the accumulator alone, so a sweep that never
        // happened is retried rather than silently counted as done.
        // The "Locked" suffix is gone with the lock: this now runs on the
        // gossip thread and owns everything it touches for the duration.
        // Draw the bucket this sweep will examine, and remember it.
        //
        // Uniform over every bucket NOT in the recent history, which is
        // what makes the order look arbitrary while guaranteeing spread.
        // The history length is clamped to bucketCount-1 rather than
        // trusted: at bucketCount exactly nothing would be eligible, and
        // the setting is a knob a person turns.
        //
        // At the clamp the sequence degrades into a fixed repeating cycle
        // -- one bucket eligible, so one bucket drawn -- which is the same
        // thing as pre-shuffling a permutation, arrived at without any
        // special case for it.
        std::uint32_t DrawBucket(const GossipThread::Token& gt, std::uint32_t bucketCount)
        {
            auto& st = GossipSim::MutableState(gt);

            // A bucket count change is normally caught on load. This
            // covers the other route to the same place: settings reloaded
            // mid-session, with no serialisation callback involved.
            if (st.bucketCount != bucketCount) {
                if (st.bucketCount != 0) {
                    GossipLog::Note(std::format("harvest: bucket count changed {} -> {}; forgetting {} "
                                                "remembered selection(s)",
                                                st.bucketCount,
                                                bucketCount,
                                                st.bucketHistory.size()));
                }
                st.bucketHistory.clear();
                st.bucketCount = bucketCount;
            }

            // bucketCount-1 is both the default and the ceiling. At the
            // ceiling exactly one bucket is eligible, so the draw becomes a
            // fixed cycle and every bucket is examined every bucketCount
            // sweeps -- no tail. Anything shorter buys an arbitrary-looking
            // order that nothing in the game can observe, and pays for it
            // in worst-case waiting: at 10 buckets a history of 6 leaves
            // the mean wait at 10 sweeps but stretches the worst case to
            // 33.
            const int cap = std::max(0, static_cast<int>(bucketCount) - 1);
            const int configured = Settings::Get().gossipBucketHistoryLength;
            const auto maxHistory = static_cast<std::size_t>(configured < 0 ? cap : std::clamp(configured, 0, cap));
            while (st.bucketHistory.size() > maxHistory) {
                st.bucketHistory.pop_front();
            }

            std::vector<std::uint32_t> eligible;
            eligible.reserve(bucketCount);
            for (std::uint32_t i = 0; i < bucketCount; ++i) {
                if (std::find(st.bucketHistory.begin(), st.bucketHistory.end(), i) == st.bucketHistory.end()) {
                    eligible.push_back(i);
                }
            }
            if (eligible.empty()) {
                // Unreachable given the clamp above, and handled anyway:
                // a draw that returned nothing would skip a sweep in
                // silence, which is the one outcome with no trace of it.
                eligible.push_back(0);
            }

            std::uniform_int_distribution<std::size_t> pick(0, eligible.size() - 1);
            const auto drawn = eligible[pick(g_rng)];

            st.bucketHistory.push_back(drawn);
            while (st.bucketHistory.size() > maxHistory) {
                st.bucketHistory.pop_front();
            }
            return drawn;
        }

        std::string HistoryText(const GossipThread::Token& gt)
        {
            const auto& h = GossipSim::MutableState(gt).bucketHistory;
            if (h.empty()) {
                return "-";
            }
            std::string out;
            for (const auto b : h) {
                out += std::format("{}{}", out.empty() ? "" : ",", b);
            }
            return out;
        }

        bool RunSweepImpl(const GossipThread::Token& gt,
                          double nowGameDay,
                          const GossipDispatch::CancellationHandle& cancel)
        {
            // Session counters live in GossipState so the dashboard reads
            // them from the same image as the rumors they produced. Bound
            // through the token here rather than at file scope: a
            // static-init reference would have no token to present, which
            // is the gate doing its job.
            auto& g_stats = GossipSim::MutableState(gt).harvest;
            const auto& cfg = Settings::Get();
            // The filtered memory query is a hard requirement, not a
            // preference. On an older SkyrimNet the export does not resolve
            // and every query returns "[]", so the harvest would run in full
            // and report examining a bucket of actors who each turned out to
            // have nothing — a world where nothing memorable ever happens,
            // indistinguishable in the trace from a quiet save. Refuse the
            // sweep instead, and say why.
            const bool queryAvailable = SkyrimNetAPI::GetVersion() >= kMinApiVersion;
            if (!GossipGraph::IsReady() || !SkyrimNetAPI::IsMemorySystemReady() || !queryAvailable) {
                // Logged once per outage rather than every poll: this fires
                // every two seconds while it lasts. Before this said
                // anything, a sweep that fired and bailed here was
                // indistinguishable in the trace from one that never fired,
                // which is exactly the ambiguity that made a missing sweep
                // hard to account for.
                if (!g_deferredLogged) {
                    g_deferredLogged = true;
                    GossipLog::Note(std::format("harvest: deferred — graph ready={}, memory system ready={}, "
                                                "SkyrimNet API v{} (need v{}+ for the filtered memory query)",
                                                GossipGraph::IsReady(),
                                                SkyrimNetAPI::IsMemorySystemReady(),
                                                SkyrimNetAPI::GetVersion(),
                                                kMinApiVersion));
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

            // Drawn here, after the ready checks and beside the sweep
            // count, because a bucket's turn is spent by being drawn. A
            // draw above the checks would burn a turn on a sweep that
            // then bailed and examined nobody.
            const auto bucketCount = GossipGraph::BucketCount();
            const auto bucket = DrawBucket(gt, bucketCount);
            sweep.bucket = bucket;
            sweep.bucketCount = bucketCount;
            // The bucket and its population are on the HARVEST line; this
            // carries the one thing that line cannot, which is what the
            // draw was made AGAINST. Without it a repeat looks like a bug
            // rather than like a window that had rolled over.
            GossipLog::Note(std::format("harvest: bucket {} drawn, recent={}", bucket, HistoryText(gt)));

            // Every participant in the drawn bucket, in full. No ranking,
            // no top-N cut, and nothing here consults how much the player
            // has interacted with anyone — which is the entire point of
            // the milestone.
            const auto& members = GossipGraph::BucketMembers(bucket);
            sweep.bucketPopulation = members.size();
            if (members.empty()) {
                GossipLog::Harvest(sweep);
                Accumulate(gt, sweep);
                return true;
            }

            std::vector<Candidate> candidates;
            for (const auto npc : members) {
                const auto* p = GossipGraph::Find(npc);
                if (!p || !p->actorRef) {
                    // No placed reference means no id SkyrimNet will
                    // answer to. Rare — 881 of 881 resolve on a vanilla
                    // load order — and silent, because it is a property
                    // of the graph rather than of this sweep.
                    continue;
                }
                CollectFrom(gt, *p, cfg, nowGameDay * 86400.0, sweep, candidates);
            }
            sweep.candidates = candidates.size();

            std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
                return a.importance > b.importance;
            });

            // Fill the evaluation pool ROUND-ROBIN over owners, best-first
            // within each owner, then hand it over in a RANDOM order.
            //
            // Three separate jobs, and it matters that they stay separate:
            //
            // Importance ordering WITHIN an owner decides which of that
            // person's memories represents them. Round-robin ACROSS owners
            // decides how many slots each person gets. Shuffling decides
            // which pooled candidate actually seeds, and that has to be
            // random -- walking the pool in importance order would seed the
            // top candidate every time the evaluator accepted it, which is
            // a pool size of one wearing a disguise.
            //
            // The round-robin is what stops memory COUNT deciding who
            // seeds. A flat top-N by importance handed out tickets in
            // proportion to how many qualifying memories a person held, and
            // that number tracks proximity to the player above all else: a
            // follower is present for everything the player does, so they
            // accumulate rows nobody else can. One real bucket had Onmund
            // holding 10 of 11 candidates; another had two College mages
            // holding 20 of 25. The first pass now gives every owner with a
            // candidate exactly one slot, so whenever at least `attempts`
            // people have something notable, they are all equally likely.
            // Later passes backfill from whoever has depth, which keeps the
            // pool full when only one or two people have anything at all
            // rather than shrinking it -- a short pool means more sweeps
            // that seed nothing, and the refusal rate is high enough for
            // that to be a real cost.
            //
            // What this deliberately does NOT equalise is importance. An
            // owner's best memory still competes on its own merits once
            // pooled, and a follower's best usually outscores a farmer's.
            // Only the ticket COUNT is levelled.
            struct OwnerQueue
            {
                RE::FormID owner = 0;
                // This owner's candidates, most important first.
                std::vector<const Candidate*> byImportance;
                // Lazily evaluated on first touch. The isolation gate reads
                // a life state per contact, so it is by far the most
                // expensive test here, and it is a property of the OWNER
                // rather than of the memory -- under flat pooling one
                // unreachable actor paid it once per candidate, which on a
                // real sweep meant computing Ancano's "1% of contacts
                // reachable" sixteen times. Cached per owner, and only
                // owners the fill actually reaches ever compute it.
                std::optional<bool> reachable;
            };

            // `candidates` is already sorted by importance, so first-seen
            // order gives each owner their best memory first. That part is
            // wanted and is kept: it decides which of a person's memories
            // represents them.
            std::vector<OwnerQueue> queues;
            std::unordered_map<RE::FormID, std::size_t> queueOf;
            for (const auto& c : candidates) {
                const auto [it, inserted] = queueOf.try_emplace(c.owner, queues.size());
                if (inserted) {
                    queues.push_back(OwnerQueue{c.owner, {}, std::nullopt});
                }
                queues[it->second].byImportance.push_back(&c);
            }

            // The order the OWNERS are visited in, however, must be random.
            //
            // Built as above it is descending by whose best memory scores
            // highest, and that is a real bias rather than a tie-break: when
            // more people hold candidates than there are attempts, the first
            // pass never reaches the tail of the list, so the same few actors
            // take every slot every time their bucket comes up. Since memory
            // importance tracks proximity to the player, that is the same
            // follower advantage the round-robin exists to remove, re-entering
            // one level up.
            //
            // Shuffling here also settles the backfill, which walks this order
            // again on each later pass. `queueOf` is not read past this point,
            // so reordering does not invalidate anything.
            std::shuffle(queues.begin(), queues.end(), g_rng);

            const auto attempts = static_cast<std::size_t>(std::max(1, cfg.gossipEvalAttemptsPerHarvest));
            std::vector<GossipContent::Candidate> pool;
            pool.reserve(attempts);
            for (std::size_t round = 0; pool.size() < attempts; ++round) {
                // Set only by an owner who is both reachable and still has a
                // candidate at this depth, so a pass in which every
                // remaining owner is isolated or exhausted ends the fill
                // instead of spinning out to the longest queue's length.
                bool anyLeft = false;
                for (auto& q : queues) {
                    if (pool.size() >= attempts) {
                        break;
                    }
                    if (round >= q.byImportance.size()) {
                        continue;
                    }
                    if (!q.reachable.has_value()) {
                        const float share = GossipSim::AvailableContactShare(gt, q.owner);
                        q.reachable = share >= cfg.gossipMinAvailableContactShare;
                        if (!*q.reachable) {
                            // Counted once per OWNER now rather than once
                            // per memory: what the sweep skipped is a
                            // person, and how many rows they happened to
                            // hold is not what the number is answering.
                            ++sweep.rejectedIsolated;
                            const auto* best = q.byImportance.front();
                            GossipLog::Memory(best->memoryId,
                                              q.owner,
                                              best->importance,
                                              std::format("isolated (only {:.0f}% of contacts reachable) -- "
                                                          "{} candidate(s) skipped",
                                                          share * 100.0f,
                                                          q.byImportance.size()));
                            // The MEMORY line gives the verdict; this gives
                            // the arithmetic behind it. Once per skipped
                            // owner per sweep, which the per-owner caching
                            // above already bounds.
                            GossipLog::Note(GossipSim::DescribeContactAvailability(gt, q.owner));
                        }
                    }
                    if (!*q.reachable) {
                        continue;
                    }
                    anyLeft = true;

                    const auto& c = *q.byImportance[round];
                    // Two accounts of the same happening must not both take
                    // pool slots: whichever the shuffle reaches second is
                    // guaranteed to be skipped once the first claims the
                    // events, so it would occupy an attempt and buy nothing.
                    // The persistent ledger is still re-checked per step
                    // inside the walk -- this is only about not wasting
                    // slots within one sweep.
                    const bool overlapsPool = std::any_of(pool.begin(), pool.end(), [&](const auto& queued) {
                        return std::any_of(c.eventIds.begin(), c.eventIds.end(), [&](std::int64_t id) {
                            return std::find(queued.eventIds.begin(), queued.eventIds.end(), id)
                                   != queued.eventIds.end();
                        });
                    });
                    if (overlapsPool || GossipClaims::AreEventsClaimed(gt, c.eventIds)) {
                        ++sweep.rejectedSameEvent;
                        GossipLog::Memory(c.memoryId, c.owner, c.importance, "same-event (this sweep)");
                        continue;
                    }

                    const auto* p = GossipGraph::Find(c.owner);
                    const auto& locName = GossipGraph::LocationName(p ? (p->settlement ? p->settlement : p->hold) : 0);
                    pool.push_back(
                        GossipContent::Candidate{c.memoryId, c.owner, c.importance, c.text, locName, c.eventIds});
                }
                if (!anyLeft) {
                    break;
                }
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
            Accumulate(gt, sweep);
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
        out.actorsExamined = h.actorsExamined;
        out.memoriesExamined = h.memoriesExamined;
        out.sentForGeneration = h.sentForGeneration;
        out.rejectedUnfiltered = h.rejectedUnfiltered;
        out.rejectedClaimed = h.rejectedClaimed;
        out.rejectedDiary = h.rejectedDiary;
        out.rejectedNoContent = h.rejectedNoContent;
        out.rejectedSameEvent = h.rejectedSameEvent;
        out.rejectedIsolated = h.rejectedIsolated;
        return out;
    }
} // namespace NarrativeEngine::GossipHarvest
