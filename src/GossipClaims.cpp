#include <GossipClaims.h>

#include <GossipSim.h>
#include <GossipState.h>

#include <GossipLog.h>
#include <logger.h>
#include <Settings.h>

#include <SKSE/SKSE.h>

#include <format>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace NarrativeEngine::GossipClaims
{
    namespace
    {
        // v2 nests each memory claim's event ids inside its entry. v1
        // records are skipped rather than migrated.
        constexpr std::uint32_t kRecordVersion = 2;

        std::mutex g_mutex;

        // The ledger lives in GossipState alongside the simulation, not
        // in a global of its own, because the two have to be SAVED FROM
        // THE SAME INSTANT. Publish them separately and a reload can
        // restore a rumor whose source memory is no longer claimed,
        // leaving that memory free to seed a second rumor about a
        // happening already going round.
        //
        // GossipSim owns the instance; these are references into it.
        // EventClaim itself moved to GossipState.h for the same reason.
        using GossipState_::EventClaim;

        auto& g_claims = GossipSim::MutableState().claims;
        auto& g_eventClaims = GossipSim::MutableState().eventClaims;
    } // namespace

    void Initialize()
    {
        const auto& cfg = Settings::Get();
        logger::info("GossipClaims: initialized (expiry={} days, harvest window={} days)",
                     cfg.gossipClaimExpiryDays,
                     cfg.gossipHarvestWindowDays);
    }

    bool IsClaimed(std::int64_t memoryId)
    {
        std::scoped_lock lock(g_mutex);
        return g_claims.contains(memoryId);
    }

    bool AreEventsClaimed(const std::vector<std::int64_t>& eventIds)
    {
        if (eventIds.empty()) {
            return false;
        }
        std::scoped_lock lock(g_mutex);
        for (const auto e : eventIds) {
            if (g_eventClaims.contains(e)) {
                return true;
            }
        }
        return false;
    }

    void Claim(std::int64_t memoryId, const std::vector<std::int64_t>& eventIds, double nowGameDay)
    {
        const auto& cfg = Settings::Get();
        const double expiry = nowGameDay + std::max(1.0f, cfg.gossipClaimExpiryDays);
        std::size_t outstanding = 0;
        std::size_t eventsTaken = 0;
        bool inserted = false;
        {
            std::scoped_lock lock(g_mutex);
            // emplace, not insert_or_assign: a repeated claim must not push
            // the expiry further out, or a memory could be held indefinitely.
            inserted = g_claims.emplace(memoryId, expiry).second;
            outstanding = g_claims.size();

            // Every related event is claimed separately, on the same expiry.
            // This is what stops the same happening becoming several rumors:
            // SkyrimNet writes one memory per actor present, each with its
            // own id but overlapping related_event_ids, and per-memory dedup
            // alone lets all of them through.
            for (const auto e : eventIds) {
                if (g_eventClaims.emplace(e, EventClaim{expiry, memoryId}).second) {
                    ++eventsTaken;
                }
            }
        }
        // Logged outside the lock: GossipLog takes its own, and nesting two
        // module locks in a fixed order is a habit worth not starting.
        if (inserted) {
            GossipLog::Claim(memoryId, "claimed", outstanding);
        }
        if (eventsTaken > 0) {
            // One line for the batch, not one per event — a single memory
            // can carry a hundred of them.
            GossipLog::Note(std::format("claim: memory {} also claimed {} related event(s)", memoryId, eventsTaken));
        }
    }

    void Release(std::int64_t memoryId)
    {
        std::size_t outstanding = 0;
        std::size_t eventsFreed = 0;
        bool erased = false;
        {
            std::scoped_lock lock(g_mutex);
            erased = g_claims.erase(memoryId) > 0;
            outstanding = g_claims.size();

            // The events go back too. Holding them after a failed
            // generation would lock every other witness's account of the
            // same happening out for the whole expiry window, in exchange
            // for a rumor that does not exist.
            for (auto it = g_eventClaims.begin(); it != g_eventClaims.end();) {
                if (it->second.claimedByMemoryId == memoryId) {
                    it = g_eventClaims.erase(it);
                    ++eventsFreed;
                } else {
                    ++it;
                }
            }
        }
        if (erased) {
            GossipLog::Claim(memoryId, "released", outstanding);
        }
        if (eventsFreed > 0) {
            GossipLog::Note(std::format("claim: memory {} released {} related event(s)", memoryId, eventsFreed));
        }
    }

    void ReleaseEvents(std::int64_t memoryId)
    {
        std::size_t freed = 0;
        {
            std::scoped_lock lock(g_mutex);
            for (auto it = g_eventClaims.begin(); it != g_eventClaims.end();) {
                if (it->second.claimedByMemoryId == memoryId) {
                    it = g_eventClaims.erase(it);
                    ++freed;
                } else {
                    ++it;
                }
            }
        }
        if (freed > 0) {
            GossipLog::Note(std::format("claim: memory {} kept, but released {} related event(s)", memoryId, freed));
        }
    }

    std::size_t Sweep(double nowGameDay)
    {
        std::vector<std::int64_t> expired;
        std::size_t outstanding = 0;
        std::size_t eventsExpired = 0;
        {
            std::scoped_lock lock(g_mutex);
            for (auto it = g_claims.begin(); it != g_claims.end();) {
                if (it->second <= nowGameDay) {
                    expired.push_back(it->first);
                    it = g_claims.erase(it);
                } else {
                    ++it;
                }
            }
            outstanding = g_claims.size();

            // Event claims age out on their own stored expiry rather than
            // with their memory. They were written with the same expiry, so
            // in practice they go together; doing it independently means a
            // stray event claim can never outlive the sweep that should
            // have taken it.
            for (auto it = g_eventClaims.begin(); it != g_eventClaims.end();) {
                if (it->second.expiresOnGameDay <= nowGameDay) {
                    it = g_eventClaims.erase(it);
                    ++eventsExpired;
                } else {
                    ++it;
                }
            }
        }
        for (const auto id : expired) {
            GossipLog::Claim(id, "expired", outstanding);
        }
        if (eventsExpired > 0) {
            GossipLog::Note(std::format("claim: {} event claim(s) expired", eventsExpired));
        }
        return expired.size();
    }

    std::size_t Count()
    {
        std::scoped_lock lock(g_mutex);
        return g_claims.size();
    }

    std::size_t EventCount()
    {
        std::scoped_lock lock(g_mutex);
        return g_eventClaims.size();
    }

    void OnSave(SKSE::SerializationInterface* intfc)
    {
        if (!intfc) {
            return;
        }
        std::scoped_lock lock(g_mutex);
        if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
            logger::error("GossipClaims::OnSave: OpenRecord failed");
            return;
        }

        // Invert the event map so each memory's events can be written
        // beneath it. The runtime keeps them keyed by event id because
        // that is what the harvester probes; only the on-disk shape is
        // grouped.
        std::unordered_map<std::int64_t, std::vector<std::int64_t>> byOwner;
        for (const auto& [eventId, claim] : g_eventClaims) {
            byOwner[claim.claimedByMemoryId].push_back(eventId);
        }

        const auto count = static_cast<std::uint32_t>(g_claims.size());
        intfc->WriteRecordData(count);
        std::size_t eventsWritten = 0;
        for (const auto& [id, expiry] : g_claims) {
            intfc->WriteRecordData(id);
            intfc->WriteRecordData(expiry);

            const auto it = byOwner.find(id);
            const auto eventCount = static_cast<std::uint32_t>(it == byOwner.end() ? 0 : it->second.size());
            intfc->WriteRecordData(eventCount);
            if (it != byOwner.end()) {
                for (const auto eventId : it->second) {
                    intfc->WriteRecordData(eventId);
                }
                eventsWritten += it->second.size();
                byOwner.erase(it);
            }
        }

        // Anything left in byOwner is an event claim whose memory claim is
        // gone. The two are created and destroyed together, so this should
        // be empty; if it is not, the entries are dropped here rather than
        // written somewhere nothing will read them, and the count is worth
        // seeing.
        if (!byOwner.empty()) {
            std::size_t orphans = 0;
            for (const auto& [owner, events] : byOwner) {
                orphans += events.size();
            }
            logger::warn("GossipClaims::OnSave: dropped {} orphaned event claim(s) across {} missing memory claim(s)",
                         orphans,
                         byOwner.size());
        }
        logger::debug("GossipClaims::OnSave: wrote {} claims and {} event claim(s)", count, eventsWritten);
    }

    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t)
    {
        if (!intfc) {
            return;
        }
        if (version != kRecordVersion) {
            logger::warn("GossipClaims::OnLoad: unrecognized record version {} (expected {}); skipping",
                         version,
                         kRecordVersion);
            OnRevert();
            return;
        }

        std::scoped_lock lock(g_mutex);
        g_claims.clear();
        g_eventClaims.clear();

        std::uint32_t count = 0;
        if (intfc->ReadRecordData(count) != sizeof(count)) {
            logger::error("GossipClaims::OnLoad: short read on header; reverting");
            return;
        }
        std::size_t eventsRestored = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::int64_t id = 0;
            double expiry = 0.0;
            std::uint32_t eventCount = 0;
            if (intfc->ReadRecordData(id) != sizeof(id) || intfc->ReadRecordData(expiry) != sizeof(expiry)
                || intfc->ReadRecordData(eventCount) != sizeof(eventCount)) {
                logger::error("GossipClaims::OnLoad: short read on claim {}; reverting", i);
                g_claims.clear();
                g_eventClaims.clear();
                return;
            }
            // Memory and event ids are SkyrimNet's own, not FormIDs — no
            // ResolveFormID, and they are stable across a load order change
            // in a way FormIDs are not.
            g_claims.emplace(id, expiry);

            for (std::uint32_t e = 0; e < eventCount; ++e) {
                std::int64_t eventId = 0;
                if (intfc->ReadRecordData(eventId) != sizeof(eventId)) {
                    logger::error("GossipClaims::OnLoad: short read on event {} of claim {}; reverting", e, i);
                    g_claims.clear();
                    g_eventClaims.clear();
                    return;
                }
                // Expiry and owner are rebuilt from the claim this event
                // was written under; they are not stored per event.
                g_eventClaims.emplace(eventId, EventClaim{expiry, id});
                ++eventsRestored;
            }
        }
        logger::info("GossipClaims::OnLoad: restored {} claims and {} event claim(s)", g_claims.size(), eventsRestored);
    }

    void OnRevert()
    {
        std::scoped_lock lock(g_mutex);
        g_claims.clear();
        g_eventClaims.clear();
    }
} // namespace NarrativeEngine::GossipClaims
