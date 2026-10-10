#include <LetterPool.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <FakeSkyrimNet.h>
#include <MinHookMocks.h>

#include <NPCLetterBeat.h>
#include <SkyrimNetAPI.h>

#include <SKSE/Interfaces.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Tests for the twenty slots a generated letter can live in.
//
// A letter is not a document; it is one of twenty Book records in the mod's
// own ESP, borrowed for as long as one letter needs it and then handed back.
// So the pool's job is bookkeeping over a fixed, small, permanently-existing
// set of forms, and every way of getting it wrong is a form the player can
// still find afterwards: a book with somebody else's letter in it, a slot
// nothing can ever allocate again, or twenty copies of the same title.
//
// The slot lifecycle is Free -> PendingDelivery -> InInventory -> Read, and
// the transitions are driven from outside by things the player does. Each of
// those entry points refuses to act from the wrong state, which is what stops
// a duplicate container event writing a second "she sent me a letter" memory.
//
// Mocked-engine: the Book forms, the per-slot delivery quests, the player's
// inventory, and the two detours the pool installs over engine functions. The
// detours themselves are recorded rather than installed -- see MinHookMocks.h.

namespace
{
    namespace LetterPool = NarrativeEngine::LetterPool;
    namespace BeatQuests = NarrativeEngine::NPCLetterBeat_QuestControl;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;
    using NarrativeEngine::Testing::MinHookMocks;

    constexpr const char* kSettings = "[General]\nbDebugMode=0\n";

    constexpr std::uint32_t kBookBase = 0x0E030001u;
    constexpr std::uint32_t kQuestBase = 0x0E031001u;
    constexpr std::uint32_t kSender = 0x0001A6A0u;
    constexpr std::uint32_t kContainer = 0x0E032001u;

    constexpr std::uint32_t kRecordVersion = 2;

    constexpr const char* kBody = "I have been turning this over since you left and I mean to put it in writing.";

    FakeSkyrimNetState& FakeLLM()
    {
        HMODULE module = ::LoadLibraryA("SkyrimNet");
        REQUIRE(module != nullptr);
        auto* accessor = reinterpret_cast<FakeSkyrimNetStateFunc>(
            reinterpret_cast<void*>(::GetProcAddress(module, kFakeSkyrimNetStateExport)));
        REQUIRE(accessor != nullptr);
        return *accessor();
    }

    SKSE::SerializationInterface* FakeInterface()
    {
        alignas(16) static std::byte storage[64]{};
        return reinterpret_cast<SKSE::SerializationInterface*>(storage);
    }

    // The twenty Book records the pool borrows, under the editor IDs it
    // resolves them by, and the twenty per-slot delivery quests without which
    // a slot cannot actually deliver anything.
    struct Pool
    {
        std::vector<RE::TESForm*> books;
        std::array<RE::TESQuest*, LetterPool::kPoolSize> quests{};
    };

    Pool BuildPool(EngineMock& engine, std::size_t bookCount = LetterPool::kPoolSize, bool withQuests = true)
    {
        Pool pool;
        for (std::size_t i = 0; i < bookCount; ++i) {
            char editorID[64];
            std::snprintf(editorID, sizeof(editorID), "_ne_PooledLetter%02zu", i);
            pool.books.push_back(engine.AddBook(kBookBase + static_cast<std::uint32_t>(i), editorID));
        }
        if (withQuests) {
            for (std::size_t i = 0; i < LetterPool::kPoolSize; ++i) {
                char editorID[64];
                std::snprintf(editorID, sizeof(editorID), "_ne_PooledLetterQuest%02zu", i);
                EngineMock::QuestState state;
                state.editorID = editorID;
                state.formID = kQuestBase + static_cast<std::uint32_t>(i);
                state.running = false;
                pool.quests[i] = engine.AddQuestWithAliases(state, {"Sender", "LetterRef"}).quest;
            }
        }
        LetterPool::Initialize();
        LetterPool::SetPerSlotQuests(pool.quests);
        return pool;
    }

    // Take a slot and fill it, which is what the letter beat does in one
    // frame once the model has answered.
    std::size_t TakeSlot(const char* label = "A note", const char* body = kBody)
    {
        const auto taken = LetterPool::Allocate();
        REQUIRE(taken.has_value());
        LetterPool::PopulateSlot(taken->slotIndex, label, body, kSender, "the tusk", "warm", {"trade", "debt"});
        return taken->slotIndex;
    }

