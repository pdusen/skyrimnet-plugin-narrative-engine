#include <EventLogUtil.h>

#include <SKSE/SKSE.h>

#include <RE/Skyrim.h>

#include <array>
#include <filesystem>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <logger.h>

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

    namespace
    {
        // Skyrim month names, 0-indexed. Frozen — matches
        // Calendar::GetMonth's 0-11 return.
        constexpr std::array<const char*, 12> kMonthNames = {
            "Morning Star",
            "Sun's Dawn",
            "First Seed",
            "Rain's Hand",
            "Second Seed",
            "Midyear",
            "Sun's Height",
            "Last Seed",
            "Hearthfire",
            "Frostfall",
            "Sun's Dusk",
            "Evening Star",
        };

        // Days per month, 0-indexed. Skyrim's calendar is 365 days
        // exactly (no leap years).
        constexpr std::array<int, 12> kMonthDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

        // Convert (year, monthIdx, day-1-indexed, hh:mm:ss) into a
        // formatted bracketed string.
        std::string FormatDate(int year, std::uint32_t monthIdx, int day, int h, int m, int s)
        {
            const char* monthName = (monthIdx < kMonthNames.size()) ? kMonthNames[monthIdx] : "Unknown";
            char buf[80];
            std::snprintf(buf, sizeof(buf), "[4E %d, %s %d, %02d:%02d:%02d]", year, monthName, day, h, m, s);
            return std::string(buf);
        }
    } // namespace

    std::string CurrentInGameTimestamp()
    {
        auto* cal = RE::Calendar::GetSingleton();
        if (!cal) {
            return "[unknown time]";
        }

        const int year = cal->GetYear();
        const std::uint32_t monthIdx = static_cast<std::uint32_t>(cal->GetMonth());
        const int day = static_cast<int>(cal->GetDay());
        const float hour = cal->GetHour();

        const int h = static_cast<int>(hour);
        const float remMinutes = (hour - static_cast<float>(h)) * 60.0f;
        const int m = static_cast<int>(remMinutes);
        const float remSeconds = (remMinutes - static_cast<float>(m)) * 60.0f;
        const int s = static_cast<int>(remSeconds);

        return FormatDate(year, monthIdx, day, h, m, s);
    }

    std::string FormatInGameTimestampFromGameTime(double gameTimeSeconds)
    {
        // Reference epoch: 17 Last Seed 4E 201 = daysPassed 0 (Skyrim's
        // canonical game start). Compute day-of-year for that anchor,
        // then walk forward by the event's daysPassed value.
        //
        //   Last Seed is month index 7. Day-of-year for "17 Last Seed"
        //   (0-indexed) = 31 + 28 + 31 + 30 + 31 + 30 + 31 + 16 = 228.
        constexpr int kGameStartDayOfYear = 228;
        constexpr int kGameStartYear = 201;
        constexpr int kYearDays = 365;

        const double daysFromStart = gameTimeSeconds / 86400.0;
        const int wholeDays = static_cast<int>(std::floor(daysFromStart));
        const double dayFraction = daysFromStart - static_cast<double>(wholeDays);

        // Convert to absolute-days-since-year-0 so month/year rollover
        // is trivial arithmetic.
        int absoluteDays = kGameStartYear * kYearDays + kGameStartDayOfYear + wholeDays;
        if (absoluteDays < 0) {
            // Defensive: negative gameTime would produce nonsense. Fall
            // back to a placeholder rather than emit garbage.
            return "[invalid time]";
        }

        int year = absoluteDays / kYearDays;
        int dayOfYear = absoluteDays % kYearDays;

        std::uint32_t monthIdx = 0;
        while (monthIdx < 11 && dayOfYear >= kMonthDays[monthIdx]) {
            dayOfYear -= kMonthDays[monthIdx];
            ++monthIdx;
        }
        const int day = dayOfYear + 1; // 1-indexed for display

        const double hoursFractional = dayFraction * 24.0;
        const int h = static_cast<int>(std::floor(hoursFractional));
        const double minutesFractional = (hoursFractional - static_cast<double>(h)) * 60.0;
        const int m = static_cast<int>(std::floor(minutesFractional));
        const double secondsFractional = (minutesFractional - static_cast<double>(m)) * 60.0;
        const int s = static_cast<int>(std::floor(secondsFractional));

        return FormatDate(year, monthIdx, day, h, m, s);
    }

    std::vector<HistoryEntry> DrainVector(std::vector<HistoryEntry>& pending)
    {
        std::vector<HistoryEntry> out;
        out.swap(pending);
        return out;
    }
    void RotateLogFiles(const std::filesystem::path& directory,
                        std::string_view stem,
                        int slots,
                        std::string_view label)
    {
        if (directory.empty() || stem.empty() || slots < 1) {
            return;
        }

        const auto pathForSlot = [&](int slot) {
            const std::string name =
                slot == 0 ? std::string(stem) + ".log" : std::string(stem) + "." + std::to_string(slot) + ".log";
            return directory / name;
        };

        std::error_code ec;

        // Drop the oldest.
        const auto oldest = pathForSlot(slots);
        if (std::filesystem::exists(oldest, ec)) {
            std::filesystem::remove(oldest, ec);
            if (ec) {
                logger::warn("{}: failed to delete '{}': {}", label, oldest.string(), ec.message());
                ec.clear();
            }
        }

        // Shift the history down. These may be renamed freely -- nothing
        // reads a rotated file while the game is running.
        for (int slot = slots - 1; slot >= 1; --slot) {
            const auto src = pathForSlot(slot);
            const auto dst = pathForSlot(slot + 1);
            if (std::filesystem::exists(src, ec)) {
                std::filesystem::rename(src, dst, ec);
                if (ec) {
                    logger::warn("{}: rotate '{}' -> '{}' failed: {}", label, src.string(), dst.string(), ec.message());
                    ec.clear();
                }
            }
        }

        // The current file is COPIED into slot 1 and left where it is.
        // See the header for why this is not a rename. The caller
        // truncates it on reopen.
        const auto current = pathForSlot(0);
        if (std::filesystem::exists(current, ec)) {
            std::filesystem::copy_file(current, pathForSlot(1), std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                logger::warn("{}: copy current '{}' -> '{}' failed: {}",
                             label,
                             current.string(),
                             pathForSlot(1).string(),
                             ec.message());
            }
        }
    }
} // namespace NarrativeEngine::EventLogUtil
