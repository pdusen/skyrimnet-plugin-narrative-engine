#pragma once

#include <PluginThread.h>

#include <functional>

// Dedicated single-threaded worker for the Director's per-tick
// evaluation. Distinct from AsyncDispatch's worker so that the
// synchronous LLM round-trip inside EvaluationPipeline::BeginEvaluation
// (typically a few seconds; occasionally tens of seconds when the
// upstream API is slow) does not stall the cadenced 500 ms poll body
// that runs on AsyncDispatch's worker.
//
// Both workers hand out PluginThread::Token, so every plugin-thread
// function is callable from either. Coordination between them is the
// same story as it was between AsyncDispatch and BeatSystem's own
// worker: shared state is protected by mutexes / atomics, and single-
// flighting (EvaluationPipeline::g_inFlight, BeatSystem's
// g_topLevelState) prevents overlap on the specific paths that need it.
namespace NarrativeEngine::EvalDispatch
{
    // Start the evaluation worker thread. Idempotent — second call is
    // a no-op. Call once from SKSE's kDataLoaded message, alongside
    // AsyncDispatch::Start.
    void Start();

    // Signal the worker to drain and exit, then join. Idempotent.
    // Safe to call from any thread.
    void Stop();

    // Queue `work` onto the evaluation worker. Returns immediately.
    // If the worker is not started, the task is dropped with a warning.
    //
    // The lambda receives a PluginThread::Token freshly constructed
    // by the shared JobDispatcher when the job pops off the queue.
    // Functionally identical to AsyncDispatch::EnqueueWork; the
    // separation exists purely so long-running work doesn't stall
    // AsyncDispatch's cadenced queue.
    void EnqueueWork(std::function<void(const PluginThread::Token&)> work);
} // namespace NarrativeEngine::EvalDispatch
