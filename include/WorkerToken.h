#pragma once

#include <type_traits>

#include <GossipThread.h>
#include <PluginThread.h>

// Which thread tokens may hold a call that blocks for seconds.
//
// Some plugin APIs are safe on any worker we own but catastrophic on the
// main thread — the blocking SkyrimNet LLM call being the example that
// forced this to exist. Gating those on PluginThread::Token was right
// while AsyncDispatch and EvalDispatch were the only workers; it stopped
// being right the moment the gossip worker needed the same call.
//
// A trait plus a concept rather than a shared base class, deliberately:
//
//   * The existing token types are NOT modified. Each keeps its private
//     constructor and its single friend. Retrofitting a common base
//     would mean either a protected base constructor — forgeable by
//     anyone willing to write `struct Fake : WorkerTokenBase {};` — or a
//     friend list on the base naming every token, which is exactly the
//     coupling this avoids.
//
//   * Registering a future worker is one specialisation, in one place,
//     with no edit to any existing type.
//
// What deliberately does NOT accept a WorkerToken: MainThread::Run and
// MainThread::FireAndForget. They keep demanding PluginThread::Token, so
// gossip code cannot reach the main thread at all. That is a compile
// error rather than a rule, and it is what makes the one deadlock this
// design could otherwise suffer — the gossip thread blocking on
// something that is itself waiting on the gossip thread —
// unrepresentable.
namespace NarrativeEngine
{
    template <typename T> struct is_worker_token : std::false_type
    {};

    template <> struct is_worker_token<PluginThread::Token> : std::true_type
    {};

    template <> struct is_worker_token<GossipThread::Token> : std::true_type
    {};

    template <typename T>
    concept WorkerToken = is_worker_token<std::remove_cvref_t<T>>::value;
} // namespace NarrativeEngine
