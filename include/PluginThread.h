#pragma once

// Zero-sized proof-of-plugin-thread. Every NarrativeEngine plugin
// function that is not itself the sole "foreign entry point"
// (AsyncDispatch::EnqueueWork) takes either a PluginThread::Token or a
// MainThread::Token as an argument. Foreign-thread code, holding
// neither, is structurally locked out of the plugin surface.
//
// The Token is unforgeable outside AsyncDispatch's job dispatcher. The
// only way for code to hold one is to be inside a lambda passed to
// AsyncDispatch::EnqueueWork — which by construction runs on the
// plugin-owned worker thread.
namespace NarrativeEngine::PluginThread
{
    namespace detail
    {
        // Forward-declared here; defined in src/AsyncDispatch.cpp when
        // Step 4 rewires EnqueueWork to construct the token per job.
        struct JobDispatcher;
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
} // namespace NarrativeEngine::PluginThread
