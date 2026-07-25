#pragma once

#include <functional>

// Zero-sized proof-of-plugin-thread. See docs/THREADING_MODEL.md for
// the full contract. Unforgeable outside JobDispatcher; the only way
// to hold one is to be inside a lambda passed to a sanctioned
// dispatcher (AsyncDispatch / EvalDispatch / BeatWorkDispatch / the
// BeatSystem master poll).
namespace NarrativeEngine::PluginThread
{
    class Token;

    namespace detail
    {
        // Header-only so every plugin worker can invoke it without
        // depending on one particular translation unit.
        struct JobDispatcher
        {
            static inline void Invoke(const std::function<void(const Token&)>& job);
        };
    } // namespace detail

    class Token
    {
    public:
        // Non-copyable / non-movable so the reference the dispatcher
        // hands out cannot escape the callee.
        Token(const Token&) = delete;
        Token& operator=(const Token&) = delete;
        Token(Token&&) = delete;
        Token& operator=(Token&&) = delete;

    private:
        Token() = default;

        friend struct detail::JobDispatcher;
    };

    inline void detail::JobDispatcher::Invoke(const std::function<void(const Token&)>& job)
    {
        Token token;
        job(token);
    }
} // namespace NarrativeEngine::PluginThread
