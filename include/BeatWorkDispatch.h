#pragma once

#include <PluginThread.h>

#include <functional>

// Dedicated single-threaded worker for beat->Tick bodies. Separate
// from BeatSystem's cadenced master poll so a Tick that blocks on
// MainThread::Run can't delay the next poll. Single-flighting is
// enforced by BeatSystem before each EnqueueWork.
namespace NarrativeEngine::BeatWorkDispatch
{
    // Idempotent. Call once at kDataLoaded.
    void Start();

    // Idempotent. Safe from any thread.
    void Stop();

    void EnqueueWork(std::function<void(const PluginThread::Token&)> work);
} // namespace NarrativeEngine::BeatWorkDispatch
