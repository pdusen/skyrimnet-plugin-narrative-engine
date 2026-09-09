#include "LetterPoolSpies.h"

// See LetterPoolSpies.h for why the pool is stood in for.

namespace NarrativeEngine::Testing
{
    void LetterPoolSpyState::Reset()
    {
        std::scoped_lock lock(mutex);
        nextSlot = 0;
        bookFormID = 0x0E000001u;
        allocationFailure.reset();
        allocations = 0;
        populated.clear();
        aborted.clear();
    }

    LetterPoolSpyState& LetterPoolSpies()
    {
        static auto* state = new LetterPoolSpyState();
        return *state;
    }
} // namespace NarrativeEngine::Testing

namespace NarrativeEngine::LetterPool
{
    void SetPerSlotQuests(const std::array<RE::TESQuest*, kPoolSize>& quests)
    {
        auto& spies = Testing::LetterPoolSpies();
        std::scoped_lock lock(spies.mutex);
        spies.perSlotQuests = quests;
        spies.setPerSlotQuestsCalled = true;
    }

    RE::TESQuest* GetPerSlotQuest(std::size_t slotIndex)
    {
        auto& spies = Testing::LetterPoolSpies();
        std::scoped_lock lock(spies.mutex);
        return slotIndex < spies.perSlotQuests.size() ? spies.perSlotQuests[slotIndex] : nullptr;
    }

    std::expected<AllocatedSlot, AllocationFailure> Allocate()
    {
        auto& spies = Testing::LetterPoolSpies();
        std::scoped_lock lock(spies.mutex);
        ++spies.allocations;
        if (spies.allocationFailure) {
            return std::unexpected(*spies.allocationFailure);
        }
        return AllocatedSlot{spies.nextSlot, spies.bookFormID};
    }

    void PopulateSlot(std::size_t slotIndex,
                      std::string senderLabel,
                      std::string body,
                      RE::FormID senderNpcFormID,
                      std::string topicTag,
                      std::string mood,
                      std::vector<std::string> tags)
    {
        auto& spies = Testing::LetterPoolSpies();
        std::scoped_lock lock(spies.mutex);
        spies.populated.push_back({slotIndex,
                                   std::move(senderLabel),
                                   std::move(body),
                                   senderNpcFormID,
                                   std::move(topicTag),
                                   std::move(mood),
                                   std::move(tags)});
    }

    void AbortPending(std::size_t slotIndex)
    {
        auto& spies = Testing::LetterPoolSpies();
        std::scoped_lock lock(spies.mutex);
        spies.aborted.push_back(slotIndex);
    }
} // namespace NarrativeEngine::LetterPool
