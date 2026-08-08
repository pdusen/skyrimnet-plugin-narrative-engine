#include <GossipClaims.h>

#include <GossipLog.h>
#include <logger.h>
#include <Settings.h>

#include <SKSE/SKSE.h>

#include <mutex>
#include <unordered_map>
#include <vector>

namespace NarrativeEngine::GossipClaims
{
    namespace
    {
        constexpr std::uint32_t kRecordVersion = 1;

        std::mutex g_mutex;
        // memoryId -> game day the claim expires.
        std::unordered_map<std::int64_t, double> g_claims;
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

    void Claim(std::int64_t memoryId, double nowGameDay)
    {
        const auto& cfg = Settings::Get();
        std::size_t outstanding = 0;
        bool inserted = false;
        {
            std::scoped_lock lock(g_mutex);
            // emplace, not insert_or_assign: a repeated claim must not push
            // the expiry further out, or a memory could be held indefinitely.
            inserted = g_claims.emplace(memoryId, nowGameDay + std::max(1.0f, cfg.gossipClaimExpiryDays)).second;
            outstanding = g_claims.size();
        }
        // Logged outside the lock: GossipLog takes its own, and nesting two
        // module locks in a fixed order is a habit worth not starting.
        if (inserted) {
            GossipLog::Claim(memoryId, "claimed", outstanding);
        }
    }

    void Release(std::int64_t memoryId)
    {
        std::size_t outstanding = 0;
        bool erased = false;
        {
            std::scoped_lock lock(g_mutex);
            erased = g_claims.erase(memoryId) > 0;
            outstanding = g_claims.size();
        }
        if (erased) {
            GossipLog::Claim(memoryId, "released", outstanding);
        }
    }

    std::size_t Sweep(double nowGameDay)
    {
        std::vector<std::int64_t> expired;
        std::size_t outstanding = 0;
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
        }
        for (const auto id : expired) {
            GossipLog::Claim(id, "expired", outstanding);
        }
        return expired.size();
    }

    std::size_t Count()
    {
        std::scoped_lock lock(g_mutex);
        return g_claims.size();
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
        const auto count = static_cast<std::uint32_t>(g_claims.size());
        intfc->WriteRecordData(count);
        for (const auto& [id, expiry] : g_claims) {
            intfc->WriteRecordData(id);
            intfc->WriteRecordData(expiry);
        }
        logger::debug("GossipClaims::OnSave: wrote {} claims", count);
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

        std::uint32_t count = 0;
        if (intfc->ReadRecordData(count) != sizeof(count)) {
            logger::error("GossipClaims::OnLoad: short read on header; reverting");
            return;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            std::int64_t id = 0;
            double expiry = 0.0;
            if (intfc->ReadRecordData(id) != sizeof(id) || intfc->ReadRecordData(expiry) != sizeof(expiry)) {
                logger::error("GossipClaims::OnLoad: short read on claim {}; reverting", i);
                g_claims.clear();
                return;
            }
            // Memory ids are SkyrimNet's own, not FormIDs — no
            // ResolveFormID, and they are stable across a load order change
            // in a way FormIDs are not.
            g_claims.emplace(id, expiry);
        }
        logger::info("GossipClaims::OnLoad: restored {} claims", g_claims.size());
    }

    void OnRevert()
    {
        std::scoped_lock lock(g_mutex);
        g_claims.clear();
    }
} // namespace NarrativeEngine::GossipClaims
