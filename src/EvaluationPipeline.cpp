#include <EvaluationPipeline.h>

#include <AlphaCanon.h>
#include <DecisionLog.h>
#include <PhaseTracker.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <logger.h>

#include <algorithm>
#include <atomic>
#include <chrono>

namespace NarrativeEngine::EvaluationPipeline
{
    namespace
    {
        std::atomic<bool> g_inFlight = false;

        // Wall-clock anchor for Snapshot::realTimeSec — monotonic seconds
        // since the first BuildSnapshot() call.
        const std::chrono::steady_clock::time_point& StartTime()
        {
            static const auto start = std::chrono::steady_clock::now();
            return start;
        }

        double SecondsSinceStart()
        {
            const auto now = std::chrono::steady_clock::now();
            return std::chrono::duration<double>(now - StartTime()).count();
        }

        std::string JoinCSV(const std::vector<std::string>& parts)
        {
            std::string out;
            for (const auto& s : parts) {
                if (!out.empty()) out += ',';
                out += s;
            }
            return out;
        }

        // Single multi-line debug dump of a snapshot. Gated on debugMode by
        // the caller.
        void LogSnapshot(const Snapshot& s)
        {
            logger::debug("Snapshot: realTimeSec={:.2f} phase={} timeInPhase={:.2f}s",
                          s.realTimeSec, s.currentPhase, s.timeInPhaseSeconds);
            logger::debug(
                "Snapshot: player formID=0x{:08X} location='{}' (0x{:08X}) cell='{}' (0x{:08X}) interior={} "
                "gameDays={:.3f} hour={:.2f}",
                s.player.formID, s.player.locationName, s.player.locationFormID,
                s.player.cellName, s.player.cellFormID, s.player.cellIsInterior,
                s.player.gameDaysPassed, s.player.timeOfDayHours);
            logger::debug(
                "Snapshot: alphaCanon mask=0x{:08X} signals=[{}] decisionTail={} eventsJSON.size={}B",
                s.alphaCanonSignalBitmask, JoinCSV(s.alphaCanonSignals),
                s.decisionLogTail.size(), s.skyrimNetEventsJSON.size());
        }
    }

    bool IsEvaluationInFlight()
    {
        return g_inFlight.load();
    }

    Snapshot BuildSnapshot()
    {
        // Ensure the static anchor is initialized on the first call. This
        // also implicitly pins the start time to the first tick rather than
        // to plugin load — close enough that the difference doesn't matter.
        (void)StartTime();

        const bool debug = Settings::Get().debugMode;
        if (debug) logger::debug("BuildSnapshot: begin");

        Snapshot s;
        s.realTimeSec = SecondsSinceStart();

        s.currentPhase       = PhaseTracker::PhaseName(PhaseTracker::Get());
        s.timeInPhaseSeconds = PhaseTracker::TimeInPhaseSeconds();
        if (debug) logger::debug("BuildSnapshot: phase OK");

        const int eventTail = std::max(0, Settings::Get().skyrimNetEventTailSizeForPrompt);
        s.skyrimNetEventsJSON = SkyrimNetAPI::GetRecentEvents(0, eventTail, "");
        if (debug) logger::debug("BuildSnapshot: events OK ({}B)", s.skyrimNetEventsJSON.size());

        const int decisionTail = std::max(0, Settings::Get().decisionLogTailSizeForPrompt);
        s.decisionLogTail = DecisionLog::Tail(static_cast<std::size_t>(decisionTail));
        if (debug) logger::debug("BuildSnapshot: decisionTail OK");

        if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
            s.player.formID = pc->GetFormID();
            if (debug) logger::debug("BuildSnapshot: pc OK");

            if (auto* loc = pc->GetCurrentLocation()) {
                s.player.locationFormID = loc->GetFormID();
                if (const char* name = loc->GetFullName(); name && *name) {
                    s.player.locationName = name;
                }
            }
            if (debug) logger::debug("BuildSnapshot: location OK");

            if (auto* cell = pc->GetParentCell()) {
                s.player.cellFormID     = cell->GetFormID();
                s.player.cellIsInterior = cell->IsInteriorCell();
                if (const char* name = cell->GetFullName(); name && *name) {
                    s.player.cellName = name;
                }
            }
            if (debug) logger::debug("BuildSnapshot: cell OK");
        }

        if (auto* cal = RE::Calendar::GetSingleton()) {
            s.player.gameDaysPassed = cal->GetDaysPassed();
            s.player.timeOfDayHours = cal->GetHour();
        }
        if (debug) logger::debug("BuildSnapshot: calendar OK");

        const auto mask = AlphaCanon::EvaluateAll();
        s.alphaCanonSignals       = AlphaCanon::Names(mask);
        s.alphaCanonSignalBitmask = static_cast<std::uint32_t>(mask);
        if (debug) logger::debug("BuildSnapshot: alphaCanon OK; returning");

        return s;
    }

    std::string BuildPromptContext(const Snapshot& /*snapshot*/)
    {
        // Step 10 — Phase B. Not yet implemented.
        return {};
    }

    DecisionLog::DecisionRecord ParseDecision(const std::string& /*jsonResponse*/,
                                              const Snapshot& snapshot)
    {
        // Step 14 — Phase D parser. For now, return a snapshot-seeded
        // record so callers (when wired) get something sensible.
        DecisionLog::DecisionRecord r;
        r.realTimeSec             = snapshot.realTimeSec;
        r.gameDaysPassed          = snapshot.player.gameDaysPassed;
        r.currentPhase            = PhaseTracker::Get();
        r.alphaCanonActiveSignals = snapshot.alphaCanonSignalBitmask;
        return r;
    }

    void ApplyDecision(const DecisionLog::DecisionRecord& /*record*/)
    {
        // Step 14 — Phase D applier. Not yet implemented.
    }

    void BeginEvaluation()
    {
        // Atomic guard: only one evaluation can be in flight at a time. If
        // a previous one is still being processed when the next tick fires
        // (LLM slow, worker backed up), this tick is silently dropped.
        bool expected = false;
        if (!g_inFlight.compare_exchange_strong(expected, true)) {
            if (Settings::Get().debugMode) {
                logger::debug("EvaluationPipeline: evaluation already in flight; skipping tick");
            }
            return;
        }

        Snapshot snapshot = BuildSnapshot();

        if (Settings::Get().debugMode) {
            LogSnapshot(snapshot);
        }

        // Steps 10/11/14 wire the worker dispatch + Phase B/C/D chain here.
        // Until then, end the evaluation immediately so the next tick isn't
        // blocked.
        g_inFlight.store(false);
    }
}
