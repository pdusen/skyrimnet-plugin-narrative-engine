#pragma once

#include <PluginThread.h>

#include <functional>

// Cadenced short-work plugin worker. The sole plugin entry point that
// requires no token from the caller — foreign-thread code (SkyrimNet
// callbacks, engine event sinks) enters plugin-thread land through
// EnqueueWork.
namespace NarrativeEngine::AsyncDispatch
{
    // Idempotent. Call once at kDataLoaded.
    void Start();

    // Idempotent. Safe from any thread.
    void Stop();

    void EnqueueWork(std::function<void(const PluginThread::Token&)> work);

    // Deprecated: prefer MainThread::FireAndForget once you hold a
    // PluginThread::Token.
    [[deprecated(
        "Use MainThread::FireAndForget after obtaining a PluginThread::Token via AsyncDispatch::EnqueueWork.")]]
    void MarshalToMainThread(std::function<void()> work);
} // namespace NarrativeEngine::AsyncDispatch
