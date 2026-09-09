#include <LetterComposer.h>

#include <AsyncDispatch.h>
#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <FakeSkyrimNet.h>
#include <NPCLetterBeat.h>
#include <SkyrimNetAPI.h>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

// Tests for writing a letter from an NPC to the player.
//
// A letter is what an NPC sends when they are too far away to come in person,
// so the whole beat rests on picking somebody the player will believe wrote it.
// The filter is the opposite of the visit beat's: a letter must come from
// somebody NOT here. Anyone standing in the same town is a person the player
// could go and talk to, and receiving a letter from them reads as a bug.
//
// The other half is the label — the line in the player's inventory that says
// what the item is. It has a hard 24-byte cap coming from the engine's book
// display, and the model routinely returns something longer or nothing at all.
// So there is a deterministic fallback that walks a series of shortenings and
// is guaranteed to produce something inside the cap, because the alternative is
// a book in the player's pack with a truncated or blank name.
//
// The model here is the stand-in SkyrimNet beside the executable, which answers
// inline. One refusal is unreachable and left uncovered: the composer declines
// when SkyrimNet is not available at all, and availability is settled once per
// process when the wrapper resolves its exports.

namespace
{
    namespace LetterComposer = NarrativeEngine::LetterComposer;
    namespace AsyncDispatch = NarrativeEngine::AsyncDispatch;
    namespace SkyrimNet = NarrativeEngine::SkyrimNetAPI;
    using NarrativeEngine::BeatContext;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;

    constexpr const char* kSettings = "[General]\nbDebugMode=0\n"
                                      "[Beats]\niLetterContentMinWords=5\niLetterContentMaxWords=40\n";

    constexpr std::uint32_t kYsolda = 0x0001A6A0u;

    FakeSkyrimNetState& FakeState()
    {
        HMODULE module = ::LoadLibraryA("SkyrimNet");
        REQUIRE(module != nullptr);
        auto* accessor = reinterpret_cast<FakeSkyrimNetStateFunc>(
            reinterpret_cast<void*>(::GetProcAddress(module, kFakeSkyrimNetStateExport)));
        REQUIRE(accessor != nullptr);
        return *accessor();
    }

    template <std::size_t N> void SetJson(char (&dest)[N], const std::string& text)
    {
        std::size_t i = 0;
        for (; i + 1 < N && i < text.size(); ++i)
            dest[i] = text[i];
        dest[i] = '\0';
    }

    std::string EngagementRow(std::uint32_t formId, const char* name)
    {
        return R"({"formId":)" + std::to_string(formId) + R"(,"name":")" + name
               + R"(","totalMemoryImportance":12.5,"lastEventTime":100.0})";
    }

    std::string MemoryRow(const char* content)
    {
        return R"({"type":"EXPERIENCE","content":")" + std::string{content}
               + R"(","importance_score":0.8,"game_time":50.0,"emotion":"calm","location":"Whiterun"})";
    }

    const std::string kBody = "I have been turning this over since you left and I mean to put it in writing.";

    std::string LetterAnswer(const std::string& label = "A note",
                             const std::string& mood = "warm",
                             const std::string& body = kBody)
    {
        nlohmann::json j;
        j["letter_label"] = label;
        j["body"] = body;
        j["mood"] = mood;
        j["topic_tag"] = "the tusk";
        j["tags"] = nlohmann::json::array({"trade", "debt"});
        return j.dump();
    }

    std::optional<LetterComposer::LetterComposition> ComposeFor(std::uint32_t sender, bool& answered)
    {
        std::optional<LetterComposer::LetterComposition> result;
        answered = false;
        BeatContext ctx;
        LetterComposer::Compose(ctx,
                                LetterComposer::UrgencyHint::Medium,
                                sender,
                                "she has been meaning to write",
                                [&](std::optional<LetterComposer::LetterComposition> letter) {
                                    result = std::move(letter);
                                    answered = true;
                                });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!answered && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return result;
    }

    struct RunningDispatch
    {
        RunningDispatch()
        {
            AsyncDispatch::Start();
        }

        ~RunningDispatch()
        {
            AsyncDispatch::Stop();
        }

        RunningDispatch(const RunningDispatch&) = delete;
        RunningDispatch& operator=(const RunningDispatch&) = delete;
    };
} // namespace

