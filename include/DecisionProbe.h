#pragma once

#include <PlotThread.h>
#include <SkyrimNetAPI.h>

#include <chrono>
#include <string>
#include <vector>

// DecisionProbe — rounds of decision requests, sent on demand from the Plots
// tab, that settle what plot planning has to put in front of SkyrimNet's
// decision model for it to choose well.
//
// The first probe asked for the capital of the Reach out of twenty bare place
// names and got Solitude: the decision model does not know Skyrim. So every
// decision a plot makes has to carry what the model needs to know about each
// choice, and these rounds measure whether it chooses well when it does.
//
// Each round is a question with one right answer among choices that each come
// with background. Each is asked twice, in its authored order and reversed, so
// the right answer sits at two different positions and a model that favours
// the top of a list shows it. The rounds also vary WHERE the background goes:
// on each option's own line, or in a [ state ] block the options refer to --
// the second being the only place a multi-sentence biography fits.
//
// The template is prompts/decisions/narrative_engine/probe.prompt. Findings
// are written to NarrativeEngine.log as `DecisionProbe:` lines.
namespace NarrativeEngine::DecisionProbe
{
    inline constexpr const char* kTemplate = "narrative_engine/probe";
    inline constexpr const char* kQuestion = "pick";

    struct Option
    {
        std::string name;
        std::string background;
    };

    struct Round
    {
        std::string id;
        // What is going on, as the [ state situation ] block.
        std::string situation;
        std::string question;
        std::vector<Option> options;
        // The right option's name.
        std::string correct;
        // Background in a [ state choices ] block rather than on each line.
        bool backgroundInState = false;
    };

    // The rounds, in the order they are asked.
    [[nodiscard]] const std::vector<Round>& Rounds();

    // A round's request context, keyed c1 onward in the order asked.
    [[nodiscard]] std::string Context(const Round& round, bool reversed);

    // The key the right answer carries when asked in this order.
    [[nodiscard]] std::string CorrectKey(const Round& round, bool reversed);

    struct Finding
    {
        std::string round;
        bool reversed = false;
        bool answered = false;
        bool right = false;
        std::string chosen;       // the option's name, or what came back if it was not one
        std::size_t rightAt = 0;  // the right answer's 1-based position in this order
        double rightChance = 0.0; // the probability the model gave the right answer, when it sent them
        long long latencyMs = 0;
        std::string error;
    };

    // Pure: what one answer to one round shows.
    [[nodiscard]] Finding Judge(const Round& round,
                                bool reversed,
                                const SkyrimNetAPI::DecisionResult& result,
                                std::chrono::milliseconds latency);

    // Pure: one line per finding, then the totals -- overall, by where the
    // background went, and by which half of the list the right answer was in.
    [[nodiscard]] std::vector<std::string> Report(const std::vector<Finding>& findings);

    // Asks every round in both orders and logs the report. Plot worker only,
    // because each request blocks and nothing else waits on that thread.
    std::vector<Finding> Run(const PlotThread::Token& pt);
} // namespace NarrativeEngine::DecisionProbe
