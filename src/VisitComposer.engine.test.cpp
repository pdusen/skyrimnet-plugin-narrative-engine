#include <VisitComposer.h>

#include <AsyncDispatch.h>
#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <FakeSkyrimNet.h>
#include <NPCVisitBeat.h>
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
#include <vector>

// Tests for the visit briefing.
//
// A visit is one NPC deciding to come and find the player, and the composer
// writes what they are carrying when they arrive. Two separate jobs live here
// and they fail in different ways.
//
// The first is working out who could plausibly turn up at all. That list goes
// to the beat-select model as its menu, so anyone wrongly on it is someone the
// model may pick and the beat then has to walk across the province — a levelled
// guard with no identity, a follower already standing next to the player, an
// NPC in the middle of a fight, or the same person who called yesterday. Each
// of those is one line in a filter, and each of them has been an actual
// pathology at some point.
//
// The second is the compose round trip, which is untrusted text from a model
// being turned into something SkyrimNet will speak aloud. Every field is
// checked before it is accepted, and refusing is always the right answer: the
// beat treats a missing briefing as a clean failure and simply does not run,
// where a bad one becomes an NPC saying something incoherent to the player's
// face. So most of the cases below are about what gets refused.
//
// The model here is the stand-in SkyrimNet beside the executable, which answers
// inline — so a round trip the shipping build spreads over seconds finishes
// before the call returns.
//
// One refusal is not reachable and is left uncovered: the composer declines
// when SkyrimNet is not available at all, and availability is decided once per
// process when the wrapper resolves its exports. Every other case in this file
// needs it resolved.

namespace
{
    namespace VisitComposer = NarrativeEngine::VisitComposer;
    namespace AsyncDispatch = NarrativeEngine::AsyncDispatch;
    namespace SkyrimNet = NarrativeEngine::SkyrimNetAPI;
    using NarrativeEngine::BeatContext;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;

    constexpr const char* kSettings = "[General]\nbDebugMode=0\n"
                                      "[Beats]\niVisitBriefingMinWords=5\niVisitBriefingMaxWords=40\n";

    constexpr std::uint32_t kYsolda = 0x0001A6A0u;
    constexpr std::uint32_t kCarlotta = 0x0001A6A1u;

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

    // A briefing long enough to clear the word floor the settings above set.
    const std::string kBriefing = "I have been turning this over for days and I mean to say it to your face.";

    // The narration has a floor of its own, and a wider one: it is the scene
    // the dialogue that follows is written from, so a single sentence gives
    // the model nothing to work with.
    const std::string kNarration = "She crosses the market with the walk of somebody who has decided something, "
                                   "stops a pace short of you, and waits with her hands still until you look up "
                                   "from what you were doing and give her your attention.";

    std::string ComposeAnswer(const std::string& mood = "warm", const std::string& briefing = kBriefing)
    {
        nlohmann::json j;
        j["briefing"] = briefing;
        j["narration"] = kNarration;
        j["mood"] = mood;
        j["topic_tag"] = "the tusk";
        return j.dump();
    }

    // The compose callback is delivered on a worker, so the call is made and
    // then waited on. Every wait is bounded and its arrival asserted, so a
    // composer that stops answering fails the run rather than hanging it.
    std::optional<VisitComposer::VisitBriefing> ComposeFor(std::uint32_t sender, bool& answered)
    {
        std::optional<VisitComposer::VisitBriefing> result;
        answered = false;
        BeatContext ctx;
        VisitComposer::Compose(ctx,
                               VisitComposer::UrgencyHint::Medium,
                               sender,
                               "she has been meaning to say something",
                               [&](std::optional<VisitComposer::VisitBriefing> briefing) {
                                   result = std::move(briefing);
                                   answered = true;
                               });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!answered && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return result;
    }

