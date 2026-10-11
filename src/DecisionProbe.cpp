#include <DecisionProbe.h>

#include <logger.h>
#include <PlotVerbs.h>

#include <algorithm>
#include <cctype>
#include <format>

#include <nlohmann/json.hpp>

namespace NarrativeEngine::DecisionProbe
{
    namespace
    {
        std::string KeyAt(std::size_t index)
        {
            return std::format("c{}", index + 1);
        }

        // The options in the order a round is asked.
        std::vector<const Option*> Ordered(const Round& round, bool reversed)
        {
            std::vector<const Option*> out;
            for (const auto& option : round.options) {
                out.push_back(&option);
            }
            if (reversed) {
                std::reverse(out.begin(), out.end());
            }
            return out;
        }

        // The twenty places, each with what somebody would need to know to
        // tell which one is the capital of the Reach. Markarth is thirteenth.
        std::vector<Option> Places()
        {
            return {
                {"Whiterun",
                 "a city on the plains at the heart of Skyrim; the Jarl of Whiterun Hold rules from "
                 "Dragonsreach"},
                {"Solitude",
                 "a city built on a stone arch over the sea; capital of Haafingar, where the High King or "
                 "Queen rules from the Blue Palace"},
                {"Windhelm", "the oldest city in Skyrim; capital of Eastmarch, ruled from the Palace of the Kings"},
                {"Riften", "a city of canals and boardwalks; capital of the Rift, ruled from Mistveil Keep"},
                {"Falkreath", "a town among the pine forests of the south; seat of Falkreath Hold"},
                {"Morthal", "a town in the marshes; seat of Hjaalmarch, ruled from Highmoon Hall"},
                {"Dawnstar", "a mining and fishing port on the northern coast; seat of the Pale"},
                {"Winterhold",
                 "a town half fallen into the sea beside the College of Winterhold; seat of the hold of "
                 "the same name"},
                {"Riverwood", "a lumber village in Whiterun Hold"},
                {"Rorikstead", "a farming village in the west of Whiterun Hold"},
                {"Ivarstead", "a village at the foot of the Throat of the World, in the Rift"},
                {"Dragon Bridge", "a lumber village in Haafingar, beside an ancient bridge"},
                {"Markarth",
                 "a city carved into Dwemer stone in the western mountains; the Jarl of the Reach holds "
                 "court there in Understone Keep"},
                {"Shor's Stone", "a mining village in the Rift"},
                {"Karthwasten", "a small mining hamlet in the Reach"},
                {"Kynesgrove", "a mining village in Eastmarch"},
                {"Helgen", "a walled town in Falkreath Hold, left in ruins"},
                {"Raven Rock", "a Dunmer port town on the island of Solstheim"},
                {"Skaal Village", "a Nord village on the island of Solstheim"},
                {"Darkwater Crossing", "a mining settlement in Eastmarch"},
            };
        }

        // Twelve invented people, so that no background makes a claim about
        // a real record. One is plainly the person to send; the rest each have
        // a reason not to be. `longer` gives each the several sentences a
        // plot candidate's biography would run to.
        std::vector<Option> People(bool longer)
        {
            const auto person = [longer](const char* name, const char* line, const char* more) {
                return Option{name, longer ? std::format("{} {}", line, more) : std::string{line}};
            };
            return {
                person("Agna",
                       "the court's cook, loud and friendly, known by name to every stallholder.",
                       "She has worked in the same kitchen for twenty years. She cannot keep a secret and does not "
                       "try to; the whole market knows her laugh."),
                person("Bersi",
                       "a blacksmith who rarely leaves his forge.",
                       "He works from dawn to dusk and is gruff with anybody who interrupts him. He has not set foot "
                       "in the market in months."),
                person("Dagny",
                       "a bard who performs in the market square every evening and loves an audience.",
                       "Everybody in town knows her songs and her face. She is happiest at the centre of a crowd's "
                       "attention."),
                person("Eirik",
                       "an elderly priest of Arkay who walks slowly with a cane.",
                       "He tends the hall of the dead and is respected for it. He tires quickly and cannot keep pace "
                       "with anybody on a busy street."),
                person("Fjola",
                       "a farmer who comes to town twice a month to sell cabbages.",
                       "She knows little of the town beyond her stall and leaves before dark. She has never been asked "
                       "to do anything quietly."),
                person("Gunnar",
                       "a guard in the court's colours, recognised by everyone.",
                       "He patrols the market in full armour. Wherever he walks, people notice him and lower their "
                       "voices."),
                person("Hilde",
                       "the steward's own housecarl, a towering armoured warrior.",
                       "She is sworn to guard the steward's life and never leaves his side. She is the most "
                       "conspicuous person in any room."),
                person("Sorli",
                       "a former Legion scout who tracked deserters for years and can follow anyone through a crowd "
                       "unseen.",
                       "He is quiet and forgettable to look at. He has spent his life watching people without their "
                       "knowing and is patient enough to do it for weeks."),
                person(
                    "Torvar",
                    "a drunk who is thrown out of the inn most nights.",
                    "He makes a scene wherever he goes and remembers little of it. Nobody trusts him with anything."),
                person("Ulla",
                       "a scholar who never looks up from her books.",
                       "She notices nothing that is not written down. She would lose the merchant within a minute."),
                person("Vilmar",
                       "the merchant's own brother, whom she sees every day.",
                       "He is close to his sister and talks to her constantly. She would know him at a glance from any "
                       "distance."),
                person("Yrsa",
                       "a healer who is called away at all hours.",
                       "She is devoted to her patients and could not promise anybody a week of her time."),
            };
        }

