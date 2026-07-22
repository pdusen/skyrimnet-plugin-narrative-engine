#pragma once

#include <cstdint>

// Lightweight thread-local marker naming which of the three thread roles
// the current thread is executing under. Belt-and-braces layer beneath
// the compile-time token discipline in <MainThread.h> / <PluginThread.h>
// — the load-bearing enforcement is the tokens themselves, but the marker
// is useful for observability (log prefixes, debug traces) and for
// runtime assertions that catch bad-faith friend-forgery of the token
// types.
//
// Convention:
//   * ThreadRole::Foreign is the default for any thread NarrativeEngine
//     has not explicitly claimed. Skyrim's own event-dispatch pool and
//     SkyrimNet's callback-dispatch pool both start out as Foreign.
//   * ThreadRole::Main is set for the duration of a task the
//     MainThread::Run / MainThread::FireAndForget dispatchers push through
//     SKSE's task interface.
//   * ThreadRole::Plugin is set at the top of AsyncDispatch's worker
//     loop and BeatSystem's worker loop, and stays in place for the
//     thread's entire lifetime.
namespace NarrativeEngine
{
    enum class ThreadRole : std::uint8_t
    {
        Foreign = 0,
        Main,
        Plugin,
    };

    // Read the calling thread's currently-declared role.
    ThreadRole CurrentThreadRole() noexcept;

    // RAII helper — pushes a role on construction, restores the prior
    // role on destruction. Used by the dispatchers when marshaling work
    // onto the main thread, and by worker-loop entry points to declare
    // "this thread runs as Plugin for its entire lifetime."
    class ScopedThreadRole
    {
    public:
        explicit ScopedThreadRole(ThreadRole newRole) noexcept;
        ~ScopedThreadRole() noexcept;

        ScopedThreadRole(const ScopedThreadRole&) = delete;
        ScopedThreadRole& operator=(const ScopedThreadRole&) = delete;
        ScopedThreadRole(ScopedThreadRole&&) = delete;
        ScopedThreadRole& operator=(ScopedThreadRole&&) = delete;

    private:
        ThreadRole m_previous;
    };
} // namespace NarrativeEngine
