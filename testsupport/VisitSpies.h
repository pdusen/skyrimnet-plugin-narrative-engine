#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>

// Stand-in for the visit beat's own bookkeeping, which the composer consults
// while working out who could plausibly turn up.
//
// Beside the harness rather than inside one test file because the composer's
// tests and the beat's own will both want it. Two things are asked of it and
// both are about not asking the same person twice: whether a sender has visited
// recently enough to be off the list, and how far through their memories the
// last visit already read.
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
