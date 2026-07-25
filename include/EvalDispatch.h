#pragma once

#include <PluginThread.h>

#include <functional>

// Dedicated worker for the Director's per-tick LLM round-trip so it
// can't stall AsyncDispatch's cadenced queue.
namespace NarrativeEngine::EvalDispatch
{
    // Idempotent. Call once at kDataLoaded.
    void Start();

    // Idempotent. Safe from any thread.
    void Stop();

    void EnqueueWork(std::function<void(const PluginThread::Token&)> work);
} // namespace NarrativeEngine::EvalDispatch
