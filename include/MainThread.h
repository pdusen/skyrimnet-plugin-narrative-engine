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

// Zero-sized proof-of-main-thread. See docs/THREADING_MODEL.md. The
// only way to hold one is to be inside a lambda handed off via
// FireAndForget or Run below.
namespace NarrativeEngine::MainThread
{
    namespace detail
    {
        struct FireAndForgetDispatcher;
        struct RunDispatcher;
    } // namespace detail

    class Token
    {
    public:
        // Non-copyable / non-movable so a lambda can't stash the token
        // and satisfy a token-taking signature outside its dispatcher
        // invocation.
        Token(const Token&) = delete;
        Token& operator=(const Token&) = delete;
        Token(Token&&) = delete;
        Token& operator=(Token&&) = delete;

    private:
        Token() = default;

        friend struct detail::FireAndForgetDispatcher;
        friend struct detail::RunDispatcher;
    };

    // Schedule `fn` onto the main thread; return immediately. Exceptions
    // are caught and logged. Dropped with a warning if SKSE's task
    // interface is unavailable.
    void FireAndForget(const PluginThread::Token&, std::function<void(const Token&)> fn);

    namespace detail
    {
        // In the header because Run<T> is a template — the dispatcher's
        // Invoke must be visible at instantiation sites.
        struct RunDispatcher
        {
            template <typename Fn> static auto Invoke(Fn& fn) -> std::invoke_result_t<Fn, const Token&>
            {
                Token token;
                return fn(token);
            }
        };
    } // namespace detail

    // Blocking marshal to main. Exceptions thrown by `fn` propagate back
    // via the future — the caller catches them exactly like a
    // synchronous call. No timeout: a wedged main thread means the game
    // has stopped.
    template <typename Fn> auto Run(const PluginThread::Token&, Fn&& fn) -> std::invoke_result_t<Fn, const Token&>
    {
        using ResultT = std::invoke_result_t<Fn, const Token&>;

        // Runtime backstop for token forgery via friend-hackery. The
        // compile-time barrier is the PluginThread::Token signature.
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
