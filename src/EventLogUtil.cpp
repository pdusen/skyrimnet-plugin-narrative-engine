#include <EventLogUtil.h>

#include <SKSE/SKSE.h>

#include <RE/Skyrim.h>

#include <chrono>

namespace NarrativeEngine::EventLogUtil
{
    double NowUnixSeconds()
    {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    double NowGameTimeSeconds()
    {
        if (auto* cal = RE::Calendar::GetSingleton()) {
            // GetDaysPassed() returns cumulative days as a float, already
            // including the intra-day fraction — no need to add hour-of-day
            // separately.
            return static_cast<double>(cal->GetDaysPassed()) * 86400.0;
        }
        return 0.0;
    }

    void WriteString(SKSE::SerializationInterface* intfc, const std::string& s)
    {
        const auto len = static_cast<std::uint32_t>(s.size());
        intfc->WriteRecordData(len);
        if (len > 0)
            intfc->WriteRecordData(s.data(), len);
    }

    bool ReadString(SKSE::SerializationInterface* intfc, std::string& out)
    {
        std::uint32_t len = 0;
        if (intfc->ReadRecordData(len) != sizeof(len))
            return false;
        out.resize(len);
        if (len > 0 && intfc->ReadRecordData(out.data(), len) != len)
            return false;
        return true;
    }
} // namespace NarrativeEngine::EventLogUtil
