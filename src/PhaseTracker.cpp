#include <PhaseTracker.h>

#include <logger.h>

#include <array>
#include <chrono>
#include <mutex>

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
        // v2: ...plus phaseEnteredAtGameTime (double).
        constexpr std::uint32_t kRecordVersion = 2;

        using SteadyClock = std::chrono::steady_clock;

        std::mutex            g_mutex;
        Phase                 g_phase              = Phase::Exposition;
        float                 g_baseSeconds        = 0.0f;
        SteadyClock::time_point g_lastSampleTime   = SteadyClock::now();
        // Cumulative game-seconds-since-session-epoch when the current phase
        // was entered. Updated by AdvanceTo / Reset; persisted across saves.
        double                g_phaseEnteredAtGameTime = 0.0;

        // Helper: capture the current game time in seconds from Calendar.
        // SkyrimNet's per-event `gameTime` field uses the same units, so
        // direct numeric comparisons work. Returns 0.0 if Calendar isn't
        // available (e.g. very early init), which means "no filter."
        double CurrentGameTimeSecondsLocked()
        {
            if (auto *cal = RE::Calendar::GetSingleton()) {
                return static_cast<double>(cal->GetDaysPassed()) * 86400.0;
            }
            return 0.0;
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
    }

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

    double PhaseEnteredAtGameTime()
    {
        std::scoped_lock lock(g_mutex);
        return g_phaseEnteredAtGameTime;
    }

    void AdvanceTo(Phase newPhase)
    {
        Phase  previous;
        double newGameTime;
        {
            std::scoped_lock lock(g_mutex);
            previous                 = g_phase;
            g_phase                  = newPhase;
            g_baseSeconds            = 0.0f;
            g_lastSampleTime         = SteadyClock::now();
            // Mark the in-game moment this phase started so the evaluation
            // pipeline can filter SkyrimNet events to "since this advance."
            g_phaseEnteredAtGameTime = CurrentGameTimeSecondsLocked();
            newGameTime              = g_phaseEnteredAtGameTime;
        }
        logger::info("PhaseTracker: advanced {} -> {} (at gameTime={:.1f}s)",
                     PhaseName(previous), PhaseName(newPhase), newGameTime);
    }

    void Reset(Phase initial)
    {
        std::scoped_lock lock(g_mutex);
        g_phase              = initial;
        g_baseSeconds        = 0.0f;
        g_lastSampleTime     = SteadyClock::now();
        // Anchor "phase started" to *now* so a fresh new-game state filters
        // events to "since now" (i.e. nothing yet). For kPreLoadGame, this
        // value gets immediately overwritten by OnLoad's deserialized value.
        g_phaseEnteredAtGameTime = CurrentGameTimeSecondsLocked();
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
        float        timeSnapshot;
        double       phaseEnteredAtSnapshot;
        {
            std::scoped_lock lock(g_mutex);
            // Sample first so the on-disk value reflects time-in-phase as
            // of the moment of OnSave, not as of the last Tick.
            SampleLocked();
            phaseByte              = static_cast<std::uint8_t>(g_phase);
            timeSnapshot           = g_baseSeconds;
            phaseEnteredAtSnapshot = g_phaseEnteredAtGameTime;
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
        if (version != 1 && version != 2) {
            logger::warn("PhaseTracker::OnLoad: unrecognized record version {} (length={}); using defaults",
                         version, length);
            Reset();
            return;
        }

        std::uint8_t phaseByte = 0;
        float        timeSeconds = 0.0f;
        if (intfc->ReadRecordData(phaseByte) != sizeof(phaseByte) ||
            intfc->ReadRecordData(timeSeconds) != sizeof(timeSeconds)) {
            logger::error("PhaseTracker::OnLoad: short read; using defaults");
            Reset();
            return;
        }
        if (phaseByte >= static_cast<std::uint8_t>(Phase::Count)) {
            logger::error("PhaseTracker::OnLoad: invalid phase byte {}; resetting to Exposition", phaseByte);
            Reset();
            return;
        }

        // v2 added phaseEnteredAtGameTime (double). For v1 saves, default to
        // 0.0, which means "all events visible" — safe fallback that may
        // overcount events for one tick after load but doesn't break.
        double phaseEnteredAt = 0.0;
        if (version >= 2) {
            if (intfc->ReadRecordData(phaseEnteredAt) != sizeof(phaseEnteredAt)) {
                logger::error("PhaseTracker::OnLoad: short read on phaseEnteredAtGameTime; defaulting to 0");
                phaseEnteredAt = 0.0;
            }
        }

        {
            std::scoped_lock lock(g_mutex);
            g_phase                  = static_cast<Phase>(phaseByte);
            g_baseSeconds            = timeSeconds;
            g_phaseEnteredAtGameTime = phaseEnteredAt;
            // Re-anchor the sample clock to "now" so subsequent Tick /
            // OnSave / TimeInPhaseSeconds calls add real time on top of the
            // loaded base, rather than counting elapsed time from before
            // the load.
            g_lastSampleTime = SteadyClock::now();
        }
        logger::info("PhaseTracker::OnLoad: restored phase={} timeInPhase={}s phaseEnteredAtGameTime={:.1f}s",
                     PhaseName(static_cast<Phase>(phaseByte)), timeSeconds, phaseEnteredAt);
    }

    void OnRevert()
    {
        Reset();
    }
}