TEST_CASE("LetterComposer fits a label in the space the engine gives it", "[LetterComposer][engine]")
{
    // Twenty-four bytes, no more, and never empty. Past the cap the engine
    // truncates the book's name in the player's inventory; empty leaves them
    // holding an unnamed item.
    SECTION("when the name fits as it is")
    {
        SECTION("should use it whole")
        {
            REQUIRE(LetterComposer::SynthesizeFallbackLabel("Ysolda") == "Note from Ysolda");
        }
    }

    SECTION("when the name carries a trailing title")
    {
        SECTION("should drop the title to make room")
        {
            // "Note from " leaves fourteen bytes. The name alone fits inside
            // them once the epithet is off.
            REQUIRE(LetterComposer::SynthesizeFallbackLabel("Balgruuf the Greater") == "Note from Balgruuf");
        }
    }

    SECTION("when the name carries a leading title as well")
    {
        SECTION("should drop that too")
        {
            // The suffix alone is not enough here: "Housecarl Lydia" is
            // fifteen bytes against fourteen of room.
            REQUIRE(LetterComposer::SynthesizeFallbackLabel("Housecarl Lydia the Faithful") == "Note from Lydia");
        }
    }

    SECTION("when no shortening is enough")
    {
        const auto label = LetterComposer::SynthesizeFallbackLabel("Ahtar the Executioner of Solitude");

        SECTION("should cut it to the cap")
        {
            REQUIRE(label.size() <= 24);
        }

        SECTION("should still say who it is from")
        {
            REQUIRE(label.starts_with("Note from "));
        }
    }

    SECTION("when there is no name at all")
    {
        SECTION("should still produce a label")
        {
            // An unnamed item in the player's pack is worse than a vague one,
            // and this path is reached whenever a sender's name fails to
            // resolve.
            REQUIRE_FALSE(LetterComposer::SynthesizeFallbackLabel("").empty());
        }
    }
}

TEST_CASE("LetterComposer writes only to people who are away", "[LetterComposer][engine]")
{
    // Happy path, re-run per leaf: one NPC with a memory worth writing about,
    // somewhere the player is not. Each case brings them closer.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();
    NarrativeEngine::NPCLetterBeat_Persistence::OnRevert();
    (void)engine.AddActor(kYsolda);
    engine.visibility.target3DPresent = false;
    engine.world.playerHasLocation = false;
    SetJson(fake.engagementJson, "[" + EngagementRow(kYsolda, "Ysolda") + "]");
    SetJson(fake.memoriesJson, "[" + MemoryRow("She spoke of the mammoth tusk.") + "]");

    SECTION("when they are somewhere else entirely")
    {
        SECTION("should offer them")
        {
            REQUIRE(LetterComposer::CollectSenderCandidates().size() == 1);
        }
    }

    SECTION("when they are loaded in the world around the player")
    {
        engine.visibility.target3DPresent = true;

        SECTION("should leave them out")
        {
            // Somebody the player can see is somebody the player can talk to.
            // A letter from them is a letter from across the room.
            REQUIRE(LetterComposer::CollectSenderCandidates().empty());
        }
    }

    SECTION("when they are somewhere the player could walk to")
    {
        // Same location hub as the player, which is the test for "near
        // enough that a letter makes no sense".
        auto* town = engine.AddLocation(0x00E00001u, "Whiterun", {});
        engine.world.playerHasLocation = true;
        engine.world.playerLocationOverride = town;

        SECTION("should leave them out")
        {
            REQUIRE(LetterComposer::CollectSenderCandidates().empty());
        }
    }

    SECTION("when they wrote only recently")
    {
        // Stamped through the beat's own delivery hook rather than by setting
        // a flag: the cooldown the composer reads is the one a delivered
        // letter leaves behind, and the two have to be the same ledger. The
        // clock has to be off zero for the stamp to mean anything -- a stamp
        // at hour zero reads as never having happened.
        engine.calendar.hoursPassed = 100.0f;
        NarrativeEngine::NPCLetterBeat_Cooldowns::OnLetterDelivered(kYsolda);

        SECTION("should leave them out")
        {
            REQUIRE(LetterComposer::CollectSenderCandidates().empty());
        }
    }
}

