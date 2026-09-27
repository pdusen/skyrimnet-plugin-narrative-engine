#include <DashboardTimers.h>

#include <BeatRegistry.h>
#include <BeatSystem.h>
#include <EngineUtils.h>
#include <EventLogUtil.h>
#include <GossipTick.h>
#include <LetterPool.h>
#include <NPCLetterBeat.h>
#include <NPCVisitBeat.h>
#include <PhaseTracker.h>
#include <Settings.h>
#include <Tick.h>
#include <VisitConclusionPoll.h>

#include <algorithm>
#include <string>

namespace NarrativeEngine::DashboardTimers
{
    namespace
    {
        // Game-hours remaining on a named beat's own cooldown. Goes
        // through the registry rather than the concrete beat so a
        // disabled or unregistered beat reports "nothing pending"
        // instead of needing a special case at each call site.
        Timer BeatCooldown(std::string_view beatName)
        {
            auto* beat = BeatRegistry::Find(beatName);
            if (!beat) {
                return Inactive(Clock::GameTime);
            }
            const double remaining = beat->RemainingCooldownGameHours();
            return FromRemaining(remaining > 0.0, remaining, Clock::GameTime);
        }

        nlohmann::json PendingSummaryJson(const SenderCooldownTable::PendingSummary& summary)
        {
            return {
                {"count", summary.count},
                {"soonest", ToJson(FromRemaining(summary.count > 0, summary.soonestRemainingHours, Clock::GameTime))},
            };
        }

        nlohmann::json DirectorTimers()
        {
            const auto& cfg = Settings::Get();

            // The Director tick owns its own accumulator, so it reports
            // a remainder rather than an elapsed figure.
            const auto nextEvaluation =
                FromRemaining(Tick::IsEnabled(), Tick::SecondsUntilNextTick(), Clock::UnpausedRealTime);

            // Phase advance is gated on a floor, not an interval: the
            // phase may advance once it has run this long, and then
            // stays advanceable.
            const auto phaseAdvance = Remaining(static_cast<double>(std::max(0, cfg.minPhaseDurationSeconds)),
                                                static_cast<double>(PhaseTracker::TimeInPhaseSeconds()),
                                                Clock::UnpausedRealTime);

            const auto beatCooldown = Remaining(static_cast<double>(std::max(0, cfg.beatCooldownSeconds)),
                                                static_cast<double>(BeatSystem::GetGlobalCooldownMs()) / 1000.0,
                                                Clock::ActivePlayRealTime);

            const auto repetition = BeatSystem::GetRepetitionWindowInfo();

            return {
                {"next_evaluation", ToJson(nextEvaluation)},
                {"phase_advance_unlock", ToJson(phaseAdvance)},
                {"global_beat_cooldown", ToJson(beatCooldown)},
                {"repetition_window",
                 {
                     {"suppressed_count", repetition.suppressedCount},
                     {"soonest",
                      ToJson(FromRemaining(
                          repetition.suppressedCount > 0, repetition.soonestExpirySeconds, Clock::WallClock))},
                 }},
            };
        }

        nlohmann::json LetterTimers()
        {
            const auto& cfg = Settings::Get();

            // Slots waiting on the vanilla courier. deliveredAt is
            // Unix-epoch seconds, so this one is genuinely wall-clock:
            // the load-time demotion that consumes it compares against
            // the same clock and does not care what the player was
            // doing in between.
            nlohmann::json pending = nlohmann::json::array();
            const double nowUnix = EventLogUtil::NowUnixSeconds();
            const double timeout = static_cast<double>(std::max(0, cfg.letterPendingDeliveryTimeoutSeconds));
            for (const auto& slot : LetterPool::GetSlotSnapshots()) {
                if (slot.state != LetterPool::State::PendingDelivery || slot.deliveredAt <= 0.0) {
                    continue;
                }
                pending.push_back({
                    {"slot_index", static_cast<int>(slot.index)},
                    {"timeout", ToJson(Remaining(timeout, nowUnix - slot.deliveredAt, Clock::WallClock))},
                });
            }

            return {
                {"beat_cooldown", ToJson(BeatCooldown("npc_letter"))},
                {"sender_cooldowns", PendingSummaryJson(NPCLetterBeat_Cooldowns::SummarizeSenderCooldowns())},
                {"pending_delivery", std::move(pending)},
            };
        }