        // The real verbs, each described by its own dashboard sentence with
        // nothing filled in -- exactly what plot planning offers.
        std::vector<Option> Verbs()
        {
            std::vector<Option> out;
            for (const auto& verb : PlotVerbs::kVerbs) {
                auto line = PlotVerbs::Render(
                    verb.Text(PlotVerbs::Template::Dashboard),
                    [](std::string_view) { return std::nullopt; },
                    [&verb](std::string_view name) {
                        const auto* slot = PlotVerbs::FindSlot(verb, name);
                        return std::string{slot != nullptr ? PlotVerbs::Unfilled(slot->kind) : "someone"};
                    });
                if (!line.empty()) {
                    line[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(line[0])));
                }
                out.push_back({std::string{verb.id}, std::move(line)});
            }
            return out;
        }

        // Eight things from the plot item pool, described. The deed is
        // seventh.
        std::vector<Option> Items()
        {
            return {
                {"a ruby", "a red gem, valuable and easy to carry"},
                {"a bottle of skooma", "an illegal drug"},
                {"a set of lockpicks", "tools for opening locks"},
                {"a bottle of Black-Briar mead", "a famous mead brewed in Riften"},
                {"a purse of septims", "a small bag of gold coins"},
                {"a sealed letter", "a letter closed with a wax seal, its contents unknown"},
                {"a deed to a property", "a legal document naming who owns a house or a piece of land"},
                {"a set of written orders", "instructions written by a commander to his men"},
            };
        }

