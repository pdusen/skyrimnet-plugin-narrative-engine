#pragma once

#include <PluginThread.h>

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
    //
    // This is the sole plugin API that does not require a token from the
    // caller — it's the entry point foreign-thread code (SkyrimNet
    // callbacks, engine event sinks) uses to enter plugin-thread land
    // in the first place. The lambda receives a PluginThread::Token
    // freshly constructed by the dispatcher when the job pops off the
    // queue and begins running on the worker.
    void EnqueueWork(std::function<void(const PluginThread::Token&)> work);

    // Schedule `work` to run on Skyrim's main thread via SKSE's task
    // interface. Returns immediately. Safe from any thread.
    //
    // Deprecated in favour of MainThread::FireAndForget, which requires
    // a PluginThread::Token proving the caller is on the plugin thread.
    // The old symbol stays as a passthrough for the transitional grace
    // period so the audit-fix follow-on phase can migrate call sites
    // individually as it touches them.
    [[deprecated(
        "Use MainThread::FireAndForget after obtaining a PluginThread::Token via AsyncDispatch::EnqueueWork.")]]
    void MarshalToMainThread(std::function<void()> work);
} // namespace NarrativeEngine::AsyncDispatch
