#include <PhaseTracker.h>

#include <logger.h>
#include <Settings.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>

// Forward decls to avoid pulling the event-log headers into
// PhaseTracker.h. Keeps the dependency one-way (PhaseTracker.cpp →
// event logs) and out of every translation unit that includes
// PhaseTracker.h.
namespace NarrativeEngine::CombatEventLog
{
    void OnPhaseAdvanced();
}
namespace NarrativeEngine::WeatherEventLog
{
    void OnPhaseAdvanced();
}
namespace NarrativeEngine::TravelEventLog
{
    void OnPhaseAdvanced();
}

namespace NarrativeEngine::PhaseTracker
{
    namespace
    {
        constexpr std::array<const char*, static_cast<std::size_t>(Phase::Count)> kPhaseNames = {
            "Exposition",
            "RisingAction",
            "Climax",
            "FallingAction",
            "Resolution",
        };

        // Bumped on any wire-format change to the OnSave/OnLoad payload.
        // v1: phase byte + base-seconds float.
        // v2: ...plus phaseEnteredAtGameTime (double, cumulative game-sec
        //     since session epoch). Wire-compatible with v3 but the field's
        //     semantics changed — v2 saves are read into a discarded slot
        //     and the live filter starts uncutoff'd until the first advance.
        // v3: ...phaseEnteredAtRealTime (double, Unix-epoch seconds).
        constexpr std::uint32_t kRecordVersion = 3;

        using SteadyClock = std::chrono::steady_clock;

        std::mutex g_mutex;
        Phase g_phase = Phase::Exposition;
        float g_baseSeconds = 0.0f;
        SteadyClock::time_point g_lastSampleTime = SteadyClock::now();
        // Unix-epoch real-wall-clock seconds when the current phase was
        // entered. Updated by AdvanceTo / Reset; persisted across saves.
        // Compared against SkyrimNet event `localTime` field — both are
        // Unix-epoch real seconds, monotonic, and immune to the game-time
        // quirks that make `evt.gameTime` unsuitable for this comparison
        // (it's time-of-day, not cumulative, and the engine's calendar
        // value can be stale at kNewGame before the world finishes
        // initializing).
        double g_phaseEnteredAtRealTime = 0.0;

        // Helper: capture the current Unix-epoch real-time in seconds.
        // Matches SkyrimNet's per-event `localTime` field so the filter
        // can use direct numeric comparison.
        double CurrentRealTimeSecondsLocked()
        {
            return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        // Roll the elapsed wall-clock time since the last sample into the
        // accumulator (subject to GameIsPaused), then reset the anchor.
        // MUST be called with g_mutex held. Main thread only — touches RE::UI.
        void SampleLocked()
        {
            const auto now = SteadyClock::now();
            const float dt = std::chrono::duration<float>(now - g_lastSampleTime).count();
            g_lastSampleTime = now;

            if (dt <= 0.0f) {
                return;
            }
            if (auto* ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) {
                return;
            }
            g_baseSeconds += dt;
        }
    } // namespace

    const char* PhaseName(Phase p)
    {
        const auto idx = static_cast<std::size_t>(p);
        if (idx >= kPhaseNames.size()) {
            return "Unknown";
        }
        return kPhaseNames[idx];
    }

    std::optional<Phase> PhaseFromName(std::string_view name)
    {
        for (std::size_t i = 0; i < kPhaseNames.size(); ++i) {
            if (name == kPhaseNames[i]) {
                return static_cast<Phase>(i);
            }
        }
        return std::nullopt;
    }

    Phase NextPhase(Phase p)
    {
        const auto idx = static_cast<std::size_t>(p);
        // Cyclical: Resolution wraps back to Exposition. An out-of-range
        // input is clamped to the start of the cycle as a defensive fallback.
        if (idx + 1 >= kPhaseNames.size()) {
            return Phase::Exposition;
        }
        return static_cast<Phase>(idx + 1);
    }

