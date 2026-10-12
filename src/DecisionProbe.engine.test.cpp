#include <DecisionProbe.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <string>

// Tests for the decision API probe.
//
// The probe's worth is in reading each answer correctly: a question marked
// right when the model chose wrong, or a position recorded wrongly, would make
// the report say the opposite of what happened. So the suite pins the runs a
// sentence gives, the rounds' own consistency, and the reading of a result.

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

    const DecisionProbe::Question& QuestionOf(const DecisionProbe::Round& round, const std::string& id)
    {
        for (const auto& q : round.questions) {
            if (q.id == id) {
                return q;
            }
        }
        FAIL("no question " << id);
        return round.questions.front();
    }

    // The key a choice carries in this order.
    std::string KeyOf(const DecisionProbe::Round& round, bool reversed, std::string_view text)
    {
        const auto choices = DecisionProbe::Choices(round, reversed);
        const auto it = std::find(choices.begin(), choices.end(), text);
        REQUIRE(it != choices.end());
        return "c" + std::to_string(it - choices.begin() + 1);
    }

    constexpr std::chrono::milliseconds kLatency{200};
} // namespace

TEST_CASE("DecisionProbe runs", "[DecisionProbe][engine]")
{
    SECTION("when a sentence is split into runs of words")
    {
        const auto runs = DecisionProbe::Runs("Bob ran to Whiterun.");

        SECTION("should give every run of consecutive words, shortest first from each word in turn")
        {
            REQUIRE(runs
                    == std::vector<std::string>{"Bob",
                                                "Bob ran",
                                                "Bob ran to",
                                                "Bob ran to Whiterun",
                                                "ran",
                                                "ran to",
                                                "ran to Whiterun",
                                                "to",
                                                "to Whiterun",
                                                "Whiterun"});
        }
    }

    SECTION("when a word comes twice")
    {
        const auto runs = DecisionProbe::Runs("to Bob to");

        SECTION("should offer the same words once")
        {
            REQUIRE(runs == std::vector<std::string>{"to", "to Bob", "to Bob to", "Bob", "Bob to"});
        }
    }

    SECTION("when the example sentence is split")
    {
        SECTION("should give its twelve words' seventy-eight runs, less the second \"to\"")
        {
            REQUIRE(DecisionProbe::Runs("Bob ran to Whiterun to purchase 9 red claws from Alan Grimes").size() == 77);
        }
    }
}

TEST_CASE("DecisionProbe rounds", "[DecisionProbe][engine]")
{
    SECTION("when every round is read")
    {
        SECTION("should keep its sentence within the limit, and every answer one of its runs")
        {
            for (const auto& round : DecisionProbe::Rounds()) {
                INFO(round.id);
                REQUIRE(round.sentence.size() <= DecisionProbe::kMaxSentence);
                REQUIRE_FALSE(round.questions.empty());
                const auto runs = DecisionProbe::Runs(round.sentence);
                for (const auto& question : round.questions) {
                    for (const auto& answer : question.answers) {
                        INFO(question.id << ": " << answer);
                        REQUIRE(std::find(runs.begin(), runs.end(), answer) != runs.end());
                    }
                }
            }
        }

        SECTION("should ask at least one question its sentence does not answer")
        {
            const bool some = std::any_of(DecisionProbe::Rounds().begin(), DecisionProbe::Rounds().end(), [](auto& r) {
                return std::any_of(r.questions.begin(), r.questions.end(), [](auto& q) { return q.answers.empty(); });
            });
            REQUIRE(some);
        }
    }

    SECTION("when a question's choices are built")
    {
        const auto& round = RoundNamed("purchase");

        SECTION("should offer every run, then that it is not named")
        {
            const auto choices = DecisionProbe::Choices(round, false);
            REQUIRE(choices.size() == DecisionProbe::Runs(round.sentence).size() + 1);
            REQUIRE(choices.front() == "Bob");
            REQUIRE(choices.back() == DecisionProbe::kNotNamed);
        }

        SECTION("should reverse them for the second asking")
        {
            const auto choices = DecisionProbe::Choices(round, true);
            REQUIRE(choices.front() == DecisionProbe::kNotNamed);
            REQUIRE(choices.back() == "Bob");
        }

        SECTION("should key them c1 onward and carry the sentence and the question")
        {
            const auto ctx = nlohmann::json::parse(DecisionProbe::Context(round, QuestionOf(round, "seller"), false));
            REQUIRE(ctx["sentence"] == round.sentence);
            REQUIRE(ctx["question"] == "Who was the seller?");
            REQUIRE(ctx["options"][0]["key"] == "c1");
            REQUIRE(ctx["options"][0]["text"] == "Bob");
        }
    }
}