TEST_CASE("LetterComposer describes candidates for the picker", "[LetterComposer][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};

    LetterComposer::SenderCandidate candidate;
    candidate.formId = kYsolda;
    candidate.name = "Ysolda";
    candidate.engagementScore = 12.5;
    candidate.lastInteractedAt = 100.0;
    candidate.memories = nlohmann::json::array({"she spoke of the mammoth tusk"});

    SECTION("when a candidate is serialized")
    {
        const auto out = LetterComposer::SerializeSenderCandidates({candidate});

        SECTION("should write the form id the way the model must return it")
        {
            REQUIRE(out.size() == 1);
            REQUIRE(out[0].value("form_id", std::string{}) == "0x1A6A0");
        }

        SECTION("should carry everything the choice turns on")
        {
            REQUIRE(out[0].value("name", std::string{}) == "Ysolda");
            REQUIRE(out[0].value("engagement_score", 0.0) == 12.5);
            REQUIRE(out[0].at("memories").size() == 1);
        }
    }

    SECTION("when there are no candidates")
    {
        SECTION("should write an empty list rather than nothing")
        {
            const auto out = LetterComposer::SerializeSenderCandidates({});
            REQUIRE(out.is_array());
            REQUIRE(out.empty());
        }
    }
}

TEST_CASE("LetterComposer writes what arrives in the post", "[LetterComposer][engine]")
{
    // Happy path, re-run per leaf: a live sender the player cannot see, and a
    // model answering with a complete letter.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const RunningDispatch dispatch;
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();
    (void)engine.AddActor(kYsolda);
    // The sender is re-checked at compose time, and a letter only makes sense
    // from somebody the player cannot walk over to.
    engine.visibility.target3DPresent = false;
    engine.world.playerHasLocation = false;
    SetJson(fake.memoriesJson, "[" + MemoryRow("She spoke of the mammoth tusk.") + "]");
    SetJson(fake.promptResponse, LetterAnswer());
    bool answered = false;

    SECTION("when the model answers properly")
    {
        const auto letter = ComposeFor(kYsolda, answered);

        SECTION("should produce a letter")
        {
            REQUIRE(answered);
            REQUIRE(letter.has_value());
        }

        SECTION("should carry what was written")
        {
            REQUIRE(letter.has_value());
            REQUIRE(letter->body == kBody);
            REQUIRE(letter->mood == "warm");
            REQUIRE(letter->topicTag == "the tusk");
        }

        SECTION("should say who it is from")
        {
            REQUIRE(letter.has_value());
            REQUIRE(letter->senderNpcFormID == kYsolda);
            REQUIRE(letter->senderLabel == "A note");
        }

        SECTION("should keep the facets it was tagged with")
        {
            // Tags are what the memory written for the sender is filed under
            // later, so losing them costs the letter its trace in their head.
            REQUIRE(letter.has_value());
            REQUIRE(letter->tags.size() == 2);
        }
    }

    SECTION("when the model gives no label")
    {
        nlohmann::json j = nlohmann::json::parse(LetterAnswer());
        j.erase("letter_label");
        SetJson(fake.promptResponse, j.dump());

        SECTION("should make one up rather than refuse")
        {
            // The label is soft-required: a letter with a synthesised name is
            // a letter, and refusing over it throws away a model call and the
            // beat with it.
            const auto letter = ComposeFor(kYsolda, answered);
            REQUIRE(letter.has_value());
            REQUIRE_FALSE(letter->senderLabel.empty());
        }
    }

    SECTION("when the label the model gave is too long for the engine")
    {
        SetJson(fake.promptResponse, LetterAnswer("A very long letter label indeed, far past the cap"));

        SECTION("should replace it with one that fits")
        {
            const auto letter = ComposeFor(kYsolda, answered);
            REQUIRE(letter.has_value());
            REQUIRE(letter->senderLabel.size() <= 24);
        }
    }

    SECTION("when the tags are not a list")
    {
        nlohmann::json j = nlohmann::json::parse(LetterAnswer());
        j["tags"] = "trade";
        SetJson(fake.promptResponse, j.dump());

        SECTION("should keep the letter and drop the tags")
        {
            // Soft-failing here rather than refusing: the topic alone is
            // enough to file the letter, and a malformed extra is not worth
            // the whole beat.
            const auto letter = ComposeFor(kYsolda, answered);
            REQUIRE(letter.has_value());
            REQUIRE(letter->tags.empty());
        }
    }

    SECTION("when no sender was picked")
    {
        SECTION("should refuse")
        {
            const auto letter = ComposeFor(0, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(letter.has_value());
        }
    }

    SECTION("when the sender no longer resolves")
    {
        SECTION("should refuse")
        {
            const auto letter = ComposeFor(0x00DEAD01u, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(letter.has_value());
        }
    }

    SECTION("when the memory system is not ready")
    {
        fake.memorySystemReady = false;

        SECTION("should refuse")
        {
            const auto letter = ComposeFor(kYsolda, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(letter.has_value());
        }
    }

    SECTION("when the model call fails")
    {
        fake.sendPromptSucceeds = false;

        SECTION("should refuse")
        {
            const auto letter = ComposeFor(kYsolda, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(letter.has_value());
        }
    }

    SECTION("when the answer is not JSON")
    {
        SetJson(fake.promptResponse, "She would like to write to you about the tusk.");

        SECTION("should refuse")
        {
            const auto letter = ComposeFor(kYsolda, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(letter.has_value());
        }
    }

    SECTION("when the answer has no body")
    {
        nlohmann::json j = nlohmann::json::parse(LetterAnswer());
        j.erase("body");
        SetJson(fake.promptResponse, j.dump());

        SECTION("should refuse")
        {
            // Unlike the label, there is nothing to fall back to: a book with
            // no text is a blank page in the player's inventory.
            const auto letter = ComposeFor(kYsolda, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(letter.has_value());
        }
    }

    SECTION("when the mood is one nothing downstream knows")
    {
        SetJson(fake.promptResponse, LetterAnswer("A note", "wistful"));

        SECTION("should refuse")
        {
            const auto letter = ComposeFor(kYsolda, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(letter.has_value());
        }
    }

    SECTION("when the letter is too short to be worth sending")
    {
        SetJson(fake.promptResponse, LetterAnswer("A note", "warm", "Hello."));

        SECTION("should refuse")
        {
            const auto letter = ComposeFor(kYsolda, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(letter.has_value());
        }
    }

    SECTION("when the letter runs far too long")
    {
        std::string wall;
        for (int i = 0; i < 200; ++i)
            wall += "word ";
        SetJson(fake.promptResponse, LetterAnswer("A note", "warm", wall));

        SECTION("should refuse")
        {
            const auto letter = ComposeFor(kYsolda, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(letter.has_value());
        }
    }

    SECTION("when there is nobody to hand the answer to")
    {
        SECTION("should do nothing at all")
        {
            BeatContext ctx;
            const int before = fake.sendPromptCalls;
            LetterComposer::Compose(ctx, LetterComposer::UrgencyHint::Medium, kYsolda, "", nullptr);
            REQUIRE(fake.sendPromptCalls == before);
        }
    }
}
