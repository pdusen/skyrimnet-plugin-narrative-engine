#include <EvalDispatch.h>

#include <DispatcherContract.h>

#include <catch2/catch_test_macros.hpp>

// EvalDispatch is one of three plugin worker queues built to the same shape, so
// it is checked against the shared contract in testsupport/DispatcherContract.h
// rather than against a copy of it. Anything specific to this dispatcher would
// go below as its own TEST_CASE; so far there is nothing — the three differ
// only in which stall they exist to isolate, which is a fact about their
// callers rather than about them.

TEST_CASE("EvalDispatch worker contract", "[EvalDispatch][engine]")
{
    NarrativeEngine::Testing::CheckDispatcherContract({
        &NarrativeEngine::EvalDispatch::Start,
        &NarrativeEngine::EvalDispatch::Stop,
        &NarrativeEngine::EvalDispatch::EnqueueWork,
    });
}
