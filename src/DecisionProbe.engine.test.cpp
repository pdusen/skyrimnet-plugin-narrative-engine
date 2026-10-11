#include <DecisionProbe.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <set>
#include <string>

// Tests for the decision API probe.
//
// The probe's worth is in reading each answer correctly: a round marked right
// when the model chose wrong, or a position recorded wrongly, would make the
// report say the opposite of what happened. So the suite pins the rounds' own
// consistency, the keys each order produces, and the reading of a result.

namespace
{
    namespace DecisionProbe = NarrativeEngine::DecisionProbe;
    namespace SkyrimNet = NarrativeEngine::SkyrimNetAPI;

    SkyrimNet::DecisionResult Chose(const std::string& key, double p = 0.0, const std::string& pKey = {})
    {
        SkyrimNet::DecisionResult r;
        r.ok = true;
        SkyrimNet::DecisionAnswer a;
        a.type = "choice";
        a.choice = key;
        if (!pKey.empty()) {
            a.probabilities.emplace_back(pKey, p);
        }
        r.answers.emplace_back(DecisionProbe::kQuestion, a);
        return r;
    }

    const DecisionProbe::Round& RoundNamed(const std::string& id)
    {
        for (const auto& r : DecisionProbe::Rounds()) {
            if (r.id == id) {
                return r;
            }
        }
        FAIL("no round " << id);
        return DecisionProbe::Rounds().front();
    }

    constexpr std::chrono::milliseconds kLatency{200};
} // namespace

TEST_CASE("DecisionProbe rounds", "[DecisionProbe][engine]")
{
    SECTION("when every round is read")
    {
        SECTION("should carry a background for every option and a right answer among them")
        {
            for (const auto& round : DecisionProbe::Rounds()) {
                INFO(round.id);
                std::set<std::string> names;
                for (const auto& option : round.options) {
                    REQUIRE_FALSE(option.background.empty());
                    REQUIRE(names.insert(option.name).second);
                }
                REQUIRE(names.contains(round.correct));
            }
        }

        SECTION("should put the right answer at a different position in each order")
        {
            for (const auto& round : DecisionProbe::Rounds()) {
                INFO(round.id);
                REQUIRE(DecisionProbe::CorrectKey(round, false) != DecisionProbe::CorrectKey(round, true));
            }
        }

        SECTION("should place background both ways, so the two can be compared")
        {
            bool onLine = false;
            bool inState = false;
            for (const auto& round : DecisionProbe::Rounds()) {
                (round.backgroundInState ? inState : onLine) = true;
            }
            REQUIRE(onLine);
            REQUIRE(inState);
        }
    }

    SECTION("when a round's context is built")
    {
        const auto& capital = RoundNamed("capital");
        const auto asWritten = nlohmann::json::parse(DecisionProbe::Context(capital, false));
        const auto reversed = nlohmann::json::parse(DecisionProbe::Context(capital, true));

        SECTION("should key the options c1 onward and carry each one's background")
        {
            REQUIRE(asWritten["options"].size() == 20);
            REQUIRE(asWritten["options"][0]["key"] == "c1");
            REQUIRE(asWritten["options"][12]["name"] == "Markarth");
            REQUIRE_FALSE(asWritten["options"][12]["background"].get<std::string>().empty());
        }

        SECTION("should reverse the options, not the keys")
        {
            REQUIRE(reversed["options"][0]["key"] == "c1");
            REQUIRE(reversed["options"][0]["name"] == "Darkwater Crossing");
            REQUIRE(DecisionProbe::CorrectKey(capital, false) == "c13");
            REQUIRE(DecisionProbe::CorrectKey(capital, true) == "c8");
        }
    }
}

TEST_CASE("DecisionProbe::Judge", "[DecisionProbe][engine]")
{
    const auto& capital = RoundNamed("capital");

    SECTION("when the right key comes back")
    {
        const auto f = DecisionProbe::Judge(capital, false, Chose("c13", 0.8, "c13"), kLatency);

        SECTION("should mark it right, by name, with its position and probability")
        {
            REQUIRE(f.answered);
            REQUIRE(f.right);
            REQUIRE(f.chosen == "Markarth");
            REQUIRE(f.rightAt == 13);
            REQUIRE(f.rightChance == 0.8);
        }
    }

    SECTION("when the same key comes back for the reversed order")
    {
        const auto f = DecisionProbe::Judge(capital, true, Chose("c13"), kLatency);

        SECTION("should mark it wrong, because c13 is somebody else there")
        {
            REQUIRE_FALSE(f.right);
            REQUIRE(f.chosen == "Winterhold");
            REQUIRE(f.rightAt == 8);
        }
    }

    SECTION("when a key that was never offered comes back")
    {
        const auto f = DecisionProbe::Judge(capital, false, Chose("Markarth"), kLatency);

        SECTION("should mark it wrong and keep what came back")
        {
            REQUIRE_FALSE(f.right);
            REQUIRE(f.chosen == "Markarth");
        }
    }

    SECTION("when the request fails")
    {
        SkyrimNet::DecisionResult failed;
        failed.error = "no_decisions_route";
        const auto f = DecisionProbe::Judge(capital, false, failed, kLatency);

        SECTION("should record it as unanswered, with the reason")
        {
            REQUIRE_FALSE(f.answered);
            REQUIRE(f.error == "no_decisions_route");
        }
    }
}

TEST_CASE("DecisionProbe::Report", "[DecisionProbe][engine]")
{
    const auto& capital = RoundNamed("capital");
    const auto& state = RoundNamed("capital_state");
    const std::vector<DecisionProbe::Finding> findings{
        DecisionProbe::Judge(capital, false, Chose("c13"), kLatency),
        DecisionProbe::Judge(capital, true, Chose("c1"), kLatency),
        DecisionProbe::Judge(state, false, Chose("c13"), kLatency),
    };
    const auto lines = DecisionProbe::Report(findings);

    SECTION("when findings are reported")
    {
        SECTION("should give one line per finding and a total")
        {
            REQUIRE(lines.size() == 4);
            REQUIRE(lines[0].find("capital (as written): right") != std::string::npos);
            REQUIRE(lines[1].find("WRONG") != std::string::npos);
        }

        SECTION("should total right answers by where the background went")
        {
            REQUIRE(lines.back().find("2 of 3 right") != std::string::npos);
            REQUIRE(lines.back().find("background on each option's line 1 of 2") != std::string::npos);
            REQUIRE(lines.back().find("in a state block 1 of 1") != std::string::npos);
        }
    }
}