    std::optional<Phase> EvaluateAdvance(Phase current, std::uint32_t tensionScore, float timeInPhaseSeconds)
    {
        const auto& cfg = Settings::Get();

        // Minimum-dwell floor: no phase transition until the current phase
        // has been active at least this many unpaused real-time seconds.
        // Applies uniformly across all phases and both threshold directions
        // (raise / lower). Separates "when is advancement even permitted"
        // from "when is a beat welcome to nudge things along" — the latter
        // lives in BeatSystem::ConsiderBeat under the per-phase ideal
        // durations.
        if (timeInPhaseSeconds < static_cast<float>(std::max(0, cfg.minPhaseDurationSeconds))) {
            return std::nullopt;
        }

        const int score = static_cast<int>(tensionScore);
        switch (current) {
        case Phase::Exposition:
            if (score >= cfg.advanceThresholdExposition)
                return Phase::RisingAction;
            return std::nullopt;
        case Phase::RisingAction:
            if (score >= cfg.advanceThresholdRisingAction)
                return Phase::Climax;
            return std::nullopt;
        case Phase::Climax:
            if (score <= cfg.advanceThresholdClimax)
                return Phase::FallingAction;
            return std::nullopt;
        case Phase::FallingAction:
            if (score <= cfg.advanceThresholdFallingAction)
                return Phase::Resolution;
            return std::nullopt;
        case Phase::Resolution:
            if (score >= cfg.advanceThresholdResolution)
                return Phase::Exposition;
            return std::nullopt;
        default:
            return std::nullopt;
        }
    }

    Direction OutgoingDirection(Phase p)
    {
        // Mirrors the threshold direction in EvaluateAdvance: rises out of
        // Exposition / RisingAction / Resolution; drops out of Climax /
        // FallingAction. Out-of-range inputs default to Raise (the most
        // common direction across the cycle).
        switch (p) {
        case Phase::Climax:
        case Phase::FallingAction:
            return Direction::Lower;
        case Phase::Exposition:
        case Phase::RisingAction:
        case Phase::Resolution:
        default:
            return Direction::Raise;
        }
    }

    Phase Get()
    {
        std::scoped_lock lock(g_mutex);
        return g_phase;
    }

    float TimeInPhaseSeconds()
    {
        std::scoped_lock lock(g_mutex);
        SampleLocked();
        return g_baseSeconds;
    }

    double PhaseEnteredAtRealTime()
    {
        std::scoped_lock lock(g_mutex);
        return g_phaseEnteredAtRealTime;
    }

    void AdvanceTo(Phase newPhase)
    {
        Phase previous;
        double newRealTime;
        {
            std::scoped_lock lock(g_mutex);
            previous = g_phase;
            g_phase = newPhase;
            g_baseSeconds = 0.0f;
            g_lastSampleTime = SteadyClock::now();
            // Mark the real-time moment this phase started so the
            // evaluation pipeline can filter SkyrimNet events to
            // "since this advance."
            g_phaseEnteredAtRealTime = CurrentRealTimeSecondsLocked();
            newRealTime = g_phaseEnteredAtRealTime;
        }
        // Notify event logs outside our mutex — each takes its own lock,
        // and we don't want to hold two at once.
        CombatEventLog::OnPhaseAdvanced();
        WeatherEventLog::OnPhaseAdvanced();
        TravelEventLog::OnPhaseAdvanced();
        logger::info("PhaseTracker: advanced {} -> {} (at realTime={:.1f})",
                     PhaseName(previous),
                     PhaseName(newPhase),
                     newRealTime);
    }

    void Reset(Phase initial)
    {
        {
            std::scoped_lock lock(g_mutex);
            g_phase = initial;
            g_baseSeconds = 0.0f;
            g_lastSampleTime = SteadyClock::now();
            // Anchor "phase started" to *now* so a fresh new-game state
            // filters events to "since now" (i.e. nothing yet). For
            // kPreLoadGame, this value gets immediately overwritten by
            // OnLoad's deserialized value.
            g_phaseEnteredAtRealTime = CurrentRealTimeSecondsLocked();
        }
        // Same notify-outside-mutex discipline as AdvanceTo.
        CombatEventLog::OnPhaseAdvanced();
        WeatherEventLog::OnPhaseAdvanced();
        TravelEventLog::OnPhaseAdvanced();
    }