TEST_CASE("DecisionProbe::Judge", "[DecisionProbe][engine]")
{
    const auto& round = RoundNamed("purchase");
    const auto& seller = QuestionOf(round, "seller");

    SECTION("when the answer's own words come back")
    {
        const auto key = KeyOf(round, false, "Alan Grimes");
        const auto f = DecisionProbe::Judge(round, seller, false, Chose(key, 0.9, key), kLatency);

        SECTION("should mark it right, with where it sat and the probability given it")
        {
            REQUIRE(f.verdict == DecisionProbe::Verdict::Right);
            REQUIRE(f.chosen == "Alan Grimes");
            REQUIRE(f.rightAt == std::stoul(key.substr(1)));
            REQUIRE(f.rightChance == 0.9);
        }
    }

    SECTION("when either accepted answer comes back")
    {
        const auto& what = QuestionOf(round, "what");
        const auto f = DecisionProbe::Judge(round, what, true, Chose(KeyOf(round, true, "red claws")), kLatency);

        SECTION("should mark it right")
        {
            REQUIRE(f.verdict == DecisionProbe::Verdict::Right);
        }
    }

    SECTION("when a run holding the answer comes back")
    {
        const auto f =
            DecisionProbe::Judge(round, seller, false, Chose(KeyOf(round, false, "from Alan Grimes")), kLatency);

        SECTION("should mark it near")
        {
            REQUIRE(f.verdict == DecisionProbe::Verdict::Near);
        }
    }

    SECTION("when part of the answer comes back")
    {
        const auto f = DecisionProbe::Judge(round, seller, false, Chose(KeyOf(round, false, "Grimes")), kLatency);

        SECTION("should mark it near")
        {
            REQUIRE(f.verdict == DecisionProbe::Verdict::Near);
        }
    }

    SECTION("when somebody else comes back, or that the seller is not named")
    {
        SECTION("should mark it wrong")
        {
            REQUIRE(DecisionProbe::Judge(round, seller, false, Chose(KeyOf(round, false, "Bob")), kLatency).verdict
                    == DecisionProbe::Verdict::Wrong);
            REQUIRE(DecisionProbe::Judge(
                        round, seller, false, Chose(KeyOf(round, false, DecisionProbe::kNotNamed)), kLatency)
                        .verdict
                    == DecisionProbe::Verdict::Wrong);
        }
    }

    SECTION("when the sentence does not answer the question")
    {
        const auto& unnamed = RoundNamed("expose_unnamed");
        const auto& against = QuestionOf(unnamed, "against");

        SECTION("should mark not named right, and a person wrong")
        {
            REQUIRE(DecisionProbe::Judge(
                        unnamed, against, false, Chose(KeyOf(unnamed, false, DecisionProbe::kNotNamed)), kLatency)
                        .verdict
                    == DecisionProbe::Verdict::Right);
            REQUIRE(
                DecisionProbe::Judge(unnamed, against, false, Chose(KeyOf(unnamed, false, "Elisif")), kLatency).verdict
                == DecisionProbe::Verdict::Wrong);
        }
    }

    SECTION("when a key that was never offered comes back")
    {
        const auto f = DecisionProbe::Judge(round, seller, false, Chose("c999"), kLatency);

        SECTION("should mark it wrong and keep what came back")
        {
            REQUIRE(f.verdict == DecisionProbe::Verdict::Wrong);
            REQUIRE(f.chosen == "c999");
        }
    }

    SECTION("when the request fails")
    {
        SkyrimNet::DecisionResult failed;
        failed.error = "template_failed";
        const auto f = DecisionProbe::Judge(round, seller, false, failed, kLatency);

        SECTION("should record it as unanswered, with the reason")
        {
            REQUIRE(f.verdict == DecisionProbe::Verdict::NoAnswer);
            REQUIRE(f.error == "template_failed");
        }
    }
}

TEST_CASE("DecisionProbe::Report", "[DecisionProbe][engine]")
{
    const auto& round = RoundNamed("purchase");
    const auto& seller = QuestionOf(round, "seller");
    const std::vector<DecisionProbe::Finding> findings{
        DecisionProbe::Judge(round, seller, false, Chose(KeyOf(round, false, "Alan Grimes")), kLatency),
        DecisionProbe::Judge(round, seller, true, Chose(KeyOf(round, true, "Grimes")), kLatency),
    };

    SECTION("when findings are reported")
    {
        const auto lines = DecisionProbe::Report(findings);

        SECTION("should give one line per finding and a total by verdict and order")
        {
            REQUIRE(lines.size() == 3);
            REQUIRE(lines[0].find("right") != std::string::npos);
            REQUIRE(lines[1].find("NEAR") != std::string::npos);
            REQUIRE(lines[2].find("1 right, 1 near, 0 wrong of 2") != std::string::npos);
            REQUIRE(lines[2].find("as built 1 right, 0 near, 0 wrong of 1") != std::string::npos);
        }
    }
}