        nlohmann::json VisitTimers()
        {
            const auto timeouts = NPCVisitBeat_Timers::Get();
            const auto gate = VisitConclusionPoll::GetGateInfo();

            return {
                {"beat_cooldown", ToJson(BeatCooldown("npc_visit"))},
                {"sender_cooldowns", PendingSummaryJson(NPCVisitBeat_Cooldowns::SummarizeSenderCooldowns())},
                {"approach_timeout",
                 ToJson(FromRemaining(
                     timeouts.approachActive, timeouts.approachRemainingSeconds, Clock::ActivePlayRealTime))},
                {"return_home_timeout",
                 ToJson(FromRemaining(
                     timeouts.returnHomeActive, timeouts.returnHomeRemainingSeconds, Clock::ActivePlayRealTime))},
                // Three independent triggers, whichever trips first.
                // Collapsing them into one "next poll" figure would
                // have to pick a winner, and the turn counter is not a
                // clock at all.
                {"conclusion_poll",
                 {
                     {"armed", gate.armed},
                     {"silence",
                      ToJson(FromRemaining(
                          gate.armed && gate.silenceEnabled, gate.silenceRemainingSeconds, Clock::ActivePlayRealTime))},
                     {"interval",
                      ToJson(FromRemaining(gate.armed && gate.intervalEnabled,
                                           gate.intervalRemainingGameSeconds / 3600.0,
                                           Clock::GameTime))},
                     {"turns_remaining",
                      gate.armed && gate.turnsEnabled ? nlohmann::json(gate.turnsRemaining) : nlohmann::json(nullptr)},
                 }},
            };
        }

        nlohmann::json GossipTimers()
        {
            const auto schedule = GossipTick::GetScheduleInfo();
            const bool enabled = Settings::Get().gossipEnabled;

            // One scheduled tick is the harvest sweep AND the
            // simulation step -- RunTick sets the horizon, sweeps, then
            // advances -- so there is one game-time figure here rather
            // than the two the plan assumed.
            return {
                {"next_schedule_check",
                 ToJson(FromRemaining(enabled, schedule.secondsUntilNextCheck, Clock::UnpausedRealTime))},
                {"next_tick",
                 ToJson(
                     FromRemaining(enabled && schedule.hasNextDue, schedule.gameHoursUntilNextTick, Clock::GameTime))},
            };
        }
    } // namespace

    const char* ClockName(Clock clock)
    {
        switch (clock) {
        case Clock::WallClock:
            return "wall_clock";
        case Clock::UnpausedRealTime:
            return "unpaused_real";
        case Clock::ActivePlayRealTime:
            return "active_play_real";
        case Clock::GameTime:
            return "game_time";
        }
        return "wall_clock";
    }

    Timer Remaining(double configuredInterval, double elapsed, Clock clock)
    {
        Timer timer;
        timer.clock = clock;
        if (configuredInterval <= 0.0) {
            // Configured off. Distinct from "expired": a disabled
            // cooldown never blocks anything, where an expired one just
            // stopped blocking.
            return timer;
        }
        timer.active = true;
        const double remaining = configuredInterval - elapsed;
        timer.remaining = remaining > 0.0 ? remaining : 0.0;
        return timer;
    }

    Timer FromRemaining(bool enabled, double remaining, Clock clock)
    {
        Timer timer;
        timer.clock = clock;
        if (!enabled) {
            return timer;
        }
        timer.active = true;
        timer.remaining = remaining > 0.0 ? remaining : 0.0;
        return timer;
    }

    Timer Inactive(Clock clock)
    {
        Timer timer;
        timer.clock = clock;
        return timer;
    }

    nlohmann::json ToJson(const Timer& timer)
    {
        if (!timer.active) {
            return nullptr;
        }
        return {
            {"remaining", timer.remaining},
            {"clock", ClockName(timer.clock)},
        };
    }

    nlohmann::json Collect()
    {
        return {
            {"director", DirectorTimers()},
            {"letters", LetterTimers()},
            {"visit", VisitTimers()},
            {"gossip", GossipTimers()},
        };
    }
} // namespace NarrativeEngine::DashboardTimers
