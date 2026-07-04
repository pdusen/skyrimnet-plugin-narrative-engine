#pragma once

#include <cstdint>
#include <string>

#include <PhaseTracker.h>

#include <nlohmann/json_fwd.hpp>

namespace RE { class Actor; }

// IAction — the interface every Director-fireable action implements.
//
// The action toolbox is a flat registry of IAction instances. The
// ActionDispatcher filters the registry on every tick (situational
// availability + recency), asks the LLM to pick one of the survivors, and
// then calls Start() on the chosen action with the LLM-supplied parameters.
//
// Actions are long-running by default: Start() reports success-of-start, not
// success-of-completion. Completion arrives asynchronously through the
// shared `_ne_ActionCompleted` ModEvent (Papyrus → C++), which the
// dispatcher's sink consumes to clear in-flight state. Trivial actions that
// complete synchronously (e.g. a future internal-flag flip) should still fit
// this shape by sending the completion ModEvent at the end of Start.
namespace NarrativeEngine
{
    enum class ActionPolarity : std::uint8_t { Raise, Lower, Either };

    // Read-only snapshot of the world state an action needs to decide
    // availability and to execute. Built by the dispatcher just before each
    // IsAvailable / Start call; actions may not extend it.
    struct ActionContext
    {
        RE::Actor*  player           = nullptr;
        bool        playerInCombat   = false;
        bool        playerInDialogue = false;
        bool        playerInInterior = false;
        std::string locationName;   // current Location's display name, may be empty
        std::string cellName;       // current Cell's display name, may be empty

        // Which way the Director wants tension to move on this tick. Actions
        // whose Polarity is Either (e.g. NPCLetterAction) consume this to
        // shape their behavior; actions with a fixed polarity (Ambush =
        // Raise) ignore it. The dispatcher populates both fields from the
        // same values it already computed for the action-select prompt.
        PhaseTracker::Direction desiredDirection = PhaseTracker::Direction::Raise;
        int                     tensionDelta     = 0;
    };

    struct StartResult
    {
        // True when the action's start signal has been dispatched
        // successfully (e.g. the quest start ModEvent was sent and the
        // quest was confirmed running). Does NOT mean the action has
        // completed — completion arrives asynchronously via the
        // _ne_ActionCompleted ModEvent. False means the action could not
        // even begin (precondition changed, dependency missing, etc.).
        bool        started = false;
        std::string detail;   // one-line outcome description for the log
    };

    class IAction
    {
    public:
        virtual ~IAction() = default;

        // Stable snake_case identifier. Used as the value of
        // DecisionRecord::actionSelected, as the discriminator the LLM
        // returns in the selection response, and as the action-name field
        // the _ne_ActionCompleted ModEvent carries when this action
        // resolves. Never empty; never changes for a given action class.
        virtual std::string Name() const = 0;

        // One-paragraph description for the LLM. Read by the
        // action-select prompt template so the LLM understands what each
        // candidate does and when it's appropriate.
        virtual std::string Description() const = 0;

        virtual ActionPolarity Polarity() const = 0;

        // Cheap synchronous check: does current world state permit this
        // action to fire right now? Main thread. Called once per action
        // per tick to build the candidate manifest. Must be side-effect
        // free.
        virtual bool IsAvailable(const ActionContext& ctx) const = 0;

        // Start the action. Main thread. The action owns parameter
        // validation — unknown / missing / out-of-range fields should
        // fall back to defaults rather than abort. The action does NOT
        // block until completion; it kicks off whatever long-running
        // process it owns (quest start, ModEvent send, etc.) and returns.
        // The dispatcher tracks the in-flight state until the action
        // sends back _ne_ActionCompleted carrying this action's Name().
        virtual StartResult Start(const ActionContext& ctx,
                                  const nlohmann::json& parameters) = 0;

        // Poll, called from the dispatcher's main-thread tick driver
        // periodically while this action is in-flight. Returns true to
        // signal "the start visibly failed — the dispatcher should roll
        // back its in-flight bookkeeping so this action can be re-
        // selected on the next tick."
        //
        // Implementations that return true MUST also tear down any
        // engine-side state Start() brought up (stop the quest, disable
        // it, clear partial alias fills, etc.) BEFORE returning, so a
        // fresh Start() on the next attempt starts from a clean baseline.
        //
        // The dispatcher honors an internal grace period before it
        // begins calling this — actions don't need to early-return for
        // "just started" themselves, though an extra-defensive check
        // doesn't hurt.
        //
        // Default implementation always returns false: the only way out
        // of in-flight is the _ne_ActionCompleted ModEvent.
        virtual bool DetectAndRollbackFailedStart(const ActionContext& ctx,
                                                  double                secondsSinceStart)
        {
            (void)ctx;
            (void)secondsSinceStart;
            return false;
        }

        // Poll, called from the dispatcher's main-thread tick driver
        // periodically while this action is in-flight (after the
        // failed-start check). Returns true to signal "the action has
        // completed successfully — the dispatcher should clear in-flight
        // and apply the post-action cooldown."
        //
        // Implementations that return true MUST tear down any engine-side
        // state Start() brought up (stop + reset the quest, disable it,
        // etc.) BEFORE returning, so a future Start() begins from a clean
        // baseline.
        //
        // This is a complementary path to the _ne_ActionCompleted
        // ModEvent: actions whose Papyrus reliably sends that ModEvent
        // can ignore this hook; actions whose completion is observable
        // from engine state but doesn't reach Papyrus (e.g. quest reached
        // its "Complete Quest" stage without our script being involved)
        // can use this poll instead.
        //
        // Default implementation always returns false.
        virtual bool DetectCompletion(const ActionContext& ctx,
                                      double                secondsSinceStart)
        {
            (void)ctx;
            (void)secondsSinceStart;
            return false;
        }

        // In-game hours remaining before the action's own cooldown
        // expires. Zero means "no cooldown active" / "can fire now".
        // Actions with no per-action cooldown inherit the default
        // zero. Read-only, main thread, side-effect free — the
        // dashboard queries this every state push, and the
        // action-select pipeline never touches it (cooldowns are
        // already enforced inside IsAvailable).
        virtual double RemainingCooldownGameHours() const { return 0.0; }
    };
}
