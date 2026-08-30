#pragma once

#include <functional>

// Zero-sized proof-of-plot-thread. The exact shape of
// PluginThread::Token and GossipThread::Token, minted by a different
// dispatcher, and that difference is the whole point.
//
// The plot simulation owns its mutable state outright: plots, steps,
// the occupancy table, the RNG stream. Nothing outside PlotDispatch's
// worker may touch it, which is what lets PlotState carry no mutex at
// all. A separate token turns that from a convention into a signature —
// a function taking PlotThread::Token const& cannot be called from
// AsyncDispatch, from the gossip worker, from a SkyrimNet callback, or
// from the main thread, because none of them can produce one.
//
// See docs/implementation/PHASE_14_FACTION_PLOTS.md for the phase this
// belongs to and docs/THREADING_MODEL.md for the roles this sits
// inside. Note that the plot worker still declares ThreadRole::Plugin:
// this is a narrower CAPABILITY within the plugin role, not a fifth
// role.
namespace NarrativeEngine::PlotThread
{
    class Token;

    namespace detail
    {
        // Header-only, mirroring PluginThread::detail::JobDispatcher and
        // GossipThread's, so the worker loop can invoke it without
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
} // namespace NarrativeEngine::PlotThread
