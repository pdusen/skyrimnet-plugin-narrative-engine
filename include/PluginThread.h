#pragma once

#include <functional>

// Zero-sized proof-of-plugin-thread. Every NarrativeEngine plugin
// function that is not itself a sanctioned foreign entry point
// (AsyncDispatch::EnqueueWork or EvalDispatch::EnqueueWork) takes
// either a PluginThread::Token or a MainThread::Token as an argument.
// Foreign-thread code, holding neither, is structurally locked out
// of the plugin surface.
//
// The Token is unforgeable outside the shared JobDispatcher below.
// The only way for code to hold one is to be inside a lambda passed
// to one of the sanctioned dispatchers (AsyncDispatch::EnqueueWork
// runs its lambda on the cadenced short-work worker; EvalDispatch::
// EnqueueWork runs its lambda on the Director-evaluation worker so
// long-running LLM waits don't stall the cadenced queue). Both
// workers construct their token through the shared dispatcher below.
namespace NarrativeEngine::PluginThread
{
    class Token;

    namespace detail
    {
        // Sole legitimate invoker of a plugin-thread job. Each
        // dispatcher (AsyncDispatch's cadenced worker, EvalDispatch's
        // long-work worker) hands the caller's std::function into
        // Invoke, which constructs a fresh Token in its own stack
        // frame and passes it into the lambda by const-reference.
        // Token's copy/move are deleted so the reference cannot
        // escape the callee.
        //
        // Header-only so both workers can call it without the
        // definition living in one particular translation unit.
        struct JobDispatcher
        {
            static inline void Invoke(const std::function<void(const Token&)>& job);
        };
    } // namespace detail

    class Token
    {
    public:
        // Non-copyable, non-movable — same reasoning as
        // MainThread::Token.
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
