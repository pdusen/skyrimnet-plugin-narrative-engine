#pragma once

#include <PluginThread.h>

#include <functional>

// The behaviour every plugin worker queue promises.
//
// AsyncDispatch, EvalDispatch and BeatWorkDispatch are the same module three
// times over: identical queue, identical worker loop, identical start/stop
// handshake, differing only in which stall they exist to isolate. Three
// near-identical test files would be three places to update when the shape
// changes and three chances to update only two of them.
//
// So the contract is written once here and instantiated per dispatcher. Each
// module's own `*.engine.test.cpp` stays a few lines, and a dispatcher that
// drifts away from the shared shape fails to satisfy this and says so.
//
// This lives beside the harness rather than beside a module because it belongs
// to no one module. Anything specific to a single dispatcher belongs in that
// dispatcher's own file, not here.
namespace NarrativeEngine::Testing
{
    struct DispatcherOps
    {
        void (*start)();
        void (*stop)();
        void (*enqueue)(std::function<void(const PluginThread::Token&)>);
    };

    // Runs the whole contract as nested SECTIONs. Call from inside a TEST_CASE:
    // Catch2 sections are ordinary runtime constructs, so a function full of
    // them composes exactly as if it had been written inline.
    void CheckDispatcherContract(const DispatcherOps& ops);
} // namespace NarrativeEngine::Testing
