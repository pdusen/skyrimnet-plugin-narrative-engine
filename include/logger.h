#pragma once

#include <EventLogUtil.h>

#include <spdlog/sinks/basic_file_sink.h>

namespace logger = SKSE::log;

// Keep the last few sessions of the main plugin log, the way the gossip
// and event-history logs already do.
inline constexpr int kMainLogRotationSlots = 5;

inline void SetupLog()
{
    auto logsFolder = SKSE::log::log_directory();
    if (!logsFolder)
        SKSE::stl::report_and_fail("SKSE log_directory not provided, logs disabled.");
    auto pluginName = SKSE::PluginDeclaration::GetSingleton()->GetName();
    auto logFilePath = *logsFolder / std::format("{}.log", pluginName);

    // Shift the previous session into the history before the sink opens.
    //
    // RotateLogFiles copies rather than renames, so the file this sink is
    // about to open is the same file it has always been -- which is what
    // lets an editor tail it across restarts. The sink then truncates it
    // in place, exactly as it did before this call existed; the only
    // change is that the outgoing session now survives as `.1`.
    //
    // ONE CAVEAT: this runs before the default logger is replaced, so any
    // warning the rotation emits goes to spdlog's default sink rather
    // than into this file. There is no way around that ordering -- the
    // file cannot be rotated after the logger has started writing to it
    // -- and a failed rotation is visible anyway as a missing `.1`. If
    // that ever needs diagnosing properly, the fix is for RotateLogFiles
    // to hand its messages back instead of logging them.
    NarrativeEngine::EventLogUtil::RotateLogFiles(*logsFolder, pluginName, kMainLogRotationSlots, "SetupLog");

    auto fileLoggerPtr = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
    auto loggerPtr = std::make_shared<spdlog::logger>("log", std::move(fileLoggerPtr));
    spdlog::set_default_logger(std::move(loggerPtr));
    spdlog::set_level(spdlog::level::trace);
    spdlog::flush_on(spdlog::level::trace);
}
