#include <EventHistoryWriter.h>

#include <CombatEventLog.h>
#include <EventLogUtil.h>
#include <logger.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <SkyrimNetEvents.h>
#include <TravelEventLog.h>
#include <WeatherEventLog.h>

#include <nlohmann/json.hpp>

#include <SKSE/SKSE.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace NarrativeEngine::EventHistoryWriter
{
    namespace
    {
        constexpr const char* kFileStem = "NarrativeEngine_EventHistory";
        constexpr int kRotationSlots = 5; // keep current + .1 .. .5

        std::mutex g_mutex;

        // Open file handle for the current session. `nullopt`-shaped
        // (unopened) when no save is loaded.
        std::ofstream g_file;

        // Tick-driven accumulator toward the next flush. Grows by the
        // caller-supplied unpausedElapsedSeconds each Poll call; when
        // it crosses iEventHistoryFlushIntervalSeconds, flush + subtract.
        double g_unpausedSecondsSinceLastFlush = 0.0;

        // SkyrimNet event dedup: bookmark the largest `localTime` we've
        // written so far. Next fetch filters to > this value. Reset at
        // OnSessionStart.
        double g_lastSkyrimNetLocalTime = 0.0;

        // Session-tracking bookkeeping. Rotations happen in
        // OnSessionStart; log lines here reference the count so
        // debugging log-rotation issues is easier.
        std::uint32_t g_sessionCounter = 0;

        double FlushIntervalSeconds()
        {
            const int v = Settings::Get().eventHistoryFlushIntervalSeconds;
            return v <= 0 ? 5.0 : static_cast<double>(v);
        }

        std::filesystem::path LogDirectory()
        {
            auto dir = SKSE::log::log_directory();
            if (!dir) {
                return {};
            }
            return *dir;
        }

        std::filesystem::path FilePathForSlot(int slot)
        {
            const auto dir = LogDirectory();
            if (slot == 0) {
                return dir / (std::string(kFileStem) + ".log");
            }
            return dir / (std::string(kFileStem) + "." + std::to_string(slot) + ".log");
        }

        // Rotate: delete .5, rename .4 -> .5, .3 -> .4, ..., current
        // -> .1. Errors are logged and swallowed — the writer degrades
        // to "current session's file only" if rotation fails.
        void RotateFilesLocked()
        {
            std::error_code ec;

            // Drop the oldest.
            const auto oldest = FilePathForSlot(kRotationSlots);
            if (std::filesystem::exists(oldest, ec)) {
                std::filesystem::remove(oldest, ec);
                if (ec) {
                    logger::warn("EventHistoryWriter: failed to delete '{}': {}", oldest.string(), ec.message());
                    ec.clear();
                }
            }

            // Shift .4 -> .5, .3 -> .4, ..., .1 -> .2.
            for (int slot = kRotationSlots - 1; slot >= 1; --slot) {
                const auto src = FilePathForSlot(slot);
                const auto dst = FilePathForSlot(slot + 1);
                if (std::filesystem::exists(src, ec)) {
                    std::filesystem::rename(src, dst, ec);
                    if (ec) {
                        logger::warn("EventHistoryWriter: rotate '{}' -> '{}' failed: {}",
                                     src.string(),
                                     dst.string(),
                                     ec.message());
                        ec.clear();
                    }
                }
            }

            // Current -> .1.
            const auto current = FilePathForSlot(0);
            const auto first = FilePathForSlot(1);
            if (std::filesystem::exists(current, ec)) {
                std::filesystem::rename(current, first, ec);
                if (ec) {
                    logger::warn("EventHistoryWriter: rotate current '{}' -> '{}' failed: {}",
                                 current.string(),
                                 first.string(),
                                 ec.message());
                }
            }
        }

        // Open the current-slot file in truncate mode. On failure,
        // leaves g_file closed so subsequent Poll calls are no-ops.
        void OpenCurrentFileLocked()
        {
            const auto path = FilePathForSlot(0);
            g_file.open(path, std::ios::out | std::ios::trunc);
            if (!g_file.is_open()) {
                logger::error("EventHistoryWriter: failed to open '{}' for writing", path.string());
                return;
            }
            g_file << "# NarrativeEngine event history log (session " << g_sessionCounter << ")\n"
                   << "# Format: [<in-game timestamp>] <source>/<kind>: <text>\n\n";
            g_file.flush();
            logger::info("EventHistoryWriter: opened '{}' for session {}", path.string(), g_sessionCounter);
        }

        // Fetch fresh SkyrimNet events, filter to those newer than
        // g_lastSkyrimNetLocalTime, render each to a HistoryEntry.
        // Bookmark advances to the newest kept event.
        std::vector<EventLogUtil::HistoryEntry> FetchFreshSkyrimNetEntriesLocked()
        {
            std::vector<EventLogUtil::HistoryEntry> out;

            // Over-fetch — SkyrimNet's event ring is bounded; if we
            // flush every 5s and the game fires up to ~40 events/tick,
            // 200 gives generous headroom without being expensive.
            constexpr int kFetchCount = 200;
            const std::string raw = SkyrimNetAPI::GetRecentEvents(0, kFetchCount, "");
            if (raw.empty()) {
                return out;
            }

            auto parsed = nlohmann::json::parse(raw, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_array()) {
                return out;
            }

            // FormatEventsText synthesizes `text` from `type` + `data`
            // in-place. We give it `0` as currentGameTimeSeconds so the
            // "[N ago]" prefix collapses to "just now" — we don't use
            // that prefix here; we prepend our own in-game timestamp.
            SkyrimNetEvents::FormatEventsText(parsed, 0.0);

            double newestKept = g_lastSkyrimNetLocalTime;

            for (const auto& evt : parsed) {
                if (!evt.is_object())
                    continue;
                const double localTime = evt.value("localTime", 0.0);
                if (localTime <= g_lastSkyrimNetLocalTime)
                    continue; // already logged in a prior flush

                // The `text` field FormatEventsText emitted has a
                // "[just now] " prefix baked in from the delta=~0
                // rendering; strip it for the history line.
                std::string text = evt.value("text", std::string{});
                if (!text.empty() && text.front() == '[') {
                    const auto close = text.find(']');
                    if (close != std::string::npos && close + 2 <= text.size()) {
                        text.erase(0, close + 2); // "[...] " prefix (bracket + space)
                    }
                }

                std::string kind = evt.value("type", std::string{});
                if (kind.empty()) {
                    kind = "unknown";
                }

                // Decode each event's own gameTime into its Skyrim
                // date, so backfill batches at session start don't
                // all collapse to the flush moment.
                const double gameTime = evt.value("gameTime", 0.0);

                EventLogUtil::HistoryEntry h;
                h.localTime = localTime;
                h.inGameTimestamp = EventLogUtil::FormatInGameTimestampFromGameTime(gameTime);
                h.sourceKind = "skyrimnet/" + kind;
                h.body = std::move(text);
                out.push_back(std::move(h));

                if (localTime > newestKept) {
                    newestKept = localTime;
                }
            }

            g_lastSkyrimNetLocalTime = newestKept;
            return out;
        }

        // Drain all sources, sort, write. Caller holds g_mutex.
        void FlushLocked()
        {
            if (!g_file.is_open()) {
                return;
            }

            std::vector<EventLogUtil::HistoryEntry> batch;

            // Internal event logs — each returns a rendered set that
            // was captured with correct emit-time timestamps.
            auto combatBatch = CombatEventLog::DrainHistoryTail();
            auto weatherBatch = WeatherEventLog::DrainHistoryTail();
            auto travelBatch = TravelEventLog::DrainHistoryTail();

            // SkyrimNet — dedup + fetch + render.
            auto skyrimNetBatch = FetchFreshSkyrimNetEntriesLocked();

            batch.reserve(combatBatch.size() + weatherBatch.size() + travelBatch.size() + skyrimNetBatch.size());
            for (auto& e : combatBatch)
                batch.push_back(std::move(e));
            for (auto& e : weatherBatch)
                batch.push_back(std::move(e));
            for (auto& e : travelBatch)
                batch.push_back(std::move(e));
            for (auto& e : skyrimNetBatch)
                batch.push_back(std::move(e));

            if (batch.empty()) {
                return;
            }

            // Cross-source ordering: stable sort by localTime so events
            // that fired at the same instant preserve their original
            // enqueue order.
            std::stable_sort(
                batch.begin(), batch.end(), [](const auto& a, const auto& b) { return a.localTime < b.localTime; });

            for (const auto& e : batch) {
                g_file << e.inGameTimestamp << ' ' << e.sourceKind << ": " << e.body << '\n';
            }
            g_file.flush();
        }
    } // namespace

    void Initialize()
    {
        const bool enabled = Settings::Get().eventHistoryEnabled;
        logger::info("EventHistoryWriter: initialized (enabled={}, flush interval={:.1f}s of unpaused play)",
                     enabled,
                     FlushIntervalSeconds());
    }

    void OnSessionStart()
    {
        if (!Settings::Get().eventHistoryEnabled) {
            return; // master switch off — no rotation, no file open
        }
        std::scoped_lock lock(g_mutex);

        // Defensive: if a file was left open from a prior session
        // boundary that skipped OnSessionEnd, flush + close it.
        if (g_file.is_open()) {
            g_file.flush();
            g_file.close();
        }

        RotateFilesLocked();
        ++g_sessionCounter;
        g_unpausedSecondsSinceLastFlush = 0.0;
        g_lastSkyrimNetLocalTime = 0.0;

        OpenCurrentFileLocked();
    }

    void OnSessionEnd()
    {
        std::scoped_lock lock(g_mutex);
        if (!g_file.is_open()) {
            return; // covers both "master switch off" and "no active session"
        }
        FlushLocked();
        g_file.flush();
        g_file.close();
        logger::info("EventHistoryWriter: session {} ended, file closed", g_sessionCounter);
    }

    void Poll(const PluginThread::Token&, double unpausedElapsedSeconds)
    {
        // Runs entirely on the plugin thread — no engine touches. File
        // I/O has no thread affinity, SkyrimNet's fetch API is safe
        // from any thread, and each internal event-log's drain uses
        // its own mutex.
        std::scoped_lock lock(g_mutex);
        if (!g_file.is_open()) {
            return; // no active session, or master switch off
        }

        g_unpausedSecondsSinceLastFlush += unpausedElapsedSeconds;
        const double intervalSec = FlushIntervalSeconds();
        if (g_unpausedSecondsSinceLastFlush < intervalSec) {
            return;
        }
        g_unpausedSecondsSinceLastFlush -= intervalSec;

        FlushLocked();
    }
} // namespace NarrativeEngine::EventHistoryWriter
