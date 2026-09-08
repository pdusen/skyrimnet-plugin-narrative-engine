#include <BeatWorkDispatch.h>

#include <DispatcherContract.h>

#include <PluginThread.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <utility>

// BeatWorkDispatch is one of four plugin worker queues built to the same shape, so
// it is checked against the shared contract in testsupport/DispatcherContract.h
// rather than against a copy of it. Anything specific to this dispatcher would
// go below as its own TEST_CASE; so far there is nothing — the four differ
// only in which stall they exist to isolate, which is a fact about their
// callers rather than about them.

TEST_CASE("BeatWorkDispatch worker contract", "[BeatWorkDispatch][engine]")
{
    namespace Dispatch = NarrativeEngine::BeatWorkDispatch;
    NarrativeEngine::Testing::CheckDispatcherContract({
        [] { Dispatch::Start(); },
        [] { Dispatch::Stop(); },
        [](std::function<void()> work) {
            Dispatch::EnqueueWork([w = std::move(work)](const NarrativeEngine::PluginThread::Token&) { w(); });
        },
        [] { Dispatch::EnqueueWork(nullptr); },
    });
}