    LetterPool::State StateOf(std::size_t slotIndex)
    {
        return LetterPool::GetSlotSnapshots()[slotIndex].state;
    }
} // namespace

TEST_CASE("LetterPool resolves the forms it borrows", "[LetterPool][engine]")
{
    // Happy path, re-run per leaf: twenty Book records under the editor IDs
    // the pool looks for, and twenty delivery quests to send them with.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    LetterPool::OnRevert();
    const auto pool = BuildPool(engine);

    SECTION("when every record is where it should be")
    {
        SECTION("should have a form for every slot")
        {
            REQUIRE(LetterPool::GetStats().resolved == LetterPool::kPoolSize);
        }

        SECTION("should recognise its own books")
        {
            // The render detours fire on every book the player opens, and the
            // first thing they do is ask this. A form it fails to recognise
            // shows the player the ESP's placeholder text instead of a letter.
            REQUIRE(LetterPool::IsManagedForm(pool.books.front()->GetFormID()));
            REQUIRE_FALSE(LetterPool::IsManagedForm(0x0001A6A0u));
            REQUIRE_FALSE(LetterPool::IsManagedForm(0));
        }

        SECTION("should keep the slot table across a second data load")
        {
            // kDataLoaded can fire again. Re-resolving must rewire the forms
            // without wiping a letter that is already in flight.
            const auto slot = TakeSlot();
            LetterPool::Initialize();
            REQUIRE(StateOf(slot) == LetterPool::State::PendingDelivery);
        }
    }
}

TEST_CASE("LetterPool with only some of its records installed", "[LetterPool][engine]")
{
    // A partial ESP install, or a patch that dropped records. Its own case
    // because the shortfall has to be the only world this process sees.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    LetterPool::OnRevert();
    BuildPool(engine, 5);

    SECTION("should count only the ones it found")
    {
        REQUIRE(LetterPool::GetStats().resolved == 5);
    }

    SECTION("should still hand out the ones it has")
    {
        // Five letters is worse than twenty and much better than none, and a
        // pool that refused everything would silence the beat entirely.
        REQUIRE(LetterPool::Allocate().has_value());
    }
}

TEST_CASE("LetterPool hands out one slot at a time", "[LetterPool][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    LetterPool::OnRevert();
    const auto pool = BuildPool(engine);

    SECTION("when the pool is untouched")
    {
        const auto first = LetterPool::Allocate();

        SECTION("should give out a slot with a book behind it")
        {
            REQUIRE(first.has_value());
            REQUIRE(first->bookFormID == pool.books.front()->GetFormID());
        }

        SECTION("should not give the same one out twice")
        {
            // Two letters sharing a Book record are one letter: the second
            // populate overwrites the first's title and body, and the player
            // is holding two copies of the same thing.
            LetterPool::PopulateSlot(first->slotIndex, "A note", kBody, kSender, "topic", "warm", {});
            const auto second = LetterPool::Allocate();
            REQUIRE(second.has_value());
            REQUIRE(second->slotIndex != first->slotIndex);
        }
    }

    SECTION("when a slot is filled")
    {
        const auto slot = TakeSlot("Note from Ysolda");

        SECTION("should hold what the letter says")
        {
            const auto snapshot = LetterPool::GetSlotSnapshots()[slot];
            REQUIRE(snapshot.state == LetterPool::State::PendingDelivery);
            REQUIRE(snapshot.senderLabel == "Note from Ysolda");
            REQUIRE(snapshot.body == kBody);
            REQUIRE(snapshot.topicTag == "the tusk");
            REQUIRE(snapshot.mood == "warm");
        }

        SECTION("should put the title on the record the player will see")
        {
            // The label reaches the player as the item's name in their
            // inventory, and nothing else writes it.
            auto* book = pool.books[slot]->As<RE::TESObjectBOOK>();
            REQUIRE(book != nullptr);
            REQUIRE(std::string{book->GetFullName()} == "Note from Ysolda");
        }

        SECTION("should serve the body to the detour that renders it")
        {
            std::string body;
            REQUIRE(LetterPool::TryGetBody(pool.books[slot]->GetFormID(), body));
            REQUIRE(body == kBody);
        }
    }

    SECTION("when every slot is taken and none delivered")
    {
        for (std::size_t i = 0; i < LetterPool::kPoolSize; ++i) {
            (void)TakeSlot();
        }

        SECTION("should refuse rather than evict a letter still in flight")
        {
            // Evicting one of these would delete a letter the courier is
            // already carrying, out from under the player.
            const auto refused = LetterPool::Allocate();
            REQUIRE_FALSE(refused.has_value());
            REQUIRE(refused.error() == LetterPool::AllocationFailure::EvictionFailed);
        }
    }
}

