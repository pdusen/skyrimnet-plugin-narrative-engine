#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>

// Stand-in for the bookkeeping the visit and letter beats each keep, which
// their composers consult while working out who could plausibly get in touch.
//
// Beside the harness rather than inside one test file because the composers'
// tests and the beats' own will both want it. Two things are asked of it and
// both are about not asking the same person twice: whether a sender has been in
// touch recently enough to be off the list, and how far through their memories
// the last approach already read.
//
// One state serves both beats. They are separate ledgers in the shipping build
// and nothing here needs them told apart — a test that wanted to would be
// asking about the beats rather than about the composers.
namespace NarrativeEngine::Testing
{
    struct VisitSpyState
    {
        std::mutex mutex;

        // Senders who have been round recently enough to be passed over.
        std::set<std::uint32_t> onCooldown;

        // How far into a sender's memories a previous visit already got, in
        // game hours. Absent means they have never visited, which is a
        // different thing from having visited and read nothing.
        std::map<std::uint32_t, double> memoryWatermarks;

        void Reset();
    };

    VisitSpyState& VisitSpies();
} // namespace NarrativeEngine::Testing
