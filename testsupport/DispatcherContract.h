#pragma once

#include <ThreadRole.h>

#include <functional>

// The behaviour every plugin worker queue promises.
//
// AsyncDispatch, EvalDispatch, BeatWorkDispatch and GossipDispatch are the same
// module four times over: identical queue, identical worker loop, identical
// start/stop handshake, differing only in which stall they exist to isolate.
// Four near-identical test files would be four places to update when the shape
// changes and four chances to update only three of them.
//
// So the contract is written once here and instantiated per dispatcher. Each
// module's own `*.engine.test.cpp` stays a few lines, and a dispatcher that
// drifts away from the shared shape fails to satisfy this and says so.
//
// The ops erase the token type. Each dispatcher hands its worker a token of its
// own class — PluginThread::Token for three of them, GossipThread::Token for
// the fourth — and those are compile-time proof objects with no runtime
// content, so nothing the contract checks can tell them apart. What the
// contract does check is the role the worker claims, which is the same for all
// four and is observable without the token.
//
// This lives beside the harness rather than beside a module because it belongs
// to no one module. Anything specific to a single dispatcher belongs in that
// dispatcher's own file, not here.
namespace NarrativeEngine::Testing
{
    struct DispatcherOps
    {
        std::function<void()> start;
        std::function<void()> stop;
        std::function<void(std::function<void()>)> enqueue;
        // Hand the dispatcher an empty task, which each one spells with its own
        // std::function type. Kept separate from `enqueue` because a lambda
        // adapter can never itself be null.
        std::function<void()> enqueueEmpty;
    };

    // Runs the whole contract as nested SECTIONs. Call from inside a TEST_CASE:
    // Catch2 sections are ordinary runtime constructs, so a function full of
    // them composes exactly as if it had been written inline.
    void CheckDispatcherContract(const DispatcherOps& ops);
} // namespace NarrativeEngine::Testing