        std::string Percent(double p)
        {
            return std::format("{:.0f}%", 100.0 * p);
        }
    } // namespace

    const std::vector<Round>& Rounds()
    {
        static const std::vector<Round> rounds{
            {"capital",
             "A traveller asks which city rules the Reach.",
             "Which of these places is the capital of the Reach?",
             Places(),
             "Markarth",
             false},
            {"capital_state",
             "A traveller asks which city rules the Reach. Every place is described in `choices`.",
             "Which of these places is the capital of the Reach?",
             Places(),
             "Markarth",
             true},
            {"watcher",
             "Hrolfdir, steward of a jarl's court, needs somebody to follow a merchant through the market for a week "
             "and learn where she goes, without her ever noticing she is being followed.",
             "Who should Hrolfdir send?",
             People(false),
             "Sorli",
             false},
            {"watcher_bios",
             "Hrolfdir, steward of a jarl's court, needs somebody to follow a merchant through the market for a week "
             "and learn where she goes, without her ever noticing she is being followed. Everybody he could send is "
             "described in `choices`.",
             "Who should Hrolfdir send?",
             People(true),
             "Sorli",
             true},
            {"next_step",
             "Bjarni means to win the favour of a jarl's steward by giving him a ruby. The steps he has taken so "
             "far: 1. Somebody he sent stole a ruby from a merchant, and it worked.",
             "What kind of step should Bjarni take next to bring the scheme about?",
             Verbs(),
             "deliver",
             false},
            {"proof",
             "Ingrid wants to prove that a rival still owns the farm he claims to have given up.",
             "Which of these things would prove it?",
             Items(),
             "a deed to a property",
             false},
        };
        return rounds;
    }

    std::string Context(const Round& round, bool reversed)
    {
        nlohmann::json options = nlohmann::json::array();
        const auto ordered = Ordered(round, reversed);
        for (std::size_t i = 0; i < ordered.size(); ++i) {
            options.push_back({{"key", KeyAt(i)}, {"name", ordered[i]->name}, {"background", ordered[i]->background}});
        }
        nlohmann::json ctx;
        ctx["situation"] = round.situation;
        ctx["question"] = round.question;
        ctx["background_in_state"] = round.backgroundInState;
        ctx["options"] = std::move(options);
        return ctx.dump();
    }

    std::string CorrectKey(const Round& round, bool reversed)
    {
        const auto ordered = Ordered(round, reversed);
        for (std::size_t i = 0; i < ordered.size(); ++i) {
            if (ordered[i]->name == round.correct) {
                return KeyAt(i);
            }
        }
        return {};
    }

    Finding Judge(const Round& round,
                  bool reversed,
                  const SkyrimNetAPI::DecisionResult& result,
                  std::chrono::milliseconds latency)
    {
        Finding f;
        f.round = round.id;
        f.reversed = reversed;
        f.latencyMs = latency.count();
        const auto correctKey = CorrectKey(round, reversed);
        f.rightAt = correctKey.empty() ? 0 : static_cast<std::size_t>(std::stoul(correctKey.substr(1)));

        if (!result.ok) {
            f.error = result.error.empty() ? std::string{"no answer"} : result.error;
            return f;
        }
        f.answered = true;

        const auto key = result.Choice(kQuestion);
        const auto ordered = Ordered(round, reversed);
        f.chosen = key;
        for (std::size_t i = 0; i < ordered.size(); ++i) {
            if (KeyAt(i) == key) {
                f.chosen = ordered[i]->name;
            }
        }
        f.right = !correctKey.empty() && key == correctKey;

        for (const auto& [question, answer] : result.answers) {
            if (question != kQuestion) {
                continue;
            }
            for (const auto& [option, p] : answer.probabilities) {
                if (option == correctKey) {
                    f.rightChance = p;
                }
            }
        }
        return f;
    }

    std::vector<std::string> Report(const std::vector<Finding>& findings)
    {
        std::vector<std::string> lines;
        std::size_t right = 0;
        std::size_t onLine = 0, onLineRight = 0, inState = 0, inStateRight = 0;
        std::size_t early = 0, earlyRight = 0, late = 0, lateRight = 0;
        long long totalMs = 0;

        for (const auto& f : findings) {
            const auto* round = [&]() -> const Round* {
                for (const auto& r : Rounds()) {
                    if (r.id == f.round) {
                        return &r;
                    }
                }
                return nullptr;
            }();
            const auto count = round != nullptr ? round->options.size() : 0;
            const auto order = f.reversed ? "reversed" : "as written";

            if (!f.answered) {
                lines.push_back(std::format(
                    "DecisionProbe: {} ({}): NO ANSWER after {} ms -- {}", f.round, order, f.latencyMs, f.error));
                continue;
            }
            lines.push_back(
                std::format("DecisionProbe: {} ({}): {} -- chose {}; the right answer was #{} of {}{}, {} ms",
                            f.round,
                            order,
                            f.right ? "right" : "WRONG",
                            f.chosen,
                            f.rightAt,
                            count,
                            f.rightChance > 0.0 ? std::format(", given {}", Percent(f.rightChance)) : std::string{},
                            f.latencyMs));

            right += f.right ? 1 : 0;
            totalMs += f.latencyMs;
            const bool state = round != nullptr && round->backgroundInState;
            (state ? inState : onLine) += 1;
            (state ? inStateRight : onLineRight) += f.right ? 1 : 0;
            const bool firstHalf = count > 0 && f.rightAt * 2 <= count;
            (firstHalf ? early : late) += 1;
            (firstHalf ? earlyRight : lateRight) += f.right ? 1 : 0;
        }

        const auto answered = onLine + inState;
        lines.push_back(std::format("DecisionProbe: {} of {} right ({} asked); background on each option's line {} of "
                                    "{}, in a state block {} of {}; right answer in the first half of the list {} of "
                                    "{}, in the second half {} of {}; {} ms a request on average",
                                    right,
                                    answered,
                                    findings.size(),
                                    onLineRight,
                                    onLine,
                                    inStateRight,
                                    inState,
                                    earlyRight,
                                    early,
                                    lateRight,
                                    late,
                                    answered > 0 ? totalMs / static_cast<long long>(answered) : 0));
        return lines;
    }

    std::vector<Finding> Run(const PlotThread::Token& pt)
    {
        std::vector<Finding> findings;
        for (const auto& round : Rounds()) {
            for (const bool reversed : {false, true}) {
                const auto started = std::chrono::steady_clock::now();
                const auto result = SkyrimNetAPI::SendCustomDecision(pt, kTemplate, Context(round, reversed));
                const auto latency =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
                findings.push_back(Judge(round, reversed, result, latency));
            }
        }
        for (const auto& line : Report(findings)) {
            logger::info("{}", line);
        }
        return findings;
    }
} // namespace NarrativeEngine::DecisionProbe
