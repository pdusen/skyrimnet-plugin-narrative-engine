#include <SenderCooldownTable.h>

#include <EngineUtils.h>

#include <SKSE/Interfaces.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace NarrativeEngine
{
    void SenderCooldownTable::Stamp(RE::FormID senderFormID)
    {
        if (senderFormID == 0)
            return;
        const double nowHours = EngineUtils::GetCurrentGameHours();
        std::scoped_lock lock(mutex_);
        stamps_[senderFormID] = nowHours;
    }

    bool SenderCooldownTable::IsOnCooldown(RE::FormID senderFormID, int cooldownHours) const
    {
        if (cooldownHours <= 0 || senderFormID == 0)
            return false;
        double stamp = 0.0;
        {
            std::scoped_lock lock(mutex_);
            auto it = stamps_.find(senderFormID);
            if (it == stamps_.end())
                return false;
            stamp = it->second;
        }
        if (stamp <= 0.0)
            return false;
        const double elapsed = EngineUtils::GetCurrentGameHours() - stamp;
        return elapsed < static_cast<double>(cooldownHours);
    }

    void SenderCooldownTable::Clear()
    {
        std::scoped_lock lock(mutex_);
        stamps_.clear();
    }

    void SenderCooldownTable::Serialize(SKSE::SerializationInterface* intfc) const
    {
        if (!intfc)
            return;
        std::vector<std::pair<RE::FormID, double>> snapshot;
        {
            std::scoped_lock lock(mutex_);
            snapshot.reserve(stamps_.size());
            for (const auto& kv : stamps_) {
                snapshot.emplace_back(kv.first, kv.second);
            }
        }
        const std::uint32_t count = static_cast<std::uint32_t>(snapshot.size());
        intfc->WriteRecordData(count);
        for (const auto& kv : snapshot) {
            intfc->WriteRecordData(kv.first);
            intfc->WriteRecordData(kv.second);
        }
    }

    bool SenderCooldownTable::Deserialize(SKSE::SerializationInterface* intfc)
    {
        if (!intfc)
            return false;
        std::uint32_t count = 0;
        if (intfc->ReadRecordData(count) != sizeof(count)) {
            Clear();
            return false;
        }
        std::unordered_map<RE::FormID, double> loaded;
        loaded.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            RE::FormID fid = 0;
            double h = 0.0;
            if (intfc->ReadRecordData(fid) != sizeof(fid) || intfc->ReadRecordData(h) != sizeof(h)) {
                Clear();
                return false;
            }
            RE::FormID resolved = 0;
            if (intfc->ResolveFormID(fid, resolved) && resolved != 0) {
                loaded[resolved] = h;
            }
        }
        {
            std::scoped_lock lock(mutex_);
            stamps_ = std::move(loaded);
        }
        return true;
    }
} // namespace NarrativeEngine
