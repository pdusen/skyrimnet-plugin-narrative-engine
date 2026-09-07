#include <BeatParamHelpers.h>

#include <EngineMock.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <string>

// Mocked-engine tests for the shared beat parameter parsing.
//
// Two of the three functions are pure JSON work and would sit happily in the
// core target; the third looks a sender up in the engine's form table and
// checks it is still alive. They are one module and are tested as one, so the
// whole thing is mocked rather than split.
//
// Everything here reads LLM-supplied parameters, which is the reason the
// failure strings exist at all: each one is a stable snake_case literal that
// ends up in a rejection census, so a caller can tell "the model gave us a
// FormID that no longer resolves" from "the model gave us nonsense". Pinning
// those strings is pinning the diagnostics.
//
// The form lookup runs for real: `TESForm::LookupByID` walks a hash map the
// engine reaches through a data relocation, and the harness registers that
// relocation against a map a test fills in. See testsupport/RelocationMocks.h.

namespace
{
    using NarrativeEngine::Testing::EngineMock;
    using nlohmann::json;
    namespace Params = NarrativeEngine::BeatParamHelpers;
    using Params::UrgencyHint;

    constexpr std::uint32_t kSenderFormID = 0x000A2C8Eu;
} // namespace

TEST_CASE("BeatParamHelpers::ParseSenderFormID", "[BeatParamHelpers][engine]")
{
    // Happy path, re-run per leaf: a well-formed parameter object carrying the
    // hex FormID string the beat-select prompt is told to produce. The reason
    // string starts as something no branch writes, so a case asserting it was
    // set cannot pass on a leftover.
    json parameters = json::object({{"sender_npc_form_id", "0xA2C8E"}});
    std::string reason = "(untouched)";

    SECTION("when the parameters carry a hex form id")
    {
        SECTION("should parse it")
        {
            REQUIRE(Params::ParseSenderFormID(parameters, &reason) == kSenderFormID);
        }

        SECTION("should leave the failure reason alone")
        {
            (void)Params::ParseSenderFormID(parameters, &reason);
            REQUIRE(reason == "(untouched)");
        }
    }

    SECTION("when the id is written in decimal")
    {
        // The parse uses base 0, so the prefix decides. Worth pinning because
        // a model that drops the 0x would otherwise be read as a different
        // form rather than rejected.
        parameters["sender_npc_form_id"] = "666254";

        SECTION("should parse it as decimal")
        {
            REQUIRE(Params::ParseSenderFormID(parameters, &reason) == 666254u);
        }
    }

    SECTION("when the parameters are not an object")
    {
        parameters = json::array({1, 2});

        SECTION("should reject them")
        {
            REQUIRE_FALSE(Params::ParseSenderFormID(parameters, &reason).has_value());
        }

        SECTION("should say the parameters were not an object")
        {
            (void)Params::ParseSenderFormID(parameters, &reason);
            REQUIRE(reason == "parameters_not_object");
        }
    }

    SECTION("when the field is missing")
    {
        parameters = json::object({{"somethingElse", "0xA2C8E"}});

        SECTION("should say the field is missing")
        {
            REQUIRE_FALSE(Params::ParseSenderFormID(parameters, &reason).has_value());
            REQUIRE(reason == "sender_npc_form_id_missing");
        }
    }

    SECTION("when the field is not a string")
    {
        // A model that emits the id as a JSON number rather than a string
        // lands here. Same diagnosis as absent, deliberately.
        parameters["sender_npc_form_id"] = 666254;

        SECTION("should say the field is missing")
        {
            REQUIRE_FALSE(Params::ParseSenderFormID(parameters, &reason).has_value());
            REQUIRE(reason == "sender_npc_form_id_missing");
        }
    }

    SECTION("when the field cannot be parsed")
    {
        parameters["sender_npc_form_id"] = "Ysolda";

        SECTION("should say it was unparseable")
        {
            REQUIRE_FALSE(Params::ParseSenderFormID(parameters, &reason).has_value());
            REQUIRE(reason == "sender_npc_form_id_unparseable");
        }
    }

    SECTION("when the field parses to zero")
    {
        // Zero is the null FormID. Letting it through would send the beat
        // looking for a form that cannot exist.
        parameters["sender_npc_form_id"] = "0x0";

        SECTION("should say it was zero")
        {
            REQUIRE_FALSE(Params::ParseSenderFormID(parameters, &reason).has_value());
            REQUIRE(reason == "sender_npc_form_id_zero");
        }
    }

    SECTION("when the caller wants no failure reason")
    {
        parameters = json::array();

        SECTION("should reject without writing anywhere")
        {
            REQUIRE_FALSE(Params::ParseSenderFormID(parameters, nullptr).has_value());
        }
    }
}

