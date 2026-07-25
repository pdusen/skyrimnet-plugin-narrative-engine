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
        // Sole legitimate construction site for a Token in the
        // FireAndForget path.
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
