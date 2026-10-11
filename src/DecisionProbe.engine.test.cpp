#include <DecisionProbe.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>

// Tests for the decision API probe.
//
// What matters is that a result is read the way the Milestone 2 doc's Step 2
// needs it read: each of its open questions gets its own yes or no, and a
// result that answers one but not another says which. A probe that reported
// "it worked" on any successful response would settle nothing.

namespace
{
    namespace DecisionProbe = NarrativeEngine::DecisionProbe;
    namespace SkyrimNet = NarrativeEngine::SkyrimNetAPI;

    SkyrimNet::DecisionAnswer Choice(std::string key)
    {
        SkyrimNet::DecisionAnswer a;
        a.type = "choice";
        a.choice = std::move(key);
        return a;
    }

    SkyrimNet::DecisionAnswer YesNo(double p)
    {
        SkyrimNet::DecisionAnswer a;
        a.type = "noul";
        a.noul = p;
        return a;
    }

    SkyrimNet::DecisionResult Answered(std::string capital, std::string fish, bool withCold = true)
    {
        SkyrimNet::DecisionResult r;
        r.ok = true;
        r.answers.emplace_back("capital", Choice(std::move(capital)));
        r.answers.emplace_back("fish", Choice(std::move(fish)));
        if (withCold) {
            r.answers.emplace_back("cold", YesNo(0.95));
        }
        return r;
    }

    constexpr std::chrono::milliseconds kLatency{1234};
} // namespace

TEST_CASE("DecisionProbe::Context", "[DecisionProbe][engine]")
{
    const auto ctx = nlohmann::json::parse(DecisionProbe::Context());

    SECTION("when built")
    {
        SECTION("should offer twenty places keyed c1 to c20")
        {
            REQUIRE(ctx["places"].size() == 20);
            REQUIRE(ctx["places"][0]["key"] == "c1");
            REQUIRE(ctx["places"][19]["key"] == "c20");
        }

        SECTION("should put the right answer under the key the probe checks for")
        {
            // The two are written separately -- a list and a constant -- so a
            // reordered list would quietly make every right answer read wrong.
            bool found = false;
            for (const auto& row : ctx["places"]) {
                if (row["label"] == "Markarth") {
                    REQUIRE(row["key"] == DecisionProbe::kCapitalKey);
                    found = true;
                }
            }
            REQUIRE(found);
        }
    }
}

TEST_CASE("DecisionProbe::Judge", "[DecisionProbe][engine]")
{
    SECTION("when every question is answered correctly")
    {
        const auto v = DecisionProbe::Judge(Answered("c13", "c3"), kLatency);

        SECTION("should report every finding as settled")
        {
            REQUIRE(v.answered);
            REQUIRE(v.loopRendered);
            REQUIRE(v.keysVerbatim);
            REQUIRE(v.twentyCorrect);
            REQUIRE(v.fiveCorrect);
            REQUIRE(v.allQuestionsAnswered);
        }

        SECTION("should record the latency")
        {
            REQUIRE(v.latencyMs == 1234);
        }
    }

    SECTION("when the request fails")
    {
        SkyrimNet::DecisionResult failed;
        failed.error = "no_decisions_route";
        failed.message = "no decision provider";
        const auto v = DecisionProbe::Judge(failed, kLatency);

        SECTION("should say it was not answered, and why")
        {
            REQUIRE_FALSE(v.answered);
            REQUIRE(v.lines.size() == 1);
            REQUIRE(v.lines.front().find("no_decisions_route") != std::string::npos);
        }
    }

    SECTION("when the capital comes back as a key that was never offered")
    {
        // What a template whose loop rendered nothing would produce: the model
        // has no keys to choose from and answers with whatever it likes.
        const auto v = DecisionProbe::Judge(Answered("Markarth", "c3"), kLatency);

        SECTION("should say the generated options were not chosen from")
        {
            REQUIRE_FALSE(v.loopRendered);
            REQUIRE_FALSE(v.keysVerbatim);
            REQUIRE_FALSE(v.twentyCorrect);
        }
    }

    SECTION("when twenty options go wrong and five go right")
    {
        const auto v = DecisionProbe::Judge(Answered("c1", "c3"), kLatency);

        SECTION("should tell the two apart")
        {
            REQUIRE(v.loopRendered);
            REQUIRE_FALSE(v.twentyCorrect);
            REQUIRE(v.fiveCorrect);
        }
    }

    SECTION("when one question is missing from the answer")
    {
        const auto v = DecisionProbe::Judge(Answered("c13", "c3", false), kLatency);

        SECTION("should say not every question was answered")
        {
            REQUIRE_FALSE(v.allQuestionsAnswered);
            REQUIRE(v.twentyCorrect);
        }
    }
}