    // Brings up the queue a compose callback is marshalled onto, and takes it
    // down again afterwards.
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

TEST_CASE("VisitComposer knows which moods it will speak in", "[VisitComposer][engine]")
{
    // The mood reaches SkyrimNet's dialogue layer, which uses it to pick a
    // delivery. A value it does not recognise is a visit spoken flat.
    SECTION("when the mood is one of the set")
    {
        SECTION("should accept it")
        {
            REQUIRE(VisitComposer::IsValidMood("warm"));
            REQUIRE(VisitComposer::IsValidMood("urgent"));
            REQUIRE(VisitComposer::IsValidMood("businesslike"));
        }

        SECTION("should accept the one a visit adds over a letter")
        {
            // Somebody can turn up to apologise in a way they cannot write.
            REQUIRE(VisitComposer::IsValidMood("contrite"));
        }
    }

    SECTION("when the mood is something the model invented")
    {
        SECTION("should refuse it")
        {
            REQUIRE_FALSE(VisitComposer::IsValidMood("wistful"));
        }
    }

    SECTION("when the mood differs only in case")
    {
        SECTION("should refuse it")
        {
            // Matched exactly rather than loosely, because the downstream
            // layer matches exactly too — a tolerated "Warm" here would be an
            // unrecognised mood one layer down, where nothing checks.
            REQUIRE_FALSE(VisitComposer::IsValidMood("Warm"));
        }
    }
}

TEST_CASE("VisitComposer describes candidates for the picker", "[VisitComposer][engine]")
{
    // This JSON is the menu the beat-select model chooses a sender from, so
    // every field in it is something the choice may turn on.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};

    VisitComposer::SenderCandidate candidate;
    candidate.formId = kYsolda;
    candidate.name = "Ysolda";
    candidate.engagementScore = 12.5;
    candidate.lastInteractedAt = 100.0;
    candidate.memories = nlohmann::json::array({"she spoke of the mammoth tusk"});

    SECTION("when a candidate is serialized")
    {
        const auto out = VisitComposer::SerializeSenderCandidates({candidate});

        SECTION("should write the form id the way the model must return it")
        {
            // The model picks by echoing this string back, so the format is
            // half of a round trip rather than a display choice.
            REQUIRE(out.size() == 1);
            REQUIRE(out[0].value("form_id", std::string{}) == "0x1A6A0");
        }

        SECTION("should carry everything the choice turns on")
        {
            REQUIRE(out[0].value("name", std::string{}) == "Ysolda");
            REQUIRE(out[0].value("engagement_score", 0.0) == 12.5);
            REQUIRE(out[0].value("last_interacted_at", 0.0) == 100.0);
            REQUIRE(out[0].at("memories").size() == 1);
        }
    }

    SECTION("when there are no candidates")
    {
        SECTION("should write an empty list rather than nothing")
        {
            // The prompt renders this, and a null where an array belongs is a
            // template failure rather than an empty menu.
            const auto out = VisitComposer::SerializeSenderCandidates({});
            REQUIRE(out.is_array());
            REQUIRE(out.empty());
        }
    }
}

TEST_CASE("VisitComposer offers only people who could actually call", "[VisitComposer][engine]")
{
    // Happy path, re-run per leaf: one named, unique, unengaged NPC with a
    // memory worth bringing. Each case takes away the one thing that makes
    // them a plausible visitor.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();
    NarrativeEngine::NPCVisitBeat_Persistence::OnRevert();
    auto* ysolda = engine.AddActor(kYsolda);
    auto* base = engine.AddNPC(0x0001A6A2u);
    base->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kUnique);
    engine.SetActorBase(ysolda, base);
    engine.world.playerHasLocation = true;
    SetJson(fake.engagementJson, "[" + EngagementRow(kYsolda, "Ysolda") + "]");
    SetJson(fake.memoriesJson, "[" + MemoryRow("She spoke of the mammoth tusk.") + "]");

    SECTION("when they are somebody in particular")
    {
        SECTION("should offer them")
        {
            REQUIRE(VisitComposer::CollectSenderCandidates().size() == 1);
        }
    }

    SECTION("when they are one of a kind rather than a person")
    {
        base->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kUnique);

        SECTION("should leave them out")
        {
            // A levelled guard has no identity to visit with, and warping one
            // in produces "the same Whiterun Guard called three times".
            REQUIRE(VisitComposer::CollectSenderCandidates().empty());
        }
    }

    SECTION("when they are in the middle of a fight")
    {
        // Set across the mock rather than on one actor: the harness answers
        // this question the same way for everybody, and the pool holds one.
        engine.player.inCombat = true;

        SECTION("should leave them out")
        {
            REQUIRE(VisitComposer::CollectSenderCandidates().empty());
        }
    }

    SECTION("when they are already following the player")
    {
        // As above, mock-wide.
        engine.world.actorIsPlayerTeammate = true;

        SECTION("should leave them out")
        {
            // A follower cannot arrive to talk. They are already there, and
            // the beat would walk them away only to walk them back.
            REQUIRE(VisitComposer::CollectSenderCandidates().empty());
        }
    }

    SECTION("when the world cannot say where they are")
    {
        engine.world.playerHasLocation = false;

        SECTION("should leave them out")
        {
            // No location is the cheapest signal that an actor is in
            // engine-limbo, and a visit from limbo never arrives.
            REQUIRE(VisitComposer::CollectSenderCandidates().empty());
        }
    }

    SECTION("when they called only recently")
    {
        // Stamped through the beat's own arrival hook rather than by setting
        // a flag: the cooldown the composer reads is the one a completed
        // visit leaves behind, and the two have to be the same ledger. The
        // clock has to be off zero for the stamp to mean anything -- a stamp
        // at hour zero reads as never having happened.
        engine.calendar.hoursPassed = 100.0f;
        NarrativeEngine::NPCVisitBeat_Cooldowns::OnVisitCompleted(kYsolda);

        SECTION("should leave them out")
        {
            // The same person turning up three times running is the
            // pathology the cooldown exists for, and it reads as a bug long
            // before it reads as a character trait.
            REQUIRE(VisitComposer::CollectSenderCandidates().empty());
        }
    }
}

