#pragma once

#include <PlotThread.h>
#include <SkyrimNetAPI.h>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

// DecisionProbe — rounds of decision requests, sent on demand from the Plots
// tab, that settle what plot planning can ask SkyrimNet's decision model to do.
//
// The first two probes settled that the model chooses well when every choice
// says what it is (Step 2 of the rigid-steps milestone). This one asks whether
// it can read a sentence: given a finish such as "Bob ran to Whiterun to
// purchase 9 red claws from Alan Grimes", and every run of its consecutive
// words as a choice, does it pick out the words that answer "Who was the
// seller?" If it can, a finish written as prose could be read for who fills
// each role of its step.
//
// Each question is asked twice, with the runs in the order they were built and
// reversed, so a model that favours one end of a long list shows it. Every
// question also offers kNotNamed, because a finish that does not name a role
// is the case that went wrong in play.
//
// The template is prompts/decisions/narrative_engine/probe.prompt. Findings
// are written to NarrativeEngine.log as `DecisionProbe:` lines.
namespace NarrativeEngine::DecisionProbe
{
    inline constexpr const char* kTemplate = "narrative_engine/probe";
    inline constexpr const char* kQuestion = "pick";

    // The choice for a question its sentence does not answer.
    inline constexpr std::string_view kNotNamed = "(not named in the sentence)";

    // The longest sentence a round may use.
    inline constexpr std::size_t kMaxSentence = 120;

    struct Question
    {
        std::string id;
        std::string text;
        // Every run of the sentence's words that answers it: with and without
        // an article, say. Empty when the sentence does not answer it, which
        // makes kNotNamed the right choice.
        std::vector<std::string> answers;
    };

    struct Round
    {
        std::string id;
        std::string sentence;
        std::vector<Question> questions;
    };

    // The rounds, in the order they are asked.
    [[nodiscard]] const std::vector<Round>& Rounds();

    // Every run of consecutive words in `sentence`, shortest first from each
    // starting word in turn: "Bob", "Bob ran", ..., then "ran", .... Words are
    // split on spaces, with trailing sentence punctuation dropped; a run whose
    // words are the same as an earlier one's is left out.
    [[nodiscard]] std::vector<std::string> Runs(std::string_view sentence);

    // The choices for a question, in the order asked: the runs, then
    // kNotNamed; reversed, the other way round.
    [[nodiscard]] std::vector<std::string> Choices(const Round& round, bool reversed);

    // A question's request context, keyed c1 onward in the order asked.
    [[nodiscard]] std::string Context(const Round& round, const Question& question, bool reversed);

    enum class Verdict
    {
        NoAnswer,
        Right,
        // Not an answer, but holding one or held in one: "from Alan Grimes"
        // for "Alan Grimes". Close enough that trimming could make it right.
        Near,
        Wrong
    };

    struct Finding
    {
        std::string round;
        std::string question;
        bool reversed = false;
        Verdict verdict = Verdict::NoAnswer;
        std::string chosen;       // the choice's words, or what came back if it was not one
        std::string expected;     // the first accepted answer, or kNotNamed
        std::size_t rightAt = 0;  // that answer's 1-based position in this order
        std::size_t choices = 0;  // how many were offered
        double rightChance = 0.0; // the probability the model gave the accepted answers together
        long long latencyMs = 0;
        std::string error;
    };

    // Pure: what one answer to one question shows.
    [[nodiscard]] Finding Judge(const Round& round,
                                const Question& question,
                                bool reversed,
                                const SkyrimNetAPI::DecisionResult& result,
                                std::chrono::milliseconds latency);

    // Pure: one line per finding, then the totals -- overall, by order, and
    // for the questions their sentence does not answer.
    [[nodiscard]] std::vector<std::string> Report(const std::vector<Finding>& findings);

    // Asks every question in both orders and logs the report. Plot worker
    // only, because each request blocks and nothing else waits on that thread.
    std::vector<Finding> Run(const PlotThread::Token& pt);
} // namespace NarrativeEngine::DecisionProbe
