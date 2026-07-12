#pragma once

#include <functional>

// A single worker thread + a thin SKSE-task-interface bridge for hopping
// back to the main game thread. The Director's three-phase async shape
// (Phase A snapshot on main → Phase B/C work on worker → Phase D apply on
// main) is built on these two primitives.
namespace NarrativeEngine::AsyncDispatch
{
    // Start the worker thread. Idempotent — second call is a no-op.
    // Call once from SKSE's kDataLoaded message.
    void Start();

    // Signal the worker to drain and exit, then join. Idempotent. Safe to
    // call from any thread. Not strictly required at process exit, but
    // declared for symmetry and useful for tests.
    void Stop();

    // Queue `work` to run on the worker thread. Returns immediately. If the
    // worker is not started, the task is dropped with a warning.
    void EnqueueWork(std::function<void()> work);

    // Schedule `work` to run on Skyrim's main thread via SKSE's task
    // interface. Returns immediately. Safe from any thread.
    void MarshalToMainThread(std::function<void()> work);
} // namespace NarrativeEngine::AsyncDispatch
