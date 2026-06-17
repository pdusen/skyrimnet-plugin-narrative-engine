#include <PhaseTracker.h>

#include <logger.h>

#include <array>
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
        constexpr std::uint32_t kRecordVersion = 1;

        std::mutex g_mutex;
        Phase     g_phase = Phase::Exposition;
        float     g_timeInPhaseSeconds = 0.0f;
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

    std::optional<Phase> NextPhase(Phase p)
    {
        const auto idx = static_cast<std::size_t>(p);
        if (idx + 1 >= kPhaseNames.size()) {
            // Resolution (or invalid) — no further advancement.
            return std::nullopt;
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
        return g_timeInPhaseSeconds;
    }

    void AdvanceTo(Phase newPhase)
    {
        Phase previous;
        {
            std::scoped_lock lock(g_mutex);
            previous = g_phase;
            g_phase = newPhase;
            g_timeInPhaseSeconds = 0.0f;
        }
        logger::info("PhaseTracker: advanced {} -> {}", PhaseName(previous), PhaseName(newPhase));
    }

    void Reset(Phase initial)
    {
        std::scoped_lock lock(g_mutex);
        g_phase = initial;
        g_timeInPhaseSeconds = 0.0f;
    }

    void Tick(float dtSeconds)
    {
        // Real-time clock: pause states (menus, console, dialogue) freeze it.
        // RE::UI::GameIsPaused() is the same predicate the rest of the engine
        // uses for "time should not advance right now."
        if (auto* ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) {
            return;
        }
        if (dtSeconds <= 0.0f) {
            return;
        }
        std::scoped_lock lock(g_mutex);
        g_timeInPhaseSeconds += dtSeconds;
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
        {
            std::scoped_lock lock(g_mutex);
            phaseByte    = static_cast<std::uint8_t>(g_phase);
            timeSnapshot = g_timeInPhaseSeconds;
        }
        intfc->WriteRecordData(phaseByte);
        intfc->WriteRecordData(timeSnapshot);
    }

    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length)
    {
        if (!intfc) {
            return;
        }
        if (version != kRecordVersion) {
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

        {
            std::scoped_lock lock(g_mutex);
            g_phase              = static_cast<Phase>(phaseByte);
            g_timeInPhaseSeconds = timeSeconds;
        }
        logger::info("PhaseTracker::OnLoad: restored phase={} timeInPhase={}s",
                     PhaseName(static_cast<Phase>(phaseByte)), timeSeconds);
    }

    void OnRevert()
    {
        Reset();
    }
}
