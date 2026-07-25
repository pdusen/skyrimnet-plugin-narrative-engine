#pragma once

#include <mutex>
#include <string>
#include <utility>

namespace NarrativeEngine::BeatUtils
{
    // ComposeSubPhaseMachine<E> — a mutex-guarded sub-state enum plus
    // a paired failure-reason string. Each beat parameterizes it with
    // its own sub-phase enum (letter's has Start / ComposingLLM /
    // LLMResultReady / DispatchRequested / PollingSender /
    // PollingLetterRef / Succeeded / Failed; visit's is a smaller set
    // with the same Start / ComposingLLM / LLMResultReady /
    // Dispatching / Succeeded / Failed shape). The storage + get /
    // set / reset scaffolding is identical, so it lives here.
    //
    // Fail(failedPhase, reason) is a convenience that combines the
    // phase transition and the reason write in one lock scope, which
    // is what every failure site actually wants.
    template <typename E> class ComposeSubPhaseMachine
    {
    public:
        explicit ComposeSubPhaseMachine(E initial) noexcept : initial_(initial), phase_(initial) {}

        ComposeSubPhaseMachine(const ComposeSubPhaseMachine&) = delete;
        ComposeSubPhaseMachine& operator=(const ComposeSubPhaseMachine&) = delete;

        E Get() const
        {
            std::scoped_lock lock(mutex_);
            return phase_;
        }

        void Set(E phase)
        {
            std::scoped_lock lock(mutex_);
            phase_ = phase;
        }

        // Transition to `failedPhase` and store `reason` in one lock
        // scope. Called from any main-thread task that observes a
        // fatal failure.
        void Fail(E failedPhase, std::string reason)
        {
            std::scoped_lock lock(mutex_);
            phase_ = failedPhase;
            failureReason_ = std::move(reason);
        }

        std::string FailureReason() const
        {
            std::scoped_lock lock(mutex_);
            return failureReason_;
        }

        // Reset to the initial phase passed at construction and clear
        // the failure-reason string.
        void Reset()
        {
            std::scoped_lock lock(mutex_);
            phase_ = initial_;
            failureReason_.clear();
        }

    private:
        mutable std::mutex mutex_;
        E initial_;
        E phase_;
        std::string failureReason_;
    };
} // namespace NarrativeEngine::BeatUtils
