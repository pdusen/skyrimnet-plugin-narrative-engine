#include <SenderCooldownTable.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace NarrativeEngine
{
    void SenderCooldownTable::Stamp(FormID senderFormID, double nowGameHours)
    {
        if (senderFormID == 0)
            return;
        std::scoped_lock lock(mutex_);
        stamps_[senderFormID] = nowGameHours;
    }

    bool SenderCooldownTable::IsOnCooldown(FormID senderFormID, int cooldownHours, double nowGameHours) const
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
        const double elapsed = nowGameHours - stamp;
        return elapsed < static_cast<double>(cooldownHours);
    }

    std::optional<double> SenderCooldownTable::GetStampGameHours(FormID senderFormID) const
    {
        if (senderFormID == 0)
            return std::nullopt;
        std::scoped_lock lock(mutex_);
        auto it = stamps_.find(senderFormID);
        if (it == stamps_.end())
            return std::nullopt;
        return it->second;
    }

    void SenderCooldownTable::Clear()
    {
        std::scoped_lock lock(mutex_);
        stamps_.clear();
    }

    void SenderCooldownTable::Serialize(ICosaveIO& io) const
    {
        std::vector<std::pair<FormID, double>> snapshot;
        {
            std::scoped_lock lock(mutex_);
            snapshot.reserve(stamps_.size());
            for (const auto& kv : stamps_) {
                snapshot.emplace_back(kv.first, kv.second);
            }
        }
        const std::uint32_t count = static_cast<std::uint32_t>(snapshot.size());
        io.Write(count);
        for (const auto& kv : snapshot) {
            io.Write(kv.first);
            io.Write(kv.second);
        }
    }

    bool SenderCooldownTable::Deserialize(ICosaveIO& io)
    {
        std::uint32_t count = 0;
        if (!io.Read(count)) {
            Clear();
            return false;
        }
        std::unordered_map<FormID, double> loaded;
        loaded.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            FormID fid = 0;
            double h = 0.0;
            if (!io.Read(fid) || !io.Read(h)) {
                Clear();
                return false;
            }
            FormID resolved = 0;
            if (io.ResolveFormID(fid, resolved) && resolved != 0) {
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
