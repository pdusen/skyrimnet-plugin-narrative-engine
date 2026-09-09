#pragma once

#include <LetterPool.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Stand-in for the letter pool, which owns the twenty Book forms a letter can
// be written into and the slot bookkeeping around them.
//
// The letter beat's whole dispatch arm is a conversation with this: ask for a
// slot, fill it with what the model wrote, and give it back if anything after
// that fails. What the beat has to get right is which of those it does and in
// what order, so the pool is stood in for and recorded rather than run -- the
// real one resolves twenty forms out of the load order and keeps a lifecycle
// of its own, none of which the beat is responsible for.
namespace NarrativeEngine::Testing
{
    struct LetterPoolSpyState
    {
        std::mutex mutex;

        // What ResolvePerSlotQuests handed over at kDataLoaded, and what the
        // beat reads back for each slot it dispatches. Deliberately NOT
        // cleared by Reset: data load runs once per process, so a second
        // section clearing this would leave the beat with no quest to start
        // and nothing in the world to blame it on.
        std::array<RE::TESQuest*, LetterPool::kPoolSize> perSlotQuests{};
        bool setPerSlotQuestsCalled = false;

        // The slot the next Allocate hands out, and the Book form on it.
        std::size_t nextSlot = 0;
        std::uint32_t bookFormID = 0x0E000001u;
        // Set to refuse instead. The two failures read differently in the
        // beat's failure reason, which is what a diagnosing player sees.
        std::optional<LetterPool::AllocationFailure> allocationFailure;
        std::size_t allocations = 0;

        // What was written into the slot. The composed letter reaches the
        // player through this and nowhere else.
        struct Populated
        {
            std::size_t slotIndex = 0;
            std::string senderLabel;
            std::string body;
            std::uint32_t senderNpcFormID = 0;
            std::string topicTag;
            std::string mood;
            std::vector<std::string> tags;
        };
        std::vector<Populated> populated;

        // Slots handed back, in order. A dispatch that fails without giving
        // its slot back leaks one of twenty for the rest of the playthrough.
        std::vector<std::size_t> aborted;

        void Reset();
    };

    LetterPoolSpyState& LetterPoolSpies();
} // namespace NarrativeEngine::Testing
