#include <VisitorTravelLog.h>

#include <EventLogUtil.h>
#include <logger.h>
#include <Settings.h>

#include <SKSE/SKSE.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace NarrativeEngine::VisitorTravelLog
{
    namespace
    {
        constexpr const char* kFileStem = "NarrativeEngine_VisitorTravel";
        constexpr int kRotationSlots = 5;

        // Tag column width. Wide enough for the longest tag so every line
        // after it starts in the same place, which is what lets a reader
        // skim one tag down the file.
        constexpr std::size_t kTagWidth = 8;

        std::mutex g_mutex;
        std::ofstream g_file;
        // Read on every Write before any formatting happens, from both
        // threads, so it is the one piece of state that is not under the
        // mutex.
        std::atomic<bool> g_active{false};
        std::size_t g_linesWritten = 0;
        std::uint32_t g_visitCounter = 0;

        std::filesystem::path FilePathForSlot(int slot)
        {
            auto dir = SKSE::log::log_directory();
            if (!dir) {
                return {};
            }
            if (slot == 0) {
                return *dir / (std::string(kFileStem) + ".log");
            }
            return *dir / (std::string(kFileStem) + "." + std::to_string(slot) + ".log");
        }

        void RotateFilesLocked()
        {
            auto dir = SKSE::log::log_directory();
            if (!dir) {
                return;
            }
            EventLogUtil::RotateLogFiles(*dir, kFileStem, kRotationSlots, "VisitorTravelLog");
        }

        void WriteLineLocked(std::string_view tag, std::string_view body)
        {
            if (!g_file.is_open()) {
                return;
            }
            std::string padded{tag};
            if (padded.size() < kTagWidth) {
                padded.append(kTagWidth - padded.size(), ' ');
            }
            // Flushed per line, for the same reason GossipLog is: a trace
            // whose tail sits in a buffer is unreadable exactly while the
            // thing it traces is running, and a crash mid-visit is one of
            // the cases this file exists to explain.
            g_file << EventLogUtil::CurrentInGameTimestamp() << ' ' << padded << ' ' << body << '\n' << std::flush;
            ++g_linesWritten;
        }
    } // namespace

    void Initialize()
    {
        logger::info("VisitorTravelLog: initialized (enabled={})", Settings::Get().visitorTravelLogEnabled);
    }

    void OnSessionStart()
    {
        if (!Settings::Get().visitorTravelLogEnabled) {
            return;
        }
        std::scoped_lock lock(g_mutex);
        if (g_file.is_open()) {
            g_file.close();
        }
        RotateFilesLocked();
        const auto path = FilePathForSlot(0);
        if (path.empty()) {
            logger::warn("VisitorTravelLog: no log directory; the travel trace will not be written");
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        g_file.open(path, std::ios::out | std::ios::trunc);
        if (!g_file.is_open()) {
            logger::warn("VisitorTravelLog: could not open '{}'", path.string());
            return;
        }
        g_linesWritten = 0;
        g_visitCounter = 0;
        g_active.store(true, std::memory_order_release);
        WriteLineLocked("SESSION", "visitor travel trace opened");
        logger::info("VisitorTravelLog: writing to '{}'", path.string());
    }

    void OnSessionEnd()
    {
        std::scoped_lock lock(g_mutex);
        if (!g_file.is_open()) {
            g_active.store(false, std::memory_order_release);
            return;
        }
        WriteLineLocked("SESSION",
                        std::format("closed after {} line(s) over {} visit(s)", g_linesWritten, g_visitCounter));
        g_active.store(false, std::memory_order_release);
        g_file.flush();
        g_file.close();
    }

    bool IsActive()
    {
        return g_active.load(std::memory_order_acquire);
    }

    void Line(std::string_view tag, std::string_view text)
    {
        if (!IsActive()) {
            return;
        }
        std::scoped_lock lock(g_mutex);
        WriteLineLocked(tag, text);
    }

    void Begin(RE::FormID senderId, std::string_view senderName)
    {
        if (!IsActive()) {
            return;
        }
        std::uint32_t index = 0;
        {
            std::scoped_lock lock(g_mutex);
            index = ++g_visitCounter;
        }
        {
            // A bare blank line between visits, with no timestamp or tag, so
            // the file breaks into readable blocks.
            std::scoped_lock lock(g_mutex);
            if (g_file.is_open()) {
                g_file << '\n' << std::flush;
            }
        }
        Line("BEGIN",
             std::format("visit #{} sender=0x{:08X} '{}'", index, senderId, senderName.empty() ? "?" : senderName));
    }

    void End(RE::FormID senderId, std::string_view tier, std::string_view pointClass)
    {
        Write("END", "sender=0x{:08X} tier={} class={}", senderId, tier, pointClass);
    }

    void Abandoned(RE::FormID senderId, std::string_view reason)
    {
        Write("END", "sender=0x{:08X} abandoned — {}", senderId, reason);
    }
} // namespace NarrativeEngine::VisitorTravelLog
