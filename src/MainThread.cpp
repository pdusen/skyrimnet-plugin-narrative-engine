#include <MainThread.h>

#include <logger.h>
#include <ThreadRole.h>

#include <SKSE/SKSE.h>

#include <exception>
#include <utility>

namespace NarrativeEngine::MainThread
{
    namespace detail
    {
        // Sole legitimate invoker of a FireAndForget lambda with a
        // freshly-constructed Token. Token is deleted-copy /
        // deleted-move, so it can only exist as a local in a friend
        // context; the dispatcher constructs it in-place here and hands
        // it into the caller's lambda by reference-that-cannot-escape.
        struct FireAndForgetDispatcher
        {
            static void Invoke(const std::function<void(const Token&)>& fn)
            {
                Token token;
                fn(token);
            }
        };
    } // namespace detail

    void FireAndForget(const PluginThread::Token&, std::function<void(const Token&)> fn)
    {
        if (!fn) {
            return;
        }

        auto* taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            logger::warn("MainThread::FireAndForget: SKSE task interface unavailable; dropping task");
            return;
        }

        taskInterface->AddTask([fn = std::move(fn)]() mutable {
            // Install the Main role marker for the duration of the
            // task. Runtime belt-and-braces — the compile-time barrier
            // via PluginThread::Token is the primary enforcement, but
            // the marker is useful for observability and for the fast-
            // path assertion in MainThread::Run.
            ScopedThreadRole roleGuard(ThreadRole::Main);

            try {
                detail::FireAndForgetDispatcher::Invoke(fn);
            } catch (const std::exception& e) {
                logger::error("MainThread::FireAndForget: task threw: {}", e.what());
            } catch (...) {
                logger::error("MainThread::FireAndForget: task threw unknown exception");
            }
        });
    }
} // namespace NarrativeEngine::MainThread