    void Tick()
    {
        // Pause states (menus, console, dialogue) freeze the clock. The
        // pause check is sampled at the moment of this call — exactly the
        // same semantic as the rest of the engine's GameIsPaused gates.
        std::scoped_lock lock(g_mutex);
        SampleLocked();
    }

    void OnSave(SKSE::SerializationInterface* intfc)
    {
        if (!intfc) {
            return;
        }
        if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
            logger::error("PhaseTracker::OnSave: OpenRecord failed");
            return;
        }

        std::uint8_t phaseByte;
        float timeSnapshot;
        double phaseEnteredAtSnapshot;
        {
            std::scoped_lock lock(g_mutex);
            // Sample first so the on-disk value reflects time-in-phase as
            // of the moment of OnSave, not as of the last Tick.
            SampleLocked();
            phaseByte = static_cast<std::uint8_t>(g_phase);
            timeSnapshot = g_baseSeconds;
            phaseEnteredAtSnapshot = g_phaseEnteredAtRealTime;
        }
        intfc->WriteRecordData(phaseByte);
        intfc->WriteRecordData(timeSnapshot);
        intfc->WriteRecordData(phaseEnteredAtSnapshot);
    }

    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length)
    {
        if (!intfc) {
            return;
        }
        if (version != 1 && version != 2 && version != 3) {
            logger::warn(
                "PhaseTracker::OnLoad: unrecognized record version {} (length={}); using defaults", version, length);
            Reset();
            return;
        }

        std::uint8_t phaseByte = 0;
        float timeSeconds = 0.0f;
        if (intfc->ReadRecordData(phaseByte) != sizeof(phaseByte)
            || intfc->ReadRecordData(timeSeconds) != sizeof(timeSeconds)) {
            logger::error("PhaseTracker::OnLoad: short read; using defaults");
            Reset();
            return;
        }
        if (phaseByte >= static_cast<std::uint8_t>(Phase::Count)) {
            logger::error("PhaseTracker::OnLoad: invalid phase byte {}; resetting to Exposition", phaseByte);
            Reset();
            return;
        }

        // v1: no trailing field — leave phaseEnteredAtRealTime at 0 (filter
        //     allows everything until the next advance/reset writes it).
        // v2: trailing field is a cumulative-game-seconds value, which no
        //     longer matches the live filter's units. Read it to consume
        //     the bytes, then discard — same 0 fallback as v1.
        // v3: trailing field is Unix-epoch real-seconds — what we want.
        double phaseEnteredAtRealTime = 0.0;
        if (version == 2) {
            double discarded = 0.0;
            if (intfc->ReadRecordData(discarded) != sizeof(discarded)) {
                logger::error("PhaseTracker::OnLoad: short read on v2 legacy field; defaulting to 0");
            }
        } else if (version >= 3) {
            if (intfc->ReadRecordData(phaseEnteredAtRealTime) != sizeof(phaseEnteredAtRealTime)) {
                logger::error("PhaseTracker::OnLoad: short read on phaseEnteredAtRealTime; defaulting to 0");
                phaseEnteredAtRealTime = 0.0;
            }
        }

        {
            std::scoped_lock lock(g_mutex);
            g_phase = static_cast<Phase>(phaseByte);
            g_baseSeconds = timeSeconds;
            g_phaseEnteredAtRealTime = phaseEnteredAtRealTime;
            // Re-anchor the sample clock to "now" so subsequent Tick /
            // OnSave / TimeInPhaseSeconds calls add real time on top of the
            // loaded base, rather than counting elapsed time from before
            // the load.
            g_lastSampleTime = SteadyClock::now();
        }
        logger::info("PhaseTracker::OnLoad: restored phase={} timeInPhase={}s phaseEnteredAtRealTime={:.1f}",
                     PhaseName(static_cast<Phase>(phaseByte)),
                     timeSeconds,
                     phaseEnteredAtRealTime);
    }

    void OnRevert()
    {
        Reset();
    }
} // namespace NarrativeEngine::PhaseTracker
