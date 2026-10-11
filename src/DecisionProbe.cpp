#include <DecisionProbe.h>

#include <logger.h>

#include <algorithm>
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

        const SkyrimNetAPI::DecisionAnswer* Find(const SkyrimNetAPI::DecisionResult& result, std::string_view id)
        {
            for (const auto& [question, answer] : result.answers) {
                if (question == id) {
                    return &answer;
                }
            }
            return nullptr;
        }

        std::string YesNo(bool value)
        {
            return value ? "yes" : "NO";
        }
    } // namespace

    const std::vector<std::string>& Places()
    {
        // Markarth is thirteenth, which is what kCapitalKey names. Every
        // other entry is a real place that is not the Reach's capital, so a
        // right answer means the option list was read rather than guessed.
        static const std::vector<std::string> places{
            "Whiterun",   "Solitude",      "Windhelm",   "Riften",        "Falkreath",
            "Morthal",    "Dawnstar",      "Winterhold", "Riverwood",     "Rorikstead",
            "Ivarstead",  "Dragon Bridge", "Markarth",   "Shor's Stone",  "Karthwasten",
            "Kynesgrove", "Helgen",        "Raven Rock", "Skaal Village", "Darkwater Crossing"};
        return places;
    }

    std::string Context()
    {
        nlohmann::json rows = nlohmann::json::array();
        const auto& places = Places();
        for (std::size_t i = 0; i < places.size(); ++i) {
            rows.push_back({{"key", KeyAt(i)}, {"label", places[i]}});
        }
        nlohmann::json ctx;
        ctx["places"] = std::move(rows);
        return ctx.dump();
    }

    Verdict Judge(const SkyrimNetAPI::DecisionResult& result, std::chrono::milliseconds latency)
    {
        Verdict v;
        v.latencyMs = latency.count();
        v.answered = result.ok;
        if (!result.ok) {
            v.lines.push_back(std::format(
                "DecisionProbe: the request failed after {} ms: {} ({})", v.latencyMs, result.error, result.message));
            return v;
        }

        const auto* capital = Find(result, "capital");
        const auto* fish = Find(result, "fish");
        const auto* cold = Find(result, "cold");

        const auto& places = Places();
        const auto offeredPlace = [&places](const std::string& key) {
            for (std::size_t i = 0; i < places.size(); ++i) {
                if (KeyAt(i) == key) {
                    return true;
                }
            }
            return false;
        };
        const auto offeredFish = [](const std::string& key) {
            return key == "c1" || key == "c2" || key == "c3" || key == "c4" || key == "c5";
        };

        v.loopRendered = capital != nullptr && capital->type == "choice" && offeredPlace(capital->choice);
        v.keysVerbatim = v.loopRendered && fish != nullptr && offeredFish(fish->choice);
        v.twentyCorrect = capital != nullptr && capital->choice == kCapitalKey;
        v.fiveCorrect = fish != nullptr && fish->choice == kFishKey;
        v.allQuestionsAnswered = capital != nullptr && fish != nullptr && cold != nullptr;

        const auto choiceOf = [](const SkyrimNetAPI::DecisionAnswer* a) {
            return a != nullptr ? (a->choice.empty() ? std::string{"(no choice)"} : a->choice)
                                : std::string{"(unanswered)"};
        };
        const auto chosenPlace = [&]() -> std::string {
            if (capital == nullptr) {
                return "nothing";
            }
            for (std::size_t i = 0; i < places.size(); ++i) {
                if (KeyAt(i) == capital->choice) {
                    return places[i];
                }
            }
            return "a key that was never offered";
        }();

        v.lines.push_back(std::format("DecisionProbe: answered in {} ms", v.latencyMs));
        v.lines.push_back(std::format("DecisionProbe: 1. loop-generated options rendered and were chosen from: {} "
                                      "(capital = {})",
                                      YesNo(v.loopRendered),
                                      choiceOf(capital)));
        v.lines.push_back(std::format("DecisionProbe: 2. keys came back verbatim: {} (capital = {}, fish = {})",
                                      YesNo(v.keysVerbatim),
                                      choiceOf(capital),
                                      choiceOf(fish)));
        v.lines.push_back(std::format("DecisionProbe: 3. twenty options: {} (chose {}, Markarth is right); "
                                      "five options: {}",
                                      v.twentyCorrect ? "right" : "WRONG",
                                      chosenPlace,
                                      v.fiveCorrect ? "right" : "WRONG"));
        v.lines.push_back(std::format("DecisionProbe: 4. every question answered: {} ({} of 3 came back)",
                                      YesNo(v.allQuestionsAnswered),
                                      (capital != nullptr) + (fish != nullptr) + (cold != nullptr)));
        if (cold != nullptr) {
            v.lines.push_back(std::format("DecisionProbe:    the yes/no came back as p(yes) = {:.2f}", cold->noul));
        }
        if (capital != nullptr && !capital->probabilities.empty()) {
            auto ranked = capital->probabilities;
            std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            std::string top;
            for (std::size_t i = 0; i < ranked.size() && i < 3; ++i) {
                top += std::format("{}{}={:.2f}", i == 0 ? "" : ", ", ranked[i].first, ranked[i].second);
            }
            v.lines.push_back(std::format("DecisionProbe:    capital's likeliest: {}", top));
        }
        return v;
    }

    Verdict Run(const PlotThread::Token& pt)
    {
        logger::info("DecisionProbe: asking '{}'", kTemplate);
        const auto started = std::chrono::steady_clock::now();
        const auto result = SkyrimNetAPI::SendCustomDecision(pt, kTemplate, Context());
        const auto latency =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        auto verdict = Judge(result, latency);
        for (const auto& line : verdict.lines) {
            logger::info("{}", line);
        }
        return verdict;
    }
} // namespace NarrativeEngine::DecisionProbe
