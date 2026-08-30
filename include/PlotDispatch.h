#pragma once

#include <PlotThread.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>

// The plot simulation's own worker thread.
//
// Fourth instance of a pattern the plugin already runs three times:
// a serial FIFO queue with one dedicated thread, so a subsystem whose
// work is long or unbounded cannot delay the cadenced AsyncDispatch
// queue. EvalDispatch and GossipDispatch exist for exactly this reason
// and plots are the same case: a tick may make several blocking LLM
// round trips (birth, adaptation, and a memory composition per step
// transition) and must not sit in a queue anything cadenced shares.
//
// One job at a time, in enqueue order. Because the only things enqueued
// here are later plot work, a job may block for as long as it needs to
// — including on a synchronous LLM round trip — without any other
// subsystem noticing. That is why plot birth and adaptation need no
// async continuation chain.
//
// The worker declares ThreadRole::Plugin, like the other workers. This
// is a narrower capability inside the plugin role, not a fifth role.
namespace NarrativeEngine::PlotDispatch
{
    // Cooperative cancellation for queued and running work.
    //
    // Loading a save cancels every outstanding token. A queued job that
    // finds itself cancelled does nothing at all; a running job checks
    // at each operation boundary, discards what it has accumulated, and
    // returns without publishing.
    //
    // Checking only at the END of a job would keep our own state
    // consistent — the results would be dropped and the loaded state
    // would stand — and would still be wrong, for two reasons that both
    // bite harder here than they do for gossip. A plot tick writes
    // memories into SkyrimNet's database, which lives outside our
    // co-save and is NOT rolled back by loading an earlier game; and it
    // mutates inventories and relationship ranks, which are engine
    // state the load likewise does not un-write on our behalf. A tick
    // that keeps running past a load leaves both behind, describing a
    // plot the loaded world has no record of. Cancelling at operation
    // boundaries is what stops those writes rather than merely
    // disowning them afterwards.
    class CancellationToken
    {
    public:
        [[nodiscard]] bool IsCancelled() const noexcept
        {
            return m_cancelled.load(std::memory_order_acquire);
        }

        void Cancel() noexcept
        {
            m_cancelled.store(true, std::memory_order_release);
        }

    private:
        std::atomic<bool> m_cancelled{false};
    };

    using CancellationHandle = std::shared_ptr<CancellationToken>;

    // Idempotent. Call once at kDataLoaded.
    void Start();

    // Idempotent. Safe from any thread.
    //
    // Cancels every outstanding token BEFORE joining, so a job blocked
    // in an LLM call unwinds at its next checkpoint instead of holding
    // shutdown for the full request timeout.
    void Stop();

    // Enqueue work with no cancellation handle of its own. For jobs
    // short enough that abandoning them would cost more than finishing
    // them.
    void EnqueueWork(std::function<void(const PlotThread::Token&)> work);

    // Enqueue cancellable work. The returned handle is registered so
    // that CancelAll reaches it, and is released when the job
    // completes.
    //
    // The job receives its own handle: it is expected to poll
    // IsCancelled() at every operation boundary and return early. A
    // scheduled plot tick is always enqueued through this overload —
    // see PlotTick — because every one of them can write outside our
    // custody.
    CancellationHandle EnqueueCancellableWork(
        std::function<void(const PlotThread::Token&, const CancellationHandle&)> work);

    // Cancel every queued and running job. Called from OnLoad and
    // OnRevert — the world those jobs were computing no longer exists.
    //
    // Safe from any thread, and deliberately does NOT wait: the caller
    // (SKSE's serialisation thread) never blocks on this one.
    void CancelAll();

    // Jobs currently queued or running. The scheduler uses this to cap
    // how far the backlog may grow (iPlotMaxOutstandingTicks).
    std::size_t OutstandingCount();
} // namespace NarrativeEngine::PlotDispatch
