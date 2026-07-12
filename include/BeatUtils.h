#pragma once

#include <AsyncDispatch.h>

#include <atomic>
#include <mutex>
#include <string>
#include <utility>

// BeatUtils — small header-only helpers for state-machine scaffolding
// shared across beats. Neither helper here touches the engine directly;
// both are pure C++ synchronization primitives plus (in CleanupLatch's
// case) a marshal into AsyncDispatch.
namespace NarrativeEngine::BeatUtils
{
    // CleanupLatch — the "fire a one-shot main-thread cleanup task
    // from Tick(CLEANUP), wait for it to signal done, then transition
    // to NOT_RUNNING" pattern.
    //
    // Usage:
    //   BeatUtils::CleanupLatch g_cleanup;
    //   ...
    //   void ResetSessionState() { ...; g_cleanup.Reset(); }
    //
    //   void MainThreadCleanup() {
    //       ... do teardown ...
    //       g_cleanup.MarkComplete();
    //   }
    //
    //   TickResult Tick(...) {
    //       ...
    //       case BeatState::CLEANUP:
    //           if (g_cleanup.Poll(&MainThreadCleanup)) {
    //               return {BeatState::NOT_RUNNING};
    //           }
    //           return {};
    //   }
    //
    // Poll returns true only once — after MarkComplete has been called.
    // First Poll marshals the task; subsequent Polls before completion
    // are cheap no-ops.
    class CleanupLatch
    {
    public:
        CleanupLatch() = default;
        CleanupLatch(const CleanupLatch&) = delete;
        CleanupLatch& operator=(const CleanupLatch&) = delete;

        // Beat's Tick(CLEANUP) arm calls this. Returns true when the
        // marshaled cleanup task has completed (Tick should transition
        // to NOT_RUNNING). Otherwise, if the task hasn't been fired
        // yet, marshals it to the main thread; the task MUST call
        // MarkComplete() at its end.
        template <typename Task> bool Poll(Task&& task)
        {
            if (complete_.load(std::memory_order_acquire))
                return true;
            if (!fired_.exchange(true, std::memory_order_acq_rel)) {
                AsyncDispatch::MarshalToMainThread(std::forward<Task>(task));
            }
            return false;
        }

        // Called from the marshaled cleanup task's tail.
        void MarkComplete()
        {
            complete_.store(true, std::memory_order_release);
        }

        // Called from the beat's session-reset path.
        void Reset()
        {
            fired_.store(false, std::memory_order_release);
            complete_.store(false, std::memory_order_release);
        }

    private:
        std::atomic<bool> fired_{false};
        std::atomic<bool> complete_{false};
    };

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
