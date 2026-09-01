#include <PlotLog.h>

#include <EventLogUtil.h>
#include <logger.h>
#include <PlotState.h>
#include <Settings.h>

#include <SKSE/SKSE.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <system_error>

// The file half of the plot trace. The line formatting lives in
// PlotLogFormat.cpp so it can be probed without a filesystem.
namespace NarrativeEngine::PlotLog
{
    namespace
    {
        constexpr const char* kFileStem = "NarrativeEngine_Plots";
        constexpr int kRotationSlots = 5;

        std::mutex g_mutex;
        std::ofstream g_file;
        std::uint32_t g_sessionCounter = 0;
        std::size_t g_linesWritten = 0;

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

        // Delete .5, shift .4 -> .5 ... current -> .1. Failures are
        // logged and swallowed: a rotation that cannot happen degrades to
        // "this session's file only", which is still useful.
        void RotateFilesLocked()
        {
            std::error_code ec;
            const auto oldest = FilePathForSlot(kRotationSlots);
            if (std::filesystem::exists(oldest, ec)) {
                std::filesystem::remove(oldest, ec);
                ec.clear();
            }
            for (int slot = kRotationSlots - 1; slot >= 1; --slot) {
                const auto src = FilePathForSlot(slot);
                if (std::filesystem::exists(src, ec)) {
                    std::filesystem::rename(src, FilePathForSlot(slot + 1), ec);
                    ec.clear();
                }
            }
            const auto current = FilePathForSlot(0);
            if (std::filesystem::exists(current, ec)) {
                std::filesystem::rename(current, FilePathForSlot(1), ec);
                if (ec) {
                    logger::warn("PlotLog: rotate of '{}' failed: {}", current.string(), ec.message());
                }
            }
        }

        // Flushed as it is written. From Step 16 a plot tick blocks on
        // LLM round trips, and buffering would leave the file silent and
        // then bursting, with its tail on a half-written line while it
        // waits. Gossip shipped that mistake once.
        void WriteLineLocked(std::string_view body)
        {
            if (!g_file.is_open()) {
                return;
            }
            g_file << EventLogUtil::CurrentInGameTimestamp() << ' ' << body << '\n' << std::flush;
            ++g_linesWritten;
        }

        void Emit(std::string_view body)
        {
            std::scoped_lock lock(g_mutex);
            WriteLineLocked(body);
        }

        bool Enabled()
        {
            const auto& cfg = Settings::Get();
            // bPlotLogEnabled alone, never bDebugMode: the point is a
            // quiet main log and a complete plot trace at the same time.
            return cfg.plotsEnabled && cfg.plotLogEnabled;
        }
    } // namespace

    void Initialize()
    {
        const auto& cfg = Settings::Get();
        logger::info("PlotLog: initialized (enabled={}, plotsEnabled={})", cfg.plotLogEnabled, cfg.plotsEnabled);
    }

    void OnSessionStart()
    {
        if (!Enabled()) {
            return;
        }

        std::scoped_lock lock(g_mutex);
        if (g_file.is_open()) {
            g_file.flush();
            g_file.close();
        }
        RotateFilesLocked();
        ++g_sessionCounter;
        g_linesWritten = 0;

        const auto path = FilePathForSlot(0);
        if (path.empty()) {
            logger::warn("PlotLog: no SKSE log directory; the plot trace will not be written.");
            return;
        }
        g_file.open(path, std::ios::out | std::ios::trunc);
        if (!g_file.is_open()) {
            logger::warn("PlotLog: could not open '{}'.", path.string());
            return;
        }

        const auto& cfg = Settings::Get();
        WriteLineLocked(std::format("SESSION start #{} tickInterval={}h budget={} mishap={} rates={:.2f}-{:.2f}",
                                    g_sessionCounter,
                                    cfg.plotTickIntervalGameHours,
                                    cfg.plotMaxConcurrent,
                                    cfg.plotMishapEnabled ? cfg.plotMishapChanceBase : 0.0f,
                                    cfg.plotProgressRateMin,
                                    cfg.plotProgressRateMax));
        logger::info("PlotLog: writing to {}", path.string());
    }

    void OnSessionEnd()
    {
        std::scoped_lock lock(g_mutex);
        if (!g_file.is_open()) {
            return;
        }
        // A closing census, so a run can be assessed without re-reading
        // the whole file.
        const auto snap = Plots::Snapshot();
        const auto& c = snap->counters;
        WriteLineLocked(std::format("CENSUS ticks={} born={} succeeded={} failed={} steps(ok={} timeout={} "
                                    "caught={}) adaptations={} lines={}",
                                    c.ticksRun,
                                    c.plotsBorn,
                                    c.plotsSucceeded,
                                    c.plotsFailed,
                                    c.stepsSucceeded,
                                    c.stepsTimedOut,
                                    c.stepsCaught,
                                    c.adaptations,
                                    g_linesWritten + 1));
        g_file.flush();
        g_file.close();
    }

    bool IsOpen()
    {
        std::scoped_lock lock(g_mutex);
        return g_file.is_open();
    }

    void Tick(std::uint32_t tickNumber, double gameDay, std::size_t activePlots, std::size_t budget)
    {
        Emit(FormatTick(tickNumber, gameDay, activePlots, budget));
    }

    void Born(const PlotModel::Plot& plot, double weight, std::size_t considered)
    {
        Emit(FormatBorn(plot, weight, considered));
    }

    void Dispatch(const PlotModel::Plot& plot,
                  const PlotModel::Step& step,
                  int rung,
                  std::size_t considered,
                  std::size_t rejected)
    {
        Emit(FormatDispatch(plot, step, rung, considered, rejected));
    }

    void Roll(const PlotModel::Plot& plot, const PlotModel::Step& step, const PlotModel::TickRecord& record)
    {
        Emit(FormatRoll(plot, step, record));
    }

    void Resolve(const PlotModel::Plot& plot, const PlotModel::Step& step)
    {
        Emit(FormatResolve(plot, step));
    }

    void Adapt(const PlotModel::Plot& plot, int adaptations, int cap, std::size_t newTailLength)
    {
        Emit(FormatAdapt(plot, adaptations, cap, newTailLength));
    }

    void End(const PlotModel::Plot& plot)
    {
        Emit(FormatEnd(plot));
    }

    void BirthRejected(const std::string& mastermind, const std::string& reason)
    {
        Emit(std::format("REJECT mastermind=\"{}\" reason=\"{}\"", mastermind, reason));
    }

    void Reap(std::size_t plots, std::size_t occupancyRows, double gameDay)
    {
        // Either alone is worth a line: a tick can retire spent cooldown
        // rows without any plot ageing out of the retention window.
        if (plots == 0 && occupancyRows == 0) {
            return;
        }
        Emit(std::format("REAP  {} plot(s) {} occupancy row(s) day={:.2f}", plots, occupancyRows, gameDay));
    }

    void Note(std::string_view text)
    {
        Emit(std::format("NOTE  {}", text));
    }
} // namespace NarrativeEngine::PlotLog