TEST_CASE("VisitComposer writes what a visitor arrives carrying", "[VisitComposer][engine]")
{
    // Happy path, re-run per leaf: a live named sender, SkyrimNet up with its
    // memory system ready, and a model answering with a complete briefing.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const RunningDispatch dispatch;
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();
    (void)engine.AddActor(kYsolda);
    SetJson(fake.memoriesJson, "[" + MemoryRow("She spoke of the mammoth tusk.") + "]");
    SetJson(fake.promptResponse, ComposeAnswer());
    bool answered = false;

    SECTION("when the model answers properly")
    {
        const auto briefing = ComposeFor(kYsolda, answered);

        SECTION("should produce a briefing")
        {
            REQUIRE(answered);
            REQUIRE(briefing.has_value());
        }

        SECTION("should carry what the sender means to say")
        {
            REQUIRE(briefing.has_value());
            REQUIRE(briefing->briefing == kBriefing);
        }

        SECTION("should carry the scene it is delivered in")
        {
            // Narration goes straight to SkyrimNet as scene context, and the
            // dialogue that follows is written from it.
            REQUIRE(briefing.has_value());
            REQUIRE_FALSE(briefing->narration.empty());
            REQUIRE(briefing->mood == "warm");
            REQUIRE(briefing->topicTag == "the tusk");
        }
    }

    SECTION("when no sender was picked")
    {
        SECTION("should refuse")
        {
            const auto briefing = ComposeFor(0, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(briefing.has_value());
        }
    }

    SECTION("when the sender no longer resolves")
    {
        SECTION("should refuse")
        {
            // Seconds pass between the pick and the compose, and a save can
            // be loaded in them.
            const auto briefing = ComposeFor(0x00DEAD01u, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(briefing.has_value());
        }
    }

    SECTION("when the sender has died since being picked")
    {
        engine.forms.actorIsDead = true;

        SECTION("should refuse")
        {
            const auto briefing = ComposeFor(kYsolda, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(briefing.has_value());
        }
    }

    SECTION("when the memory system is not ready")
    {
        fake.memorySystemReady = false;

        SECTION("should refuse")
        {
            // The briefing is written from the sender's memories. Composing
            // without them produces a visit about nothing.
            const auto briefing = ComposeFor(kYsolda, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(briefing.has_value());
        }
    }

    SECTION("when the model call fails")
    {
        fake.sendPromptSucceeds = false;

        SECTION("should refuse")
        {
            const auto briefing = ComposeFor(kYsolda, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(briefing.has_value());
        }
    }

    SECTION("when the answer is not JSON")
    {
        SetJson(fake.promptResponse, "She wants to talk about the tusk.");

        SECTION("should refuse")
        {
            const auto briefing = ComposeFor(kYsolda, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(briefing.has_value());
        }
    }

    SECTION("when the answer is missing a field")
    {
        SetJson(fake.promptResponse, R"({"briefing":"a","narration":"b","mood":"warm"})");

        SECTION("should refuse")
        {
            // Every field is used downstream, and a visit missing its topic
            // reaches the dialogue layer with nothing to be about.
            const auto briefing = ComposeFor(kYsolda, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(briefing.has_value());
        }
    }

    SECTION("when the mood is one nothing downstream knows")
    {
        SetJson(fake.promptResponse, ComposeAnswer("wistful"));

        SECTION("should refuse")
        {
            const auto briefing = ComposeFor(kYsolda, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(briefing.has_value());
        }
    }

    SECTION("when the briefing is too short to be worth a journey")
    {
        SetJson(fake.promptResponse, ComposeAnswer("warm", "Hello."));

        SECTION("should refuse")
        {
            // Someone crossing the province to say one word is the failure
            // mode a word floor exists to catch.
            const auto briefing = ComposeFor(kYsolda, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(briefing.has_value());
        }
    }

    SECTION("when the narration is a single sentence")
    {
        nlohmann::json thin;
        thin["briefing"] = kBriefing;
        thin["narration"] = "She walks up.";
        thin["mood"] = "warm";
        thin["topic_tag"] = "the tusk";
        SetJson(fake.promptResponse, thin.dump());

        SECTION("should refuse")
        {
            // The narration is the scene the dialogue that follows is written
            // from, and it has a floor of its own for that reason: three words
            // of stage direction leave the next model with nothing.
            const auto briefing = ComposeFor(kYsolda, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(briefing.has_value());
        }
    }

    SECTION("when the briefing runs far too long")
    {
        std::string wall;
        for (int i = 0; i < 200; ++i)
            wall += "word ";
        SetJson(fake.promptResponse, ComposeAnswer("warm", wall));

        SECTION("should refuse")
        {
            const auto briefing = ComposeFor(kYsolda, answered);
            REQUIRE(answered);
            REQUIRE_FALSE(briefing.has_value());
        }
    }

    SECTION("when there is nobody to hand the answer to")
    {
        SECTION("should do nothing at all")
        {
            // Not even resolve the sender: with no callback there is nowhere
            // for an answer to go, and the model call is the expensive part.
            BeatContext ctx;
            const int before = fake.sendPromptCalls;
            VisitComposer::Compose(ctx, VisitComposer::UrgencyHint::Medium, kYsolda, "", nullptr);
            REQUIRE(fake.sendPromptCalls == before);
        }
    }
}
