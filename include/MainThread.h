#pragma once

#include <logger.h>
#include <PluginThread.h>
#include <ThreadRole.h>

#include <SKSE/SKSE.h>

#include <cassert>
#include <exception>
#include <functional>
#include <future>
#include <type_traits>

// Zero-sized proof-of-main-thread. Every sanctioned engine-touching
// wrapper takes one of these as an argument, and the type is unforgeable
// outside the two dispatchers below. The only way for code to hold a
// MainThread::Token is to be inside the lambda of a MainThread::Run or
// MainThread::FireAndForget call — which by construction runs on the
// main thread.
namespace NarrativeEngine::MainThread
{
    namespace detail
    {
        // Defined in src/MainThread.cpp — each primitive owns its own
        // dispatcher so friend access stays minimal.
        struct FireAndForgetDispatcher;
        struct RunDispatcher;
    } // namespace detail

    class Token
    {
    public:
        // Non-copyable, non-movable. Copyability would let a lambda
        // stash a token into a member and satisfy a token-taking
        // signature it shouldn't be able to; non-movability closes the
        // same loophole via std::move. Intended lifetime: exactly one
        // dispatcher invocation.
        Token(const Token&) = delete;
        Token& operator=(const Token&) = delete;
        Token(Token&&) = delete;
        Token& operator=(Token&&) = delete;

    private:
        // Only the sanctioned dispatchers may construct a Token.
        Token() = default;

        friend struct detail::FireAndForgetDispatcher;
        friend struct detail::RunDispatcher;
    };

    // Fire-and-forget marshaling onto the main thread.
    //
    // The caller must supply a PluginThread::Token proving they are on
    // the plugin thread. Foreign-thread callers, main-thread callers,
    // and re-entrant callers from inside another marshaled lambda all
    // fail to compile because they have no PluginThread::Token to pass
    // — the only way to obtain one is to be inside an
    // AsyncDispatch::EnqueueWork job.
    //
    // `fn` receives a freshly-minted MainThread::Token when it runs on
    // the main thread, which it can pass into engine-touching wrappers.
    // Exceptions thrown by `fn` are caught and logged; they do not
    // propagate back to the caller (there's no caller left by the time
    // `fn` runs).
    //
    // Both token parameters are passed by const reference because
    // Token is deliberately non-copyable and non-movable — the tokens
    // are proof-of-context tags, not values to be transported.
    //
    // If SKSE's task interface is unavailable (e.g. plugin is still
    // spinning up or has shut down), the task is dropped with a warning.
    void FireAndForget(const PluginThread::Token&, std::function<void(const Token&)> fn);

    namespace detail
    {
        // RunDispatcher owns the sole legitimate construction site for a
        // MainThread::Token in the Run<T> path. Kept in the header
        // (rather than the .cpp) because Run<T> is a template — the
        // dispatcher's Invoke must be visible where the template is
        // instantiated.
        struct RunDispatcher
        {
            template <typename Fn> static auto Invoke(Fn& fn) -> std::invoke_result_t<Fn, const Token&>
            {
                Token token;
                return fn(token);
            }
        };
    } // namespace detail

    // Blocking request/response marshaling.
    //
    // The caller must supply a PluginThread::Token proving they are on
    // the plugin thread. `fn` is submitted onto the main thread via
    // SKSE's task interface; the calling thread blocks on a future
    // until `fn` returns, then unblocks with `fn`'s return value.
    //
    // Compile-time misuse barriers:
    //   * Foreign threads: no PluginThread::Token to pass — build error.
    //   * Main-thread code: holds MainThread::Token, not
    //     PluginThread::Token — build error.
    //   * Re-entrant Run from inside another Run's lambda: the inner
    //     lambda holds MainThread::Token, not PluginThread::Token —
    //     build error.
    //
    // Runtime belt-and-braces: asserts the calling thread's role is
    // Plugin. Catches bad-faith friend-forgery in debug; degrades to
    // running `fn` inline (unsafe if actually on the wrong thread, but
    // preferable to crashing a player's game) in release.
    //
    // Exceptions thrown by `fn` propagate back to the caller via the
    // future's exception path — `future.get()` re-throws on the calling
    // thread. This is the intended shape: worker-thread code catches
    // exactly what it would have caught from a synchronous call.
    //
    // Deliberately no timeout parameter — a wedged main thread means
    // the game has stopped, and there is no useful recovery for that
    // condition.
    template <typename Fn> auto Run(const PluginThread::Token&, Fn&& fn) -> std::invoke_result_t<Fn, const Token&>
    {
        using ResultT = std::invoke_result_t<Fn, const Token&>;

        // Belt-and-braces: the compile-time barrier is the
        // PluginThread::Token signature requirement. The runtime check
        // catches only the pathological case where someone has
        // constructed a token through friend-hackery.
        assert(CurrentThreadRole() == ThreadRole::Plugin
               && "MainThread::Run called from a thread whose role is not Plugin — bad-faith token forgery?");

        auto* taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            logger::error("MainThread::Run: SKSE task interface unavailable; returning default result");
            if constexpr (std::is_void_v<ResultT>) {
                return;
            } else {
                return ResultT{};
            }
        }

        if constexpr (std::is_void_v<ResultT>) {
            std::promise<void> promise;
            auto future = promise.get_future();

            taskInterface->AddTask([fnPtr = &fn, promisePtr = &promise]() {
                ScopedThreadRole roleGuard(ThreadRole::Main);
                try {
                    detail::RunDispatcher::Invoke(*fnPtr);
                    promisePtr->set_value();
                } catch (...) {
                    promisePtr->set_exception(std::current_exception());
                }
            });

            future.get(); // may re-throw
            return;
        } else {
            std::promise<ResultT> promise;
            auto future = promise.get_future();

            taskInterface->AddTask([fnPtr = &fn, promisePtr = &promise]() {
                ScopedThreadRole roleGuard(ThreadRole::Main);
                try {
                    promisePtr->set_value(detail::RunDispatcher::Invoke(*fnPtr));
                } catch (...) {
                    promisePtr->set_exception(std::current_exception());
                }
            });

            return future.get(); // may re-throw
        }
    }
} // namespace NarrativeEngine::MainThread
