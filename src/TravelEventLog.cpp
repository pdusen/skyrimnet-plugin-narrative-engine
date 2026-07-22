#include <TravelEventLog.h>

#include <EventLogUtil.h>
#include <logger.h>
#include <MainThread.h>
#include <PhaseTracker.h>
#include <Region.h>
#include <Settings.h>
#include <SkyrimNetEvents.h>

#include <nlohmann/json.hpp>

#include <RE/Skyrim.h>

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace NarrativeEngine::TravelEventLog
{
    namespace
    {
        constexpr std::uint32_t kRecordVersion = 1;
        constexpr std::size_t kDefaultMaxStored = 128;

        enum class Kind : std::uint8_t
        {
            EnteredLocation = 0,
            LeftLocation = 1,
            CrossedHolds = 2,
            EnteredWilderness = 3,
            FastTravelArrived = 4,
        };

        const char* KindSlug(Kind k)
        {
            switch (k) {
            case Kind::EnteredLocation:
                return "entered_location";
            case Kind::LeftLocation:
                return "left_location";
            case Kind::CrossedHolds:
                return "crossed_holds";
            case Kind::EnteredWilderness:
                return "entered_wilderness";
            case Kind::FastTravelArrived:
                return "fast_travel_arrived";
            }
            return "unknown";
        }

        struct TravelSnapshot
        {
            RE::FormID currentLocationID = 0;
            std::string currentLocationName;
            bool interior = false;
            RE::FormID holdFormID = 0;
            std::string holdName;
            Region::Climate climate = Region::Climate::Unknown;

            // Party names captured at the same instant as the
            // location/hold data — bundled into the snapshot so a
            // single main-thread hop covers everything the Poll
            // body needs. Player first, then followers sorted by
            // display name.
            std::vector<std::string> partyNames;
        };

        struct InternalEvent
        {
            Kind kind = Kind::EnteredLocation;
            double localTime = 0.0;
            double gameTime = 0.0;

            RE::FormID fromLocationID = 0;
            std::string fromLocationName;
            bool fromInterior = false;
            RE::FormID fromHoldRegionID = 0;
            std::string fromHoldName;

            RE::FormID toLocationID = 0;
            std::string toLocationName;
            bool toInterior = false;
            RE::FormID toHoldRegionID = 0;
            std::string toHoldName;

            std::vector<std::string> partyNames; // player first, then followers
        };

        std::mutex g_mutex;
        std::vector<InternalEvent> g_events;

        // Session-only pending queue drained by EventHistoryWriter.
        // Each entry stashes the emit-time in-game timestamp alongside
        // a raw copy of the InternalEvent; DrainHistoryTail runs the
        // rendering helpers (declared later in this TU) at drain time
        // to build the final HistoryEntry, which avoids hoisting the
        // whole rendering block above Poll.
        struct PendingHistoryItem
        {
            InternalEvent event;
            std::string inGameTimestamp;
        };
        std::vector<PendingHistoryItem> g_pendingHistory;

        TravelSnapshot g_lastSnapshot;
        bool g_baselineInitialized = false;

        // Fast-travel flag: the TESFastTravelEndEvent sink sets this to
        // true when a fast-travel or carriage arrival lands. The next
        // Poll to observe a location / hold transition consults the
        // flag to upgrade what would be entered_location / crossed_holds
        // to fast_travel_arrived, then clears it.
        //
        // The 5-second wall-clock window guards against a stale flag
        // mislabelling an unrelated later transition — if the poll
        // hasn't consumed it within 5s of the sink firing, drop it.
        // Wall-clock here (not the Tick-driven accumulator) is
        // deliberate: fast-travel completes with the game unpaused
        // basically immediately, so wall-clock and unpaused time are
        // equivalent for this narrow window.
        bool g_fastTravelPending = false;
        double g_fastTravelSampledAt = 0.0;
        constexpr double kFastTravelFlagWindowSeconds = 5.0;

        // Sink registration tracking so Initialize is idempotent.
        bool g_sinksRegistered = false;

        std::size_t MaxStored()
        {
            const int v = Settings::Get().travelEventsMaxStored;
            if (v <= 0)
                return kDefaultMaxStored;
            return static_cast<std::size_t>(v);
        }

        float FollowerRadiusUnits()
        {
            const int v = Settings::Get().travelFollowerRadiusUnits;
            return v <= 0 ? 4000.0f : static_cast<float>(v);
        }

        void PushLocked(InternalEvent evt)
        {
            // Feed the history-writer's pending queue first — capture
            // the in-game timestamp NOW so the log carries the moment
            // the event actually happened, not the moment the writer
            // gets around to flushing it. Body rendering runs at
            // drain time. Gated on the master switch so the queue
            // can't grow unbounded on long sessions with the writer
            // off.
            if (Settings::Get().eventHistoryEnabled) {
                PendingHistoryItem item;
                item.event = evt;
                item.inGameTimestamp = EventLogUtil::CurrentInGameTimestamp();
                g_pendingHistory.push_back(std::move(item));
            }
            g_events.push_back(std::move(evt));
            while (g_events.size() > MaxStored()) {
                g_events.erase(g_events.begin());
            }
        }

        std::string ActorDisplayName(RE::Actor* actor)
        {
            if (!actor)
                return {};
            const char* n = actor->GetDisplayFullName();
            if (!n || !*n)
                return {};
            return std::string(n);
        }

        // Main-thread helper — collect player display name + nearby
        // alive followers (IsPlayerTeammate + within radius), sorted
        // for stability. Called from BuildFreshSnapshot inside the
        // MainThread::Run lambda, so it's always on main.
        std::vector<std::string> CollectPartyOnMain(RE::PlayerCharacter* pc, float radius)
        {
            std::vector<std::string> out;
            std::string playerName = ActorDisplayName(pc);
            if (playerName.empty()) {
                playerName = "The Player";
            }
            out.push_back(std::move(playerName));

            if (!pc) {
                return out;
            }
            const RE::NiPoint3 playerPos = pc->GetPosition();

            std::vector<std::string> followers;
            auto* pl = RE::ProcessLists::GetSingleton();
            if (pl) {
                pl->ForEachHighActor([&](RE::Actor* actor) {
                    if (!actor)
                        return RE::BSContainer::ForEachResult::kContinue;
                    if (actor == pc)
                        return RE::BSContainer::ForEachResult::kContinue;
                    if (actor->IsDead())
                        return RE::BSContainer::ForEachResult::kContinue;
                    if (!actor->IsPlayerTeammate())
                        return RE::BSContainer::ForEachResult::kContinue;
                    const float dist = actor->GetPosition().GetDistance(playerPos);
                    if (dist > radius)
                        return RE::BSContainer::ForEachResult::kContinue;
                    std::string n = ActorDisplayName(actor);
                    if (n.empty())
                        return RE::BSContainer::ForEachResult::kContinue;
                    followers.push_back(std::move(n));
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
            std::sort(followers.begin(), followers.end());
            for (auto& f : followers)
                out.push_back(std::move(f));
            return out;
        }

        // Fresh snapshot from live world state — location + hold +
        // party, bundled together so a single main-thread hop covers
        // everything the Poll body needs. Called inside a
        // MainThread::Run lambda from Poll, and inline (main-thread)
        // from OnPostLoadGame.
        //
        // Hold-region tracking is suppressed on interior cells (they
        // often lack region data); callers merge with g_lastSnapshot's
        // hold fields to preserve last-known-hold across an interior
        // visit.
        TravelSnapshot BuildFreshSnapshot()
        {
            TravelSnapshot s;
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (!pc)
                return s;

            auto* cell = pc->GetParentCell();
            s.interior = cell && cell->IsInteriorCell();

            if (auto* loc = pc->GetCurrentLocation()) {
                s.currentLocationID = loc->GetFormID();
                if (const char* n = loc->GetFullName(); n && *n) {
                    s.currentLocationName = n;
                }
            }

            // Query hold via the BGSLocation parent-chain walk. This
            // works uniformly for exterior wilderness, cities, and
            // building / dungeon interiors — vanilla assigns
            // hold-chained locations to essentially everything within
            // a hold's territory. Genuinely unclassified locations
            // (some scripted dungeons) return holdFormID=0; the Poll
            // call site preserves the last-known hold in that case
            // so an excursion into an unclassified interior doesn't
            // spuriously trip crossed_holds.
            auto res = Region::ForPlayer();
            s.holdFormID = res.holdFormID;
            s.holdName = res.holdDisplayName;
            s.climate = res.climate;

            s.partyNames = CollectPartyOnMain(pc, FollowerRadiusUnits());

            return s;
        }

        // TESFastTravelEndEvent sink. Fires on the SKSE thread when
        // fast-travel or carriage travel completes. Just sets the flag;
        // the next Poll consumes it and upgrades the observed
        // transition kind to FastTravelArrived.
        struct FastTravelEndSink : public RE::BSTEventSink<RE::TESFastTravelEndEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(const RE::TESFastTravelEndEvent* /*evt*/,
                                                  RE::BSTEventSource<RE::TESFastTravelEndEvent>* /*src*/) override
            {
                const double now = EventLogUtil::NowUnixSeconds();
                {
                    std::scoped_lock lock(g_mutex);
                    g_fastTravelPending = true;
                    g_fastTravelSampledAt = now;
                }
                if (Settings::Get().debugMode) {
                    logger::debug("TravelEventLog: TESFastTravelEndEvent fired; flag set");
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        FastTravelEndSink* GetFastTravelEndSink()
        {
            static FastTravelEndSink s;
            return &s;
        }

        // Called under the mutex from Poll before emitting an
        // entered_location / crossed_holds. If the fast-travel flag is
        // set and still fresh, returns true and clears the flag —
        // caller re-tags the event as FastTravelArrived. Returns false
        // otherwise.
        bool ConsumeFastTravelFlagLocked()
        {
            if (!g_fastTravelPending)
                return false;
            const double now = EventLogUtil::NowUnixSeconds();
            const bool fresh = (now - g_fastTravelSampledAt) < kFastTravelFlagWindowSeconds;
            if (!fresh) {
                logger::warn("TravelEventLog: fast-travel flag expired without being consumed "
                             "({:.1f}s since sink fired); dropping",
                             now - g_fastTravelSampledAt);
                g_fastTravelPending = false;
                return false;
            }
            g_fastTravelPending = false;
            return true;
        }

        // Build an InternalEvent's from/to endpoint payload from the
        // two snapshots and stamp party + timestamps. Kind-agnostic —
        // caller sets `kind` after.
        //
        // Party names are taken from the `to` snapshot (captured in
        // the same main-thread hop as the location/hold data), not
        // re-collected here — so this function is plugin-thread safe.
        InternalEvent MakeEvent(const TravelSnapshot& from, const TravelSnapshot& to)
        {
            InternalEvent e;
            e.localTime = EventLogUtil::NowUnixSeconds();
            e.gameTime = EventLogUtil::NowGameTimeSeconds();

            e.fromLocationID = from.currentLocationID;
            e.fromLocationName = from.currentLocationName;
            e.fromInterior = from.interior;
            e.fromHoldRegionID = from.holdFormID;
            e.fromHoldName = from.holdName;

            e.toLocationID = to.currentLocationID;
            e.toLocationName = to.currentLocationName;
            e.toInterior = to.interior;
            e.toHoldRegionID = to.holdFormID;
            e.toHoldName = to.holdName;

            e.partyNames = to.partyNames;
            return e;
        }
    } // namespace

    void Initialize()
    {
        if (!g_sinksRegistered) {
            auto* src = RE::ScriptEventSourceHolder::GetSingleton();
            if (src) {
                src->AddEventSink<RE::TESFastTravelEndEvent>(GetFastTravelEndSink());
                g_sinksRegistered = true;
            } else {
                logger::error("TravelEventLog: ScriptEventSourceHolder unavailable; fast-travel sink not registered");
            }
        }
        logger::info("TravelEventLog: initialized (follower radius={:.0f} units, fast-travel sink={})",
                     static_cast<double>(FollowerRadiusUnits()),
                     g_sinksRegistered);
    }

    void Shutdown()
    {
        if (!g_sinksRegistered)
            return;
        if (auto* src = RE::ScriptEventSourceHolder::GetSingleton()) {
            src->RemoveEventSink<RE::TESFastTravelEndEvent>(GetFastTravelEndSink());
        }
        g_sinksRegistered = false;
    }

    void OnPostLoadGame()
    {
        std::scoped_lock lock(g_mutex);
        g_lastSnapshot = BuildFreshSnapshot();
        g_baselineInitialized = true;
        logger::info("TravelEventLog: OnPostLoadGame: baseline seeded (locFormID=0x{:08X} loc='{}' interior={} "
                     "hold='{}')",
                     g_lastSnapshot.currentLocationID,
                     g_lastSnapshot.currentLocationName,
                     g_lastSnapshot.interior,
                     g_lastSnapshot.holdName);
    }

    void OnPhaseAdvanced()
    {
        const double cutoff = PhaseTracker::PhaseEnteredAtRealTime();
        std::scoped_lock lock(g_mutex);
        if (cutoff <= 0.0) {
            g_events.clear();
            return;
        }
        std::erase_if(g_events, [cutoff](const InternalEvent& e) { return e.localTime < cutoff; });
    }

    void Poll(const PluginThread::Token& pt, double /*unpausedElapsedSeconds*/)
    {
        // Hop to main for the location/hold/party snapshot.
        // Deliberately outside the mutex — MainThread::Run blocks the
        // plugin thread until the main-thread lambda returns, so
        // holding g_mutex across it would prevent GetRenderedTail
        // (main-thread) from making progress. The diff + emit runs
        // under the mutex below.
        TravelSnapshot fresh = MainThread::Run(pt, [](const MainThread::Token&) { return BuildFreshSnapshot(); });

        std::scoped_lock lock(g_mutex);
        const bool debug = Settings::Get().debugMode;

        // Preserve the last-known hold when the current location
        // doesn't resolve to one (unclassified dungeons, scripted
        // interiors). Keeps hold-tracking stable across excursions —
        // the moment the player exits back into properly-classified
        // territory, the fresh Region query takes over. Doesn't kick
        // in when the fresh query DOES resolve, so genuine hold
        // crossings still fire.
        if (fresh.holdFormID == 0 && g_lastSnapshot.holdFormID != 0) {
            fresh.holdFormID = g_lastSnapshot.holdFormID;
            fresh.holdName = g_lastSnapshot.holdName;
            fresh.climate = g_lastSnapshot.climate;
        }

        if (!g_baselineInitialized) {
            g_lastSnapshot = fresh;
            g_baselineInitialized = true;
            if (debug) {
                logger::debug("TravelEventLog: Poll: baseline seeded lazily (loc='{}' interior={} hold='{}')",
                              fresh.currentLocationName,
                              fresh.interior,
                              fresh.holdName);
            }
            return;
        }

        // Interior-to-interior: skip emission entirely, but advance
        // baseline so we don't accumulate stale state.
        if (g_lastSnapshot.interior && fresh.interior) {
            g_lastSnapshot = fresh;
            return;
        }

        const bool locationChanged = (fresh.currentLocationID != g_lastSnapshot.currentLocationID);
        const bool holdChanged = (fresh.holdFormID != g_lastSnapshot.holdFormID);
        const bool interiorChanged = (fresh.interior != g_lastSnapshot.interior);

        if (!locationChanged && !holdChanged && !interiorChanged) {
            // Cell moved but nothing narratively significant changed.
            return;
        }

        const bool eitherInterior = g_lastSnapshot.interior || fresh.interior;

        // Determine whether this transition should be tagged as a
        // fast-travel arrival — check once and consume the flag. Only
        // meaningful when the transition also carries an "arrived
        // somewhere" signal (new location or new hold); a pure
        // left_location without a corresponding entered_location
        // shouldn't consume it.
        const bool arrivedSomewhere =
            (locationChanged && fresh.currentLocationID != 0 && !fresh.currentLocationName.empty())
            || (holdChanged && !eitherInterior && fresh.holdFormID != 0);
        const bool fastTravelArrival = arrivedSomewhere && ConsumeFastTravelFlagLocked();

        // If this was a fast-travel arrival, emit a single dedicated
        // FastTravelArrived event and skip the standard entered /
        // crossed-holds emissions (they'd be redundant with the
        // fast-travel event's from/to endpoints).
        if (fastTravelArrival) {
            InternalEvent e = MakeEvent(g_lastSnapshot, fresh);
            e.kind = Kind::FastTravelArrived;
            logger::info("TravelEventLog: emit fast_travel_arrived ('{}' -> '{}', hold '{}' -> '{}')",
                         e.fromLocationName,
                         e.toLocationName,
                         e.fromHoldName,
                         e.toHoldName);
            PushLocked(std::move(e));
            g_lastSnapshot = fresh;
            return;
        }

        // Standard location-chain events fire on any location change.
        // Interior<->exterior transitions fire only these (not hold
        // events).
        if (locationChanged) {
            // Left the old (if the old had a name — walking out of
            // wilderness has no "left X" to report).
            if (g_lastSnapshot.currentLocationID != 0 && !g_lastSnapshot.currentLocationName.empty()) {
                InternalEvent e = MakeEvent(g_lastSnapshot, fresh);
                e.kind = Kind::LeftLocation;
                if (debug) {
                    logger::debug(
                        "TravelEventLog: emit left_location ('{}' -> '{}')", e.fromLocationName, e.toLocationName);
                }
                PushLocked(std::move(e));
            }
            // Entered the new (if the new has a name).
            if (fresh.currentLocationID != 0 && !fresh.currentLocationName.empty()) {
                InternalEvent e = MakeEvent(g_lastSnapshot, fresh);
                e.kind = Kind::EnteredLocation;
                if (debug) {
                    logger::debug(
                        "TravelEventLog: emit entered_location ('{}' -> '{}')", e.fromLocationName, e.toLocationName);
                }
                PushLocked(std::move(e));
            }
        }

        // Hold-region + wilderness events are exterior-to-exterior only.
        if (!eitherInterior) {
            if (holdChanged && g_lastSnapshot.holdFormID != 0 && fresh.holdFormID != 0) {
                InternalEvent e = MakeEvent(g_lastSnapshot, fresh);
                e.kind = Kind::CrossedHolds;
                if (debug) {
                    logger::debug("TravelEventLog: emit crossed_holds ('{}' -> '{}')", e.fromHoldName, e.toHoldName);
                }
                PushLocked(std::move(e));
            }
            // Entered wilderness = location went null AND hold did not
            // change. (Location null + hold change is already covered
            // by left_location + crossed_holds above.)
            if (fresh.currentLocationID == 0 && g_lastSnapshot.currentLocationID != 0 && !holdChanged) {
                InternalEvent e = MakeEvent(g_lastSnapshot, fresh);
                e.kind = Kind::EnteredWilderness;
                if (debug) {
                    logger::debug("TravelEventLog: emit entered_wilderness (hold='{}')", e.fromHoldName);
                }
                PushLocked(std::move(e));
            }
        }

        g_lastSnapshot = fresh;
    }

    namespace
    {
        // "Varian" / "Varian and Jenassa" / "Varian, Jenassa, and Marcurio"
        std::string RenderParty(const std::vector<std::string>& names)
        {
            if (names.empty())
                return "The Player";
            if (names.size() == 1)
                return names[0];
            if (names.size() == 2)
                return names[0] + " and " + names[1];
            std::string out;
            for (std::size_t i = 0; i + 1 < names.size(); ++i) {
                out += names[i];
                out += ", ";
            }
            out += "and ";
            out += names.back();
            return out;
        }

        // "is" for singular party, "are" for two-plus. Only used in the
        // entered_wilderness sentence.
        const char* PartyVerb(const std::vector<std::string>& names)
        {
            return names.size() >= 2 ? "are" : "is";
        }

        // Ensure a hold display name ends with " Hold". Idempotent;
        // handles the case where vanilla data already includes "Hold"
        // in the location's FullName vs. cases where it doesn't. The
        // suffix disambiguates cities from their holds — "Whiterun"
        // the city vs. "Whiterun Hold" the region.
        std::string FormatHoldName(const std::string& name)
        {
            if (name.empty()) {
                return name;
            }
            static constexpr std::string_view suffix = " Hold";
            if (name.size() >= suffix.size() && std::string_view(name).ends_with(suffix)) {
                return name;
            }
            return name + " Hold";
        }

        // Best-of rendering for a location/hold pair — prefer the
        // named location, fall back to the hold (with " Hold" suffix
        // applied so consumers can always distinguish city from hold).
        // Empty when neither is populated.
        std::string BestName(const std::string& locName, const std::string& holdName)
        {
            if (!locName.empty()) {
                return locName;
            }
            return FormatHoldName(holdName);
        }

        // Standalone (single-event) sentence rendering, minus the
        // "[N ago]" prefix.
        //
        // Verb choice distinguishes exterior (arrived at / departed
        // from) from interior (entered / exited / visited) so the
        // reader can tell a Lakeview Manor exterior arrival from
        // walking through its front door.
        std::string RenderEventBody(const InternalEvent& e)
        {
            const std::string party = RenderParty(e.partyNames);
            switch (e.kind) {
            case Kind::EnteredLocation:
                return party + (e.toInterior ? " entered " : " arrived at ") + e.toLocationName + ".";
            case Kind::LeftLocation:
                return party + (e.fromInterior ? " exited " : " departed from ") + e.fromLocationName + ".";
            case Kind::CrossedHolds:
                return party + " crossed from " + FormatHoldName(e.fromHoldName) + " into "
                       + FormatHoldName(e.toHoldName) + ".";
            case Kind::EnteredWilderness: {
                // Player was in a named location, now in wilderness
                // within the same hold. Mention the location they
                // departed from (not the hold — they haven't left it).
                // Fall back to the hold if the location name is empty
                // for some edge case.
                const std::string& origin =
                    !e.fromLocationName.empty() ? e.fromLocationName : FormatHoldName(e.fromHoldName);
                const char* verb = (e.fromInterior && !e.fromLocationName.empty()) ? "exited" : "departed from";
                return party + " " + verb + " " + origin + " and " + PartyVerb(e.partyNames)
                       + " now in the wilderness of " + FormatHoldName(e.fromHoldName) + ".";
            }
            case Kind::FastTravelArrived: {
                const std::string arriveName = BestName(e.toLocationName, e.toHoldName);
                const std::string originName = BestName(e.fromLocationName, e.fromHoldName);
                const char* arriveVerb = e.toInterior ? "entered " : "arrived at ";
                std::string out = party + " " + arriveVerb + arriveName;
                if (!originName.empty()) {
                    out += ", having journeyed from " + originName;
                }
                out += ".";
                return out;
            }
            }
            return party + " travelled somewhere.";
        }

        // "travelled from X to Y" summary for non-net-zero runs. Uses
        // the run's front `from` and back `to` endpoints.
        std::string RenderJourneySummary(const std::vector<InternalEvent>& run)
        {
            const auto& first = run.front();
            const auto& last = run.back();
            const std::string party = RenderParty(last.partyNames);
            const std::string origin = BestName(first.fromLocationName, first.fromHoldName);
            const std::string arrival = BestName(last.toLocationName, last.toHoldName);
            if (origin.empty() && arrival.empty()) {
                return party + " travelled a while.";
            }
            if (origin.empty()) {
                return party + " ended up in " + arrival + ".";
            }
            if (arrival.empty()) {
                return party + " set out from " + origin + ".";
            }
            return party + " travelled from " + origin + " to " + arrival + ".";
        }

        // "visited X" summary for net-zero runs that touched one or
        // more interiors. Collects distinct interior names from the
        // run (events where toInterior == true).
        std::string RenderVisitSummary(const std::vector<InternalEvent>& run)
        {
            std::vector<std::string> visited;
            for (const auto& e : run) {
                if (!e.toInterior || e.toLocationName.empty())
                    continue;
                if (std::find(visited.begin(), visited.end(), e.toLocationName) == visited.end()) {
                    visited.push_back(e.toLocationName);
                }
            }
            const std::string party = RenderParty(run.back().partyNames);
            if (visited.empty()) {
                // Shouldn't reach here — caller only invokes on runs
                // where visitedInteriors is non-empty. Defensive.
                return party + " looked around.";
            }
            if (visited.size() == 1) {
                return party + " visited " + visited[0] + ".";
            }
            if (visited.size() == 2) {
                return party + " visited " + visited[0] + " and " + visited[1] + ".";
            }
            std::string out = party + " visited ";
            for (std::size_t i = 0; i + 1 < visited.size(); ++i) {
                out += visited[i];
                out += ", ";
            }
            out += "and ";
            out += visited.back();
            out += ".";
            return out;
        }

        // Retrieve the "netted zero" endpoint key for either side of
        // an event — location FormID if non-zero, else hold FormID.
        // The condensation "did we return to where we started" test
        // compares these across the first event's `from` and the last
        // event's `to`.
        RE::FormID EndpointKeyFrom(const InternalEvent& e)
        {
            return e.fromLocationID != 0 ? e.fromLocationID : e.fromHoldRegionID;
        }
        RE::FormID EndpointKeyTo(const InternalEvent& e)
        {
            return e.toLocationID != 0 ? e.toLocationID : e.toHoldRegionID;
        }

        // Convert an InternalEvent to a rendered JSON object per the
        // SkyrimNet-event shape.
        nlohmann::json ToJsonEntry(const std::string& slug,
                                   const std::string& body,
                                   double localTime,
                                   double gameTime,
                                   const std::vector<std::string>& partyNames,
                                   double currentGameTimeSeconds)
        {
            const double delta = currentGameTimeSeconds - gameTime;
            const std::string prefix = "[" + SkyrimNetEvents::FormatRelativeGameTime(delta < 0.0 ? 0.0 : delta) + "] ";

            nlohmann::json j;
            j["type"] = "travel_event";
            j["ne_kind"] = slug;
            j["localTime"] = localTime;
            j["gameTime"] = gameTime;
            j["originatingActorName"] = partyNames.empty() ? std::string{} : partyNames.front();
            j["targetActorName"] = std::string{};
            j["text"] = prefix + body;
            return j;
        }
    } // namespace

    nlohmann::json GetRenderedTail(double currentGameTimeSeconds)
    {
        std::vector<InternalEvent> snapshot;
        {
            std::scoped_lock lock(g_mutex);
            snapshot = g_events;
        }
        std::sort(snapshot.begin(), snapshot.end(), [](const InternalEvent& a, const InternalEvent& b) {
            return a.localTime < b.localTime;
        });

        const double windowSec = static_cast<double>(std::max(0, Settings::Get().travelCondensationWindowSeconds));

        nlohmann::json out = nlohmann::json::array();

        // Walk events and greedily group consecutive events into runs
        // separated by <= windowSec. FastTravelArrived breaks a run.
        std::size_t i = 0;
        while (i < snapshot.size()) {
            // Fast-travel events always stand alone — never absorbed
            // into a condensable run.
            if (snapshot[i].kind == Kind::FastTravelArrived) {
                const auto& e = snapshot[i];
                out.push_back(ToJsonEntry("fast_travel_arrived",
                                          RenderEventBody(e),
                                          e.localTime,
                                          e.gameTime,
                                          e.partyNames,
                                          currentGameTimeSeconds));
                ++i;
                continue;
            }
            // Grow the run as long as the next event is within the
            // window AND is not FastTravelArrived.
            std::size_t j = i + 1;
            while (j < snapshot.size() && snapshot[j].kind != Kind::FastTravelArrived
                   && (snapshot[j].localTime - snapshot[j - 1].localTime) <= windowSec) {
                ++j;
            }

            const std::size_t runLen = j - i;
            if (runLen == 1) {
                // Solo event — render as-is.
                const auto& e = snapshot[i];
                out.push_back(ToJsonEntry(KindSlug(e.kind),
                                          RenderEventBody(e),
                                          e.localTime,
                                          e.gameTime,
                                          e.partyNames,
                                          currentGameTimeSeconds));
            } else {
                // Run of 2+ — apply condensation.
                std::vector<InternalEvent> run(snapshot.begin() + i, snapshot.begin() + j);
                const RE::FormID startKey = EndpointKeyFrom(run.front());
                const RE::FormID endKey = EndpointKeyTo(run.back());
                const bool netZero = (startKey == endKey);

                bool touchedInterior = false;
                for (const auto& e : run) {
                    if (e.toInterior && !e.toLocationName.empty()) {
                        touchedInterior = true;
                        break;
                    }
                }

                if (netZero && !touchedInterior) {
                    // Pure exterior back-and-forth — noise; drop the
                    // whole run.
                } else if (netZero && touchedInterior) {
                    const auto& last = run.back();
                    out.push_back(ToJsonEntry("travel_summary",
                                              RenderVisitSummary(run),
                                              last.localTime,
                                              last.gameTime,
                                              last.partyNames,
                                              currentGameTimeSeconds));
                } else {
                    const auto& last = run.back();
                    out.push_back(ToJsonEntry("travel_summary",
                                              RenderJourneySummary(run),
                                              last.localTime,
                                              last.gameTime,
                                              last.partyNames,
                                              currentGameTimeSeconds));
                }
            }

            i = j;
        }

        return out;
    }

    void OnSave(SKSE::SerializationInterface* intfc)
    {
        if (!intfc)
            return;
        OnPhaseAdvanced();

        std::vector<InternalEvent> snapshot;
        {
            std::scoped_lock lock(g_mutex);
            snapshot = g_events;
        }

        if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
            logger::error("TravelEventLog::OnSave: OpenRecord failed");
            return;
        }
        const auto count = static_cast<std::uint32_t>(snapshot.size());
        intfc->WriteRecordData(count);
        for (const auto& e : snapshot) {
            const std::uint8_t kindByte = static_cast<std::uint8_t>(e.kind);
            const std::uint8_t fromInt = e.fromInterior ? 1 : 0;
            const std::uint8_t toInt = e.toInterior ? 1 : 0;
            intfc->WriteRecordData(kindByte);
            intfc->WriteRecordData(e.localTime);
            intfc->WriteRecordData(e.gameTime);
            intfc->WriteRecordData(e.fromLocationID);
            EventLogUtil::WriteString(intfc, e.fromLocationName);
            intfc->WriteRecordData(fromInt);
            intfc->WriteRecordData(e.fromHoldRegionID);
            EventLogUtil::WriteString(intfc, e.fromHoldName);
            intfc->WriteRecordData(e.toLocationID);
            EventLogUtil::WriteString(intfc, e.toLocationName);
            intfc->WriteRecordData(toInt);
            intfc->WriteRecordData(e.toHoldRegionID);
            EventLogUtil::WriteString(intfc, e.toHoldName);
            const std::uint16_t partyCount = static_cast<std::uint16_t>(e.partyNames.size());
            intfc->WriteRecordData(partyCount);
            for (const auto& n : e.partyNames) {
                EventLogUtil::WriteString(intfc, n);
            }
        }
    }

    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length)
    {
        if (!intfc)
            return;
        if (version != kRecordVersion) {
            logger::warn("TravelEventLog::OnLoad: unknown version {} (length={}); clearing", version, length);
            OnRevert();
            return;
        }
        std::uint32_t count = 0;
        if (intfc->ReadRecordData(count) != sizeof(count)) {
            logger::error("TravelEventLog::OnLoad: failed to read count");
            OnRevert();
            return;
        }
        std::vector<InternalEvent> loaded;
        loaded.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            InternalEvent e;
            std::uint8_t kindByte = 0;
            std::uint8_t fromInt = 0;
            std::uint8_t toInt = 0;
            std::uint16_t partyCount = 0;
            if (intfc->ReadRecordData(kindByte) != sizeof(kindByte)
                || intfc->ReadRecordData(e.localTime) != sizeof(e.localTime)
                || intfc->ReadRecordData(e.gameTime) != sizeof(e.gameTime)
                || intfc->ReadRecordData(e.fromLocationID) != sizeof(e.fromLocationID)) {
                logger::error("TravelEventLog::OnLoad: short read (header) record {}/{}", i, count);
                OnRevert();
                return;
            }
            if (!EventLogUtil::ReadString(intfc, e.fromLocationName)) {
                OnRevert();
                return;
            }
            if (intfc->ReadRecordData(fromInt) != sizeof(fromInt)
                || intfc->ReadRecordData(e.fromHoldRegionID) != sizeof(e.fromHoldRegionID)) {
                OnRevert();
                return;
            }
            if (!EventLogUtil::ReadString(intfc, e.fromHoldName)) {
                OnRevert();
                return;
            }
            if (intfc->ReadRecordData(e.toLocationID) != sizeof(e.toLocationID)) {
                OnRevert();
                return;
            }
            if (!EventLogUtil::ReadString(intfc, e.toLocationName)) {
                OnRevert();
                return;
            }
            if (intfc->ReadRecordData(toInt) != sizeof(toInt)
                || intfc->ReadRecordData(e.toHoldRegionID) != sizeof(e.toHoldRegionID)) {
                OnRevert();
                return;
            }
            if (!EventLogUtil::ReadString(intfc, e.toHoldName)) {
                OnRevert();
                return;
            }
            if (intfc->ReadRecordData(partyCount) != sizeof(partyCount)) {
                OnRevert();
                return;
            }
            e.partyNames.reserve(partyCount);
            for (std::uint16_t p = 0; p < partyCount; ++p) {
                std::string n;
                if (!EventLogUtil::ReadString(intfc, n)) {
                    OnRevert();
                    return;
                }
                e.partyNames.push_back(std::move(n));
            }
            if (kindByte > static_cast<std::uint8_t>(Kind::FastTravelArrived)) {
                logger::warn("TravelEventLog::OnLoad: record {} invalid kind {}, skipping", i, kindByte);
                continue;
            }
            e.kind = static_cast<Kind>(kindByte);
            e.fromInterior = fromInt != 0;
            e.toInterior = toInt != 0;
            loaded.push_back(std::move(e));
        }
        {
            std::scoped_lock lock(g_mutex);
            g_events = std::move(loaded);
        }
        logger::info("TravelEventLog::OnLoad: restored {} record(s)", count);
    }

    void OnRevert()
    {
        std::scoped_lock lock(g_mutex);
        g_events.clear();
        g_lastSnapshot = TravelSnapshot{};
        g_baselineInitialized = false;
        g_fastTravelPending = false;
        g_fastTravelSampledAt = 0.0;
        g_pendingHistory.clear();
    }

    std::vector<EventLogUtil::HistoryEntry> DrainHistoryTail()
    {
        std::vector<PendingHistoryItem> drained;
        {
            std::scoped_lock lock(g_mutex);
            drained.swap(g_pendingHistory);
        }
        std::vector<EventLogUtil::HistoryEntry> out;
        out.reserve(drained.size());
        for (const auto& item : drained) {
            EventLogUtil::HistoryEntry h;
            h.localTime = item.event.localTime;
            h.inGameTimestamp = item.inGameTimestamp;
            h.sourceKind = std::string("internal/travel_event/") + std::string(KindSlug(item.event.kind));
            h.body = RenderEventBody(item.event);
            out.push_back(std::move(h));
        }
        return out;
    }
} // namespace NarrativeEngine::TravelEventLog