TEST_CASE("LetterPool with no delivery quests behind its slots", "[LetterPool][engine]")
{
    // The Book records are there and the quests that send them are not. Its
    // own case because the pool is handed the quest table once, at data load.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    LetterPool::OnRevert();
    BuildPool(engine, LetterPool::kPoolSize, /*withQuests=*/false);

    SECTION("should refuse rather than take a slot it cannot dispatch")
    {
        // A Book with no quest is a letter with no way to reach the player.
        // Taking the slot would spend one of twenty on nothing.
        REQUIRE_FALSE(LetterPool::Allocate().has_value());
    }

    SECTION("should still say the forms themselves resolved")
    {
        // The two failures read differently in the log, and a diagnosing
        // player needs to know which half of the install is missing.
        REQUIRE(LetterPool::GetStats().resolved == LetterPool::kPoolSize);
    }
}

TEST_CASE("LetterPool follows a letter through the player's hands", "[LetterPool][engine]")
{
    // The lifecycle, driven from outside by things the player does. Each
    // transition refuses to fire from the wrong state, which is what stops a
    // repeated engine event writing a second memory for one letter.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    LetterPool::OnRevert();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    FakeLLM().Reset();
    const auto pool = BuildPool(engine);
    const auto slot = TakeSlot();

    SECTION("when the courier hands it over")
    {
        LetterPool::MarkDelivered(slot);

        SECTION("should say the player is carrying it")
        {
            REQUIRE(StateOf(slot) == LetterPool::State::InInventory);
        }

        SECTION("should give the sender a memory of having written")
        {
            // The sender's own record of the letter. Without it the NPC has
            // no idea they wrote to the player and will not refer to it.
            REQUIRE(FakeLLM().addMemoryCalls == 1);
            REQUIRE(std::string{FakeLLM().lastMemoryText}.find(kBody) != std::string::npos);
        }

        SECTION("should not write a second memory if the event repeats")
        {
            // Container events arrive more than once for one hand-off.
            LetterPool::MarkDelivered(slot);
            REQUIRE(FakeLLM().addMemoryCalls == 1);
        }
    }

    SECTION("when the player reads it")
    {
        LetterPool::MarkDelivered(slot);
        engine.papyrus.packedInts.clear();
        LetterPool::MarkRead(slot);

        SECTION("should say it has been read")
        {
            REQUIRE(StateOf(slot) == LetterPool::State::Read);
        }

        SECTION("should move the slot's quest on to the stage that says so")
        {
            // The player-side memory write is deliberately switched off in
            // the shipping build pending an upstream fix, so the quest stage
            // is the whole of what reading a letter still does.
            REQUIRE_FALSE(engine.papyrus.packedInts.empty());
            REQUIRE(engine.papyrus.packedInts.back() == 40);
        }

        SECTION("should not re-stamp it if they read it again")
        {
            const auto readAt = LetterPool::GetSlotSnapshots()[slot].readAt;
            LetterPool::MarkRead(slot);
            REQUIRE(LetterPool::GetSlotSnapshots()[slot].readAt == readAt);
        }
    }

    SECTION("when the player reads it before it was ever delivered")
    {
        LetterPool::MarkRead(slot);

        SECTION("should leave the slot where it was")
        {
            // Reachable: the render detour fires on any book the player
            // opens, including one that reached them some other way.
            REQUIRE(StateOf(slot) == LetterPool::State::PendingDelivery);
        }
    }

    SECTION("when the player sells it")
    {
        auto* chest = engine.AddReference(nullptr, kContainer, {});
        LetterPool::MarkDelivered(slot);
        LetterPool::MarkDiscardedToContainer(slot, chest);

        SECTION("should give the slot back")
        {
            REQUIRE(StateOf(slot) == LetterPool::State::Free);
        }

        SECTION("should take the letter out of the container it went into")
        {
            // A merchant chest is swept by nothing else: the eviction walks
            // loaded actors and the player's own cell, and a sold letter is
            // in neither.
            const auto& removals = engine.inventory.removals;
            REQUIRE(std::any_of(removals.begin(), removals.end(), [&](const auto& removal) {
                return removal.holderFormID == kContainer && removal.itemFormID == pool.books[slot]->GetFormID();
            }));
        }
    }

    SECTION("when the player drops it on the ground")
    {
        auto* dropped = engine.AddReference(nullptr, kContainer, {});
        LetterPool::MarkDelivered(slot);
        LetterPool::MarkDroppedToCell(slot, dropped);

        SECTION("should give the slot back")
        {
            REQUIRE(StateOf(slot) == LetterPool::State::Free);
        }
    }

    SECTION("when a dispatch never reached the world")
    {
        LetterPool::AbortPending(slot);

        SECTION("should give the slot back straight away")
        {
            // No sweep and no quest teardown: nothing was ever placed, so
            // there is nothing out there to find.
            REQUIRE(StateOf(slot) == LetterPool::State::Free);
            REQUIRE(LetterPool::GetStats().free == LetterPool::kPoolSize);
        }
    }

    SECTION("when a slot index is not one of the twenty")
    {
        SECTION("should do nothing at all")
        {
            LetterPool::MarkDelivered(LetterPool::kPoolSize);
            LetterPool::MarkRead(LetterPool::kPoolSize);
            LetterPool::AbortPending(LetterPool::kPoolSize);
            LetterPool::Free(LetterPool::kPoolSize);
            REQUIRE(StateOf(slot) == LetterPool::State::PendingDelivery);
        }
    }
}

