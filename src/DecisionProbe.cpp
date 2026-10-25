#include <DecisionProbe.h>

#include <logger.h>

#include <algorithm>
#include <format>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace NarrativeEngine::DecisionProbe
{
    namespace
    {
        std::string KeyAt(std::size_t index)
        {
            return std::format("c{}", index + 1);
        }

        std::vector<std::string> Words(std::string_view sentence)
        {
            std::vector<std::string> out;
            std::string word;
            const auto flush = [&] {
                while (!word.empty() && std::string_view{".,;:!?"}.find(word.back()) != std::string_view::npos) {
                    word.pop_back();
                }
                if (!word.empty()) {
                    out.push_back(std::move(word));
                }
                word.clear();
            };
            for (const char ch : sentence) {
                if (ch == ' ') {
                    flush();
                } else {
                    word.push_back(ch);
                }
            }
            flush();
            return out;
        }

        // Whether `inner` stands in `outer` as whole words.
        bool Within(std::string_view inner, std::string_view outer)
        {
            return std::format(" {} ", outer).find(std::format(" {} ", inner)) != std::string::npos;
        }

        bool Accepted(const Question& question, std::string_view choice)
        {
            if (question.answers.empty()) {
                return choice == kNotNamed;
            }
            return std::find(question.answers.begin(), question.answers.end(), choice) != question.answers.end();
        }

        std::string Percent(double p)
        {
            return std::format("{:.0f}%", 100.0 * p);
        }

        std::string_view VerdictWord(Verdict v)
        {
            switch (v) {
            case Verdict::Right:
                return "right";
            case Verdict::Near:
                return "NEAR";
            case Verdict::Wrong:
                return "WRONG";
            case Verdict::NoAnswer:
                break;
            }
            return "NO ANSWER";
        }

        // A tally of verdicts.
        struct Tally
        {
            std::size_t right = 0;
            std::size_t close = 0;
            std::size_t wrong = 0;

            void Add(Verdict v)
            {
                right += v == Verdict::Right ? 1 : 0;
                close += v == Verdict::Near ? 1 : 0;
                wrong += v == Verdict::Wrong ? 1 : 0;
            }

            [[nodiscard]] std::string Say() const
            {
                return std::format("{} right, {} near, {} wrong of {}", right, close, wrong, right + close + wrong);
            }
        };
    } // namespace

    const std::vector<Round>& Rounds()
    {
        // Sentences shaped like the finishes plots write, each naming several
        // things, and one that is a finish that went wrong in play: it says
        // whom the proof is shown to and not whom it is against.
        static const std::vector<Round> rounds{
            {"purchase",
             "Bob ran to Whiterun to purchase 9 red claws from Alan Grimes",
             {
                 {"what", {"9 red claws", "red claws"}},
                 {"seller", {"Alan Grimes"}},
                 {"buyer", {"Bob"}},
                 {"where", {"Whiterun"}},
             }},
            {"expose",
             "Show a merchant's ledger to Jarl Elisif as proof against Erikur",
             {
                 {"what", {"a merchant's ledger", "merchant's ledger"}},
                 {"shown_to", {"Jarl Elisif", "Elisif"}},
                 {"against", {"Erikur"}},
             }},
            {"expose_unnamed",
             "Expose a merchant's ledger to Jarl Elisif",
             {
                 {"against", {}},
                 {"shown_to", {"Jarl Elisif", "Elisif"}},
             }},
            {"denounce",
             "Denounce Vignar Gray-Mane to Jarl Balgruuf for hiding stolen gold in Whiterun",
             {
                 {"who", {"Vignar Gray-Mane"}},
                 {"to", {"Jarl Balgruuf", "Balgruuf"}},
                 {"where", {"Whiterun"}},
             }},
            {"plant",
             "Hide a bottle of skooma among Hulda's belongings at the Bannered Mare",
             {
                 {"what", {"a bottle of skooma", "bottle of skooma"}},
                 {"whose", {"Hulda's"}},
                 {"where", {"the Bannered Mare", "Bannered Mare"}},
             }},
            {"steal",
             "Steal a deed to a property from Maven Black-Briar in Riften",
             {
                 {"what", {"a deed to a property", "deed to a property"}},
                 {"from", {"Maven Black-Briar"}},
                 {"where", {"Riften"}},
             }},
            {"gift",
             "Bring a gold diamond necklace to Elisif the Fair in Solitude",
             {
                 {"what", {"a gold diamond necklace", "gold diamond necklace"}},
                 {"to", {"Elisif the Fair"}},
             }},
            // As long as a round may be, for the longest list of choices.
            {"follow",
             "Hrolfdir asked Sorli to follow the merchant Fenja from the Riften docks to the Ragged Flagon and report back",
             {
                 {"followed", {"Fenja", "the merchant Fenja", "merchant Fenja"}},
                 {"follower", {"Sorli"}},
                 {"end", {"the Ragged Flagon", "Ragged Flagon"}},
             }},
        };
        return rounds;
    }

    std::vector<std::string> Runs(std::string_view sentence)
    {
        const auto words = Words(sentence);
        std::vector<std::string> out;
        std::unordered_set<std::string> seen;
        for (std::size_t start = 0; start < words.size(); ++start) {
            std::string run;
            for (std::size_t end = start; end < words.size(); ++end) {
                run += (end == start ? "" : " ") + words[end];
                if (seen.insert(run).second) {
                    out.push_back(run);
                }
            }
        }
        return out;
    }

    std::vector<std::string> Choices(const Round& round, bool reversed)
    {
        auto out = Runs(round.sentence);
        out.emplace_back(kNotNamed);
        if (reversed) {
            std::reverse(out.begin(), out.end());
        }
        return out;
    }

    std::string Context(const Round& round, const Question& question, bool reversed)
    {
        // The choice that the sentence does not say goes as a flag, for
        // probe.prompt to word; every other choice is words of the sentence.
        nlohmann::json options = nlohmann::json::array();
        const auto choices = Choices(round, reversed);
        for (std::size_t i = 0; i < choices.size(); ++i) {
            const bool none = choices[i] == kNotNamed;
            options.push_back({{"key", KeyAt(i)}, {"text", none ? std::string{} : choices[i]}, {"none", none}});
        }
        // The question is worded by probe.prompt, by its round and its id.
        nlohmann::json ctx;
        ctx["sentence"] = round.sentence;
        ctx["round"] = round.id;
        ctx["question"] = question.id;
        ctx["options"] = std::move(options);
        return ctx.dump();
    }

    Finding Judge(const Round& round,
                  const Question& question,
                  bool reversed,
                  const SkyrimNetAPI::DecisionResult& result,
                  std::chrono::milliseconds latency)
    {
        Finding f;
        f.round = round.id;
        f.question = question.id;
        f.reversed = reversed;
        f.latencyMs = latency.count();
        f.expected = question.answers.empty() ? std::string{kNotNamed} : question.answers.front();

        const auto choices = Choices(round, reversed);
        f.choices = choices.size();
        std::unordered_set<std::string> acceptedKeys;
        for (std::size_t i = 0; i < choices.size(); ++i) {
            if (Accepted(question, choices[i])) {
                acceptedKeys.insert(KeyAt(i));
                if (f.rightAt == 0 || choices[i] == f.expected) {
                    f.rightAt = i + 1;
                }
            }
        }

        if (!result.ok) {
            f.error = result.error.empty() ? std::string{"no answer"} : result.error;
            return f;
        }

        const auto key = result.Choice(kQuestion);
        f.chosen = key;
        bool offered = false;
        for (std::size_t i = 0; i < choices.size(); ++i) {
            if (KeyAt(i) == key) {
                f.chosen = choices[i];
                offered = true;
            }
        }

        if (offered && Accepted(question, f.chosen)) {
            f.verdict = Verdict::Right;
        } else if (offered && f.chosen != kNotNamed
                   && std::any_of(question.answers.begin(), question.answers.end(), [&f](const std::string& a) {
                          return Within(a, f.chosen) || Within(f.chosen, a);
                      })) {
            f.verdict = Verdict::Near;
        } else {
            f.verdict = Verdict::Wrong;
        }

        for (const auto& [asked, answer] : result.answers) {
            if (asked != kQuestion) {
                continue;
            }
            for (const auto& [option, p] : answer.probabilities) {
                if (acceptedKeys.contains(option)) {
                    f.rightChance += p;
                }
            }
        }
        return f;
    }

    std::vector<std::string> Report(const std::vector<Finding>& findings)
    {
        std::vector<std::string> lines;
        Tally all, asBuilt, reversed, unnamed;
        long long totalMs = 0;
        std::size_t answered = 0;
        std::size_t mostChoices = 0;

        for (const auto& f : findings) {
            const auto order = f.reversed ? "reversed" : "as built";
            if (f.verdict == Verdict::NoAnswer) {
                lines.push_back(std::format("DecisionProbe: {}/{} ({}): NO ANSWER after {} ms of {} choices -- {}",
                                            f.round,
                                            f.question,
                                            order,
                                            f.latencyMs,
                                            f.choices,
                                            f.error));
                continue;
            }
            lines.push_back(std::format("DecisionProbe: {}/{} ({}): {} -- chose \"{}\"; \"{}\" was #{} of {}{}, {} ms",
                                        f.round,
                                        f.question,
                                        order,
                                        VerdictWord(f.verdict),
                                        f.chosen,
                                        f.expected,
                                        f.rightAt,
                                        f.choices,
                                        f.rightChance > 0.0
                                            ? std::format(", the right answers given {}", Percent(f.rightChance))
                                            : std::string{},
                                        f.latencyMs));

            all.Add(f.verdict);
            (f.reversed ? reversed : asBuilt).Add(f.verdict);
            if (f.expected == kNotNamed) {
                unnamed.Add(f.verdict);
            }
            totalMs += f.latencyMs;
            ++answered;
            mostChoices = std::max(mostChoices, f.choices);
        }

        lines.push_back(std::format("DecisionProbe: {} ({} asked); as built {}; reversed {}; not named in the sentence "
                                    "{}; up to {} choices; {} ms a request on average",
                                    all.Say(),
                                    findings.size(),
                                    asBuilt.Say(),
                                    reversed.Say(),
                                    unnamed.Say(),
                                    mostChoices,
                                    answered > 0 ? totalMs / static_cast<long long>(answered) : 0));
        return lines;
    }

    std::vector<Finding> Run(const PlotThread::Token& pt)
    {
        std::vector<Finding> findings;
        for (const auto& round : Rounds()) {
            for (const auto& question : round.questions) {
                for (const bool reversed : {false, true}) {
                    const auto started = std::chrono::steady_clock::now();
                    const auto result =
                        SkyrimNetAPI::SendCustomDecision(pt, kTemplate, Context(round, question, reversed));
                    const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started);
                    findings.push_back(Judge(round, question, reversed, result, latency));
                }
            }
        }
        for (const auto& line : Report(findings)) {
            logger::info("{}", line);
        }
        return findings;
    }
} // namespace NarrativeEngine::DecisionProbe
