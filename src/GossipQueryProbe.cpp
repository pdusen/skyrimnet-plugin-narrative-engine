#include <GossipQueryProbe.h>

#include <EventLogUtil.h>
#include <GossipDispatch.h>
#include <GossipGraph.h>
#include <GossipHarvest.h>
#include <GossipThread.h>
#include <JsonUtils.h>
#include <logger.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace NarrativeEngine::GossipQueryProbe
{
    namespace
    {
        // Brelyna Maryon, 0001C196 in Skyrim.esm. Chosen because she is
        // known to hold a healthy mix of both kinds of memory — a probe
        // against an actor with only one kind proves nothing.
        //
        // This is the TESNPC base form. SkyrimNet speaks placed
        // references, so it goes through GossipGraph::ActorRefFor before
        // any call.
        constexpr RE::FormID kSubjectNpc = 0x0001C196;

        // Seconds of unpaused play before firing. Long enough that
        // SkyrimNet's memory system has come up and its DB is open.
        constexpr double kDelaySeconds = 20.0;

        struct Variant
        {
            std::string_view label;
            std::string_view query;
        };

        // Round two. The filter-syntax question is settled — nothing is
        // parsed, the three probes containing the literal word "gossip"
        // scored worst of the group — so only one survives as a sanity
        // check, and the freed slots went to recency.
        constexpr std::array kVariants{
            // --- control -------------------------------------------------
            // Empty means "no vector, sort by timestamp". Its usable rows
            // are by construction the NEWEST, which is exactly the shape
            // gossip wants; the problem was only ever that ten rows is too
            // few to reach past our own writebacks.
            Variant{"control-empty", ""},

            // --- negation, kept as the demonstration ---------------------
            Variant{"neg-not-gossip", "not gossip"},
            Variant{"neg-no-rumor", "not a rumor, not something I heard from someone else"},

            // --- one filter probe, as a control on the control -----------
            Variant{"filter-type-exp", "type:EXPERIENCE"},

            // --- first-hand form, no topic (round one's winners) ---------
            Variant{"form-witnessed", "a notable thing that happened to me, or that I witnessed myself"},
            Variant{"form-firsthand", "something I experienced firsthand"},
            Variant{"form-i-was-there", "I was there when it happened"},
            Variant{"form-own-eyes", "what I saw with my own eyes"},
            Variant{"form-did-and-happened", "what I did, and what happened to me"},
            Variant{"form-took-part", "an event I took part in"},
            Variant{"form-long",
                    "A memory of an event I personally participated in or witnessed, described in the first "
                    "person, not something another person reported to me."},

            // --- terse experiential vocabulary ---------------------------
            Variant{"vocab-personal", "personal experience"},
            Variant{"vocab-direct-obs", "direct observation"},
            Variant{"vocab-happened", "what happened"},

            // --- NEW: recency ---------------------------------------------
            //
            // The embedding is computed from TEXT ALONE; game_time is a
            // separate column the vector never sees. So these can only work
            // through a loophole: memories whose own wording mentions time.
            // Expect little. What matters is whether the recency words cost
            // any of the gossip separation the form queries bought — the
            // pairing below (form-i-was-there vs recent-there-recently) is
            // the controlled comparison.
            Variant{"recent-just-happened", "something that just happened to me"},
            Variant{"recent-recently", "what happened to me recently"},
            Variant{"recent-witnessed", "a recent event I witnessed"},
            Variant{"recent-today", "what happened today"},
            Variant{"recent-there-recently", "I was there when it happened, recently"},
            Variant{"recent-lately-saw", "lately I saw something happen myself"},

            // --- topical: the ceiling, and the bias ----------------------
            Variant{"topic-college", "the College of Winterhold and its people"},
            Variant{"topic-doing", "adventuring, spellcasting, study, work, travel, conflict"},
        };

        constexpr int kShallow = 10;
        constexpr int kDeep = 50;
        // Deep enough to be the actor's whole corpus rather than a page of
        // it. Pulled once with an empty query, which is SkyrimNet's
        // recency ordering, and used as the yardstick every variant is
        // measured against.
        constexpr int kReference = 500;

        std::mutex g_mutex;
        bool g_armed = false;
        bool g_fired = false;
        double g_elapsed = 0.0;

        struct Row
        {
            std::int64_t id = 0;
            bool gossip = false;
            // Age at the moment of the probe, in game days. Negative means
            // the row is stamped in the future — which every memory written
            // through PublicAddMemory is, because that call takes no
            // timestamp and SkyrimNet stamps it with wall-clock seconds.
            double ageDays = 0.0;
            bool hasAge = false;
        };

        // A row is ours if it carries the tag we wrote. The content check is
        // an independent cross-check on the tag write, so it matches the
        // four Compose framings EXACTLY rather than loosely — round one used
        // a bare "My " prefix and false-positived on every real memory
        // starting "My studies…", which is what produced its mismatch
        // warnings.
        bool LooksLikeGossipText(const std::string& c)
        {
            return c.rfind("I heard a rumor going round:", 0) == 0 || c.find(" told me this: ") != std::string::npos
                   || c.find(" mentioned this over supper: ") != std::string::npos
                   || (c.rfind("I told ", 0) == 0 && c.find(" this: ") != std::string::npos);
        }

        bool HasGossipTag(const nlohmann::json& row)
        {
            if (const auto it = row.find("tags"); it != row.end() && it->is_array()) {
                for (const auto& t : *it) {
                    // Both tags. `ne_gossip` is what we write now;
                    // `gossip` catches memories written before the rename
                    // AND SkyrimNet's own topical use of the word, which is
                    // what the rename existed to stop colliding with.
                    const auto& v = t.template get_ref<const std::string&>();
                    if (t.is_string() && (v == GossipHarvest::kOwnOutputTag || v == "gossip")) {
                        return true;
                    }
                }
            }
            return false;
        }

        std::vector<Row> Fetch(RE::FormID actorRef,
                               std::string_view query,
                               int depth,
                               double nowGameSeconds,
                               int& mismatches)
        {
            std::vector<Row> out;
            mismatches = 0;
            const auto raw = SkyrimNetAPI::GetMemoriesForActor(actorRef, depth, std::string(query));
            auto j = nlohmann::json::parse(raw, nullptr, false);
            if (j.is_discarded() || !j.is_array()) {
                logger::warn("QUERYPROBE: response was not a JSON array for query '{}'", query);
                return out;
            }
            out.reserve(j.size());
            for (const auto& row : j) {
                if (!row.is_object()) {
                    continue;
                }
                Row r;
                r.id = static_cast<std::int64_t>(row.value("id", 0));
                r.gossip = HasGossipTag(row);
                if (r.gossip != LooksLikeGossipText(JsonUtils::StringOr(row, "content"))) {
                    ++mismatches;
                }
                if (const auto it = row.find("game_time"); it != row.end() && it->is_number()) {
                    r.hasAge = true;
                    r.ageDays = (nowGameSeconds - it->get<double>()) / 86400.0;
                }
                out.push_back(r);
            }
            return out;
        }

        // The actor's real memories in newest-first order, which is the
        // yardstick for "did this query bring back fresh material?".
        //
        // NOT fGossipHarvestWindowDays. The window is 50 days and this save
        // is 23 game days old, so nothing in the database can fail it — an
        // absolute freshness test would report every variant as perfect and
        // settle nothing. What matters is RELATIVE: of the memories this
        // actor actually has, did the query reach the recent end or the old
        // end?
        struct Reference
        {
            std::vector<std::int64_t> newestFirst; // non-gossip ids, newest first
            std::unordered_set<std::int64_t> newestTen;
            int total = 0;
        };

        Reference BuildReference(const std::vector<Row>& corpus)
        {
            Reference ref;
            // The empty query already returns recency-ordered, so position
            // in this vector IS the newness rank. No re-sorting, which also
            // means the rank stays meaningful for rows whose game_time is
            // unusable.
            for (const auto& r : corpus) {
                if (!r.gossip) {
                    ref.newestFirst.push_back(r.id);
                }
            }
            ref.total = static_cast<int>(ref.newestFirst.size());
            for (std::size_t i = 0; i < ref.newestFirst.size() && i < 10; ++i) {
                ref.newestTen.insert(ref.newestFirst[i]);
            }
            return ref;
        }

        struct Stats
        {
            int n = 0;
            int gossip = 0;
            int usable = 0;
            // Median position of the returned rows within the reference
            // ordering, 0-based. 0 means "brought back the newest thing she
            // has"; total-1 means "brought back her oldest".
            double medRank = 0.0;
            double pctRank = 0.0;
            // How many of the ten newest real memories this query reached.
            int newestTenHit = 0;
            double ageMin = 0.0;
            double ageMed = 0.0;
            double ageMax = 0.0;
        };

        Stats Summarize(const std::vector<Row>& rows, const Reference& ref)
        {
            Stats s;
            std::vector<double> ages;
            std::vector<int> ranks;
            for (const auto& r : rows) {
                ++s.n;
                if (r.gossip) {
                    ++s.gossip;
                    continue;
                }
                ++s.usable;
                if (ref.newestTen.contains(r.id)) {
                    ++s.newestTenHit;
                }
                const auto it = std::find(ref.newestFirst.begin(), ref.newestFirst.end(), r.id);
                if (it != ref.newestFirst.end()) {
                    ranks.push_back(static_cast<int>(std::distance(ref.newestFirst.begin(), it)));
                }
                if (r.hasAge) {
                    ages.push_back(r.ageDays);
                }
            }
            if (!ranks.empty()) {
                std::sort(ranks.begin(), ranks.end());
                s.medRank = ranks[ranks.size() / 2];
                s.pctRank = ref.total > 1 ? (s.medRank / static_cast<double>(ref.total - 1)) * 100.0 : 0.0;
            }
            if (!ages.empty()) {
                std::sort(ages.begin(), ages.end());
                s.ageMin = ages.front();
                s.ageMed = ages[ages.size() / 2];
                s.ageMax = ages.back();
            }
            return s;
        }

        // Rows present in either set, counted once. Answers whether running
        // BOTH calls per actor beats either alone: the empty query supplies
        // the newest real memories, the semantic one supplies the rest.
        Stats Union(const std::vector<Row>& a, const std::vector<Row>& b, const Reference& ref)
        {
            std::vector<Row> merged = a;
            std::unordered_set<std::int64_t> seen;
            for (const auto& r : a) {
                seen.insert(r.id);
            }
            for (const auto& r : b) {
                if (seen.insert(r.id).second) {
                    merged.push_back(r);
                }
            }
            return Summarize(merged, ref);
        }

        void Run(const GossipThread::Token&)
        {
            if (!SkyrimNetAPI::IsMemorySystemReady()) {
                logger::warn("QUERYPROBE: memory system not ready; probe skipped");
                return;
            }
            const auto ref = GossipGraph::ActorRefFor(kSubjectNpc);
            if (!ref) {
                logger::warn("QUERYPROBE: no placed reference for 0x{:08X}; is the graph built?", kSubjectNpc);
                return;
            }

            const double nowGameSeconds = EventLogUtil::NowGameTimeSeconds();

            int mm = 0;
            int totalMismatches = 0;
            const auto corpus = Fetch(ref, "", kReference, nowGameSeconds, mm);
            totalMismatches += mm;
            const auto yard = BuildReference(corpus);
            if (yard.total == 0) {
                logger::warn("QUERYPROBE: reference pull found no non-gossip memories; nothing to measure");
                return;
            }
            const auto controlDeep = Fetch(ref, "", kDeep, nowGameSeconds, mm);
            totalMismatches += mm;

            logger::info("QUERYPROBE: ==========================================================");
            logger::info("QUERYPROBE: subject 0x{:08X} (ref 0x{:08X}), now={:.2f} game days",
                         kSubjectNpc,
                         ref,
                         nowGameSeconds / 86400.0);
            logger::info("QUERYPROBE: reference pull: {} rows, {} of them non-gossip -- these are the yardstick, "
                         "in SkyrimNet's own recency order",
                         corpus.size(),
                         yard.total);
            logger::info("QUERYPROBE: rank = median position of returned rows in that newest-first list "
                         "(0 = her newest, {} = her oldest). LOWER IS FRESHER.",
                         yard.total - 1);
            logger::info("QUERYPROBE: new10 = how many of her 10 NEWEST real memories the query reached, out of 10");
            logger::info("QUERYPROBE: {:<22} | {:>2} {:>2} | {:>2} {:>2} | {:>5} {:>5} {:>5} | {:>6} {:>6} {:>6} "
                         "| {:>2} {:>5}",
                         "variant",
                         "g10",
                         "u10",
                         "g50",
                         "u50",
                         "rank",
                         "pct",
                         "new10",
                         "ageMin",
                         "ageMed",
                         "ageMax",
                         "uU",
                         "uNew10");

            for (const auto& v : kVariants) {
                const auto shallow = Fetch(ref, v.query, kShallow, nowGameSeconds, mm);
                totalMismatches += mm;
                const auto deep = Fetch(ref, v.query, kDeep, nowGameSeconds, mm);
                totalMismatches += mm;

                const auto s = Summarize(shallow, yard);
                const auto d = Summarize(deep, yard);
                const auto u = Union(controlDeep, deep, yard);

                logger::info("QUERYPROBE: {:<22} | {:>2} {:>2} | {:>2} {:>2} | {:>5.0f} {:>4.0f}% {:>5} | "
                             "{:>6.1f} {:>6.1f} {:>6.1f} | {:>2} {:>5}   \"{}\"",
                             v.label,
                             s.gossip,
                             s.usable,
                             d.gossip,
                             d.usable,
                             d.medRank,
                             d.pctRank,
                             d.newestTenHit,
                             d.ageMin,
                             d.ageMed,
                             d.ageMax,
                             u.usable,
                             u.newestTenHit,
                             v.query);
            }

            logger::info("QUERYPROBE: ==========================================================");
            logger::info("QUERYPROBE: want u50 HIGH, rank LOW, new10 HIGH. control-empty is the freshness "
                         "ceiling by construction (rank 0-ish, new10 10) -- the question is which query gets "
                         "closest to it while keeping g50 low.");
            if (totalMismatches > 0) {
                logger::warn("QUERYPROBE: {} row(s) across the whole run where the gossip TAG and the "
                             "content SHAPE disagreed. Round one's version of this check was too loose; "
                             "if this is still non-zero the tag write needs looking at.",
                             totalMismatches);
            } else {
                logger::info("QUERYPROBE: tag and content agreed on every row -- the [\"gossip\"] tag write "
                             "is landing, so the harvest filter can be trusted.");
            }
        }
    } // namespace

    void OnSessionStart()
    {
        std::scoped_lock lock(g_mutex);
        g_armed = true;
        g_fired = false;
        g_elapsed = 0.0;
    }

    void Poll(const PluginThread::Token&, double unpausedElapsedSeconds)
    {
        {
            std::scoped_lock lock(g_mutex);
            if (!g_armed || g_fired) {
                return;
            }
            g_elapsed += unpausedElapsedSeconds;
            if (g_elapsed < kDelaySeconds) {
                return;
            }
            g_fired = true;
        }
        if (!Settings::Get().gossipEnabled) {
            return;
        }
        GossipDispatch::EnqueueWork([](const GossipThread::Token& gt) { Run(gt); });
    }
} // namespace NarrativeEngine::GossipQueryProbe