TEST_CASE("LetterPool reuses the slot a finished letter was in", "[LetterPool][engine]")
{
    // Twenty is a hard cap, so the allocator has to be able to take one back.
    // Which one it takes is the whole of the policy: never one still in
    // flight, the longest-read first, and only then one the player is still
    // carrying unread.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    LetterPool::OnRevert();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    FakeLLM().Reset();
    const auto pool = BuildPool(engine);
    std::vector<std::size_t> taken;
    for (std::size_t i = 0; i < LetterPool::kPoolSize; ++i) {
        taken.push_back(TakeSlot());
    }

    SECTION("when one of them has been read")
    {
        LetterPool::MarkDelivered(taken[7]);
        LetterPool::MarkRead(taken[7]);

        SECTION("should take that one back")
        {
            const auto next = LetterPool::Allocate();
            REQUIRE(next.has_value());
            REQUIRE(next->slotIndex == taken[7]);
        }
    }

    SECTION("when one is carried unread and another has been read")
    {
        LetterPool::MarkDelivered(taken[3]);
        LetterPool::MarkDelivered(taken[9]);
        LetterPool::MarkRead(taken[9]);

        SECTION("should take the read one rather than the unread one")
        {
            // A letter the player has not opened yet is still a letter they
            // are going to open. Taking it deletes something unread out of
            // their inventory.
            const auto next = LetterPool::Allocate();
            REQUIRE(next.has_value());
            REQUIRE(next->slotIndex == taken[9]);
        }
    }

    SECTION("when the only spare ones are carried unread")
    {
        LetterPool::MarkDelivered(taken[5]);

        SECTION("should take one of those rather than refuse")
        {
            // Last resort. Refusing here would stop the beat firing for the
            // rest of the playthrough once twenty letters had been delivered.
            const auto next = LetterPool::Allocate();
            REQUIRE(next.has_value());
            REQUIRE(next->slotIndex == taken[5]);
        }
    }
}

TEST_CASE("LetterPool patches the two engine functions it needs", "[LetterPool][engine]")
{
    // One detour rewrites a book's body as it is read, the other catches the
    // menu opening so the substitution is visible at all. Installed once per
    // process, so the second call below is the idempotency check as much as
    // it is setup: two detours chained onto one function is a crash waiting
    // for the first book the player opens.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    LetterPool::InstallHooks();
    LetterPool::InstallHooks();

    SECTION("should install both detours, once")
    {
        REQUIRE(MinHookMocks().hooks.size() == 2);
    }

    SECTION("should enable them")
    {
        // A created-but-not-enabled hook is a detour that never runs, and
        // nothing later reports the difference.
        for (const auto& hook : MinHookMocks().hooks) {
            REQUIRE(hook.enabled);
        }
    }

    SECTION("should patch two different functions")
    {
        REQUIRE(MinHookMocks().hooks.front().target != MinHookMocks().hooks.back().target);
    }
}