TEST_CASE("BeatParamHelpers::ParseUrgencyHint", "[BeatParamHelpers][engine]")
{
    // Happy path: a well-formed object carrying a recognized hint. Every
    // rejection below falls back to Medium, so each case sets the field to
    // something that is NOT Medium's spelling to keep the fallback visible.
    json parameters = json::object({{"urgency_hint", "high"}});

    SECTION("when the hint is recognized")
    {
        SECTION("should parse high")
        {
            REQUIRE(Params::ParseUrgencyHint(parameters) == UrgencyHint::High);
        }

        SECTION("should parse low")
        {
            parameters["urgency_hint"] = "low";
            REQUIRE(Params::ParseUrgencyHint(parameters) == UrgencyHint::Low);
        }

        SECTION("should parse medium")
        {
            parameters["urgency_hint"] = "medium";
            REQUIRE(Params::ParseUrgencyHint(parameters) == UrgencyHint::Medium);
        }
    }

    SECTION("when the hint is capitalized")
    {
        // The comparison is exact, so "High" is not "high". Pinned because a
        // model capitalizing its output would silently drop to Medium rather
        // than fail, and nothing downstream would say so.
        parameters["urgency_hint"] = "High";

        SECTION("should fall back to medium")
        {
            REQUIRE(Params::ParseUrgencyHint(parameters) == UrgencyHint::Medium);
        }
    }

    SECTION("when the hint is unrecognized")
    {
        parameters["urgency_hint"] = "catastrophic";

        SECTION("should fall back to medium")
        {
            REQUIRE(Params::ParseUrgencyHint(parameters) == UrgencyHint::Medium);
        }
    }

    SECTION("when the hint is not a string")
    {
        parameters["urgency_hint"] = 2;

        SECTION("should fall back to medium")
        {
            REQUIRE(Params::ParseUrgencyHint(parameters) == UrgencyHint::Medium);
        }
    }

    SECTION("when the hint is absent")
    {
        parameters = json::object({{"somethingElse", "high"}});

        SECTION("should fall back to medium")
        {
            REQUIRE(Params::ParseUrgencyHint(parameters) == UrgencyHint::Medium);
        }
    }

    SECTION("when the parameters are not an object")
    {
        parameters = json::array({"high"});

        SECTION("should fall back to medium")
        {
            REQUIRE(Params::ParseUrgencyHint(parameters) == UrgencyHint::Medium);
        }
    }
}

TEST_CASE("BeatParamHelpers::ResolveLiveSenderActor", "[BeatParamHelpers][engine]")
{
    // Happy path, re-run per leaf: a live actor sitting in the engine's form
    // table under the id the parameters named. Each case below kills, disables,
    // or removes it.
    EngineMock engine;
    RE::Actor* sender = engine.AddActor(kSenderFormID);
    std::string reason = "(untouched)";

    SECTION("when the sender is alive and enabled")
    {
        SECTION("should hand back the actor")
        {
            REQUIRE(Params::ResolveLiveSenderActor(kSenderFormID, &reason) == sender);
        }

        SECTION("should leave the failure reason alone")
        {
            (void)Params::ResolveLiveSenderActor(kSenderFormID, &reason);
            REQUIRE(reason == "(untouched)");
        }
    }

    SECTION("when the form is not in the table")
    {
        // The gap between the beat picking a sender and dispatching to them:
        // a cell can unload and take the form with it.
        SECTION("should say the sender no longer resolves")
        {
            REQUIRE(Params::ResolveLiveSenderActor(kSenderFormID + 1, &reason) == nullptr);
            REQUIRE(reason == "sender_no_longer_resolves");
        }
    }

    SECTION("when the sender has died")
    {
        engine.forms.actorIsDead = true;

        SECTION("should say the sender died during compose")
        {
            // A distinct reason from "no longer resolves" on purpose: a dead
            // sender means the world moved on mid-compose, which is worth
            // seeing separately in the rejection census.
            REQUIRE(Params::ResolveLiveSenderActor(kSenderFormID, &reason) == nullptr);
            REQUIRE(reason == "sender_died_during_compose");
        }
    }

    SECTION("when the sender has been disabled")
    {
        engine.forms.actorIsDisabled = true;

        SECTION("should say the sender was disabled during compose")
        {
            REQUIRE(Params::ResolveLiveSenderActor(kSenderFormID, &reason) == nullptr);
            REQUIRE(reason == "sender_disabled_during_compose");
        }
    }

    SECTION("when the caller wants no failure reason")
    {
        SECTION("should reject without writing anywhere")
        {
            REQUIRE(Params::ResolveLiveSenderActor(kSenderFormID + 1, nullptr) == nullptr);
        }
    }
}