TEST_CASE("LetterPool carries its slots across a save", "[LetterPool][engine]")
{
    // Twenty Book records the player may be carrying letters in. Losing the
    // table on a load leaves the pool handing out slots whose books are
    // already in somebody's inventory with somebody else's letter in them.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    LetterPool::OnRevert();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    FakeLLM().Reset();
    const auto pool = BuildPool(engine);
    const auto slot = TakeSlot("Note from Ysolda");
    LetterPool::MarkDelivered(slot);
    // The player is carrying it, which is what the load-time sweep checks
    // each restored slot against.
    engine.inventory.playerCounts[pool.books[slot]->GetFormID()] = 1;
    // Every FormID in a co-save comes back through the load order that is
    // being entered, not the one that wrote it. Pointed at the book this slot
    // actually holds, so the restored slot resolves to a real record rather
    // than to whatever now sits at the id the save was written with.
    engine.cosave.resolvedFormID = pool.books[slot]->GetFormID();

    SECTION("when the pool is written and read back")
    {
        LetterPool::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        LetterPool::OnRevert();
        LetterPool::OnLoad(FakeInterface(), kRecordVersion, 0);

        SECTION("should bring the slot's state back")
        {
            REQUIRE(StateOf(slot) == LetterPool::State::InInventory);
        }

        SECTION("should bring back what the letter said")
        {
            // The body is the letter. It came from a model call nobody is
            // going to make again, and the render detour reads it from here
            // every time the player opens the book.
            const auto snapshot = LetterPool::GetSlotSnapshots()[slot];
            REQUIRE(snapshot.body == kBody);
            REQUIRE(snapshot.senderLabel == "Note from Ysolda");
        }

        SECTION("should bring back the mood the follow-up memories need")
        {
            // Added in the second version of the record, and read by the
            // memory writes that fire when the player finally opens it.
            REQUIRE(LetterPool::GetSlotSnapshots()[slot].mood == "warm");
        }
    }

    SECTION("when the player no longer has a letter the save says they do")
    {
        // Console removal, a save editor, or another mod. The slot would
        // otherwise stay spoken for forever.
        engine.inventory.playerCounts.clear();
        LetterPool::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        LetterPool::OnRevert();
        LetterPool::OnLoad(FakeInterface(), kRecordVersion, 0);

        SECTION("should give the slot back")
        {
            REQUIRE(StateOf(slot) == LetterPool::State::Free);
        }
    }

    SECTION("when the version is one no build ever wrote")
    {
        LetterPool::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        LetterPool::OnRevert();
        LetterPool::OnLoad(FakeInterface(), 99, 0);

        SECTION("should restore nothing")
        {
            REQUIRE(LetterPool::GetStats().free == LetterPool::kPoolSize);
        }
    }

    SECTION("when the record ends early")
    {
        engine.cosave.readable.clear();
        engine.cosave.readCursor = 0;
        LetterPool::OnRevert();

        SECTION("should restore nothing")
        {
            LetterPool::OnLoad(FakeInterface(), kRecordVersion, 0);
            REQUIRE(LetterPool::GetStats().free == LetterPool::kPoolSize);
        }
    }

    SECTION("when there is no serialization interface")
    {
        SECTION("should write nothing")
        {
            engine.cosave.written.clear();
            LetterPool::OnSave(nullptr);
            REQUIRE(engine.cosave.written.empty());
        }

        SECTION("should leave what is live alone")
        {
            LetterPool::OnLoad(nullptr, kRecordVersion, 0);
            REQUIRE(StateOf(slot) == LetterPool::State::InInventory);
        }
    }

    SECTION("when the record will not open")
    {
        engine.cosave.written.clear();
        engine.cosave.openRecordSucceeds = false;

        SECTION("should write nothing")
        {
            LetterPool::OnSave(FakeInterface());
            REQUIRE(engine.cosave.written.empty());
        }
    }
}
