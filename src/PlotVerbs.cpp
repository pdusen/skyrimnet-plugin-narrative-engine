#include <PlotVerbs.h>

#include <cctype>

namespace NarrativeEngine::PlotVerbs
{
    std::vector<const Preparation*> PreparationsOf(std::string_view verb)
    {
        std::vector<const Preparation*> out;
        for (const auto& p : kPreparations) {
            if (p.prepares == verb) {
                out.push_back(&p);
            }
        }
        return out;
    }

    const Verb* Find(std::string_view id) noexcept
    {
        for (const auto& verb : kVerbs) {
            if (verb.id == id) {
                return &verb;
            }
        }
        return nullptr;
    }

    std::optional<PlotRecipes::Craft> CraftOf(Rule rule) noexcept
    {
        switch (rule) {
        case Rule::SmithRecipe:
            return PlotRecipes::Craft::Smith;
        case Rule::ConcoctRecipe:
            return PlotRecipes::Craft::Concoct;
        case Rule::EnchantRecipe:
            return PlotRecipes::Craft::Enchant;
        case Rule::SetRecipe:
            return PlotRecipes::Craft::Set;
        case Rule::FletchRecipe:
            return PlotRecipes::Craft::Fletch;
        default:
            return std::nullopt;
        }
    }

    std::optional<PlotRecipes::Craft> CraftOf(const Verb& verb) noexcept
    {
        for (std::size_t i = 0; i < verb.slotCount; ++i) {
            if (const auto craft = CraftOf(verb.slots[i].rule)) {
                return craft;
            }
        }
        return std::nullopt;
    }

    std::string_view TemplateId(Template t) noexcept
    {
        switch (t) {
        case Template::Dashboard:
            return "dashboard";
        case Template::AgentDispatched:
            return "agent_dispatched";
        case Template::MastermindDispatched:
            return "mastermind_dispatched";
        case Template::SelfDispatched:
            return "self_dispatched";
        case Template::AgentSucceeded:
            return "agent_succeeded";
        case Template::AgentFailed:
            return "agent_failed";
        case Template::AgentTimedOut:
            return "agent_timed_out";
        case Template::MastermindToldFailed:
            return "mastermind_told_failed";
        case Template::MastermindToldTimedOut:
            return "mastermind_told_timed_out";
        case Template::MastermindToldSucceeded:
            return "mastermind_told_succeeded";
        case Template::MastermindTask:
            return "mastermind_task";
        case Template::AgentTask:
            return "agent_task";
        case Template::Count:
            break;
        }
        return "unknown";
    }

    std::vector<std::string_view> PlaceholdersIn(std::string_view text)
    {
        std::vector<std::string_view> out;
        std::size_t at = 0;
        while ((at = text.find('{', at)) != std::string_view::npos) {
            const auto close = text.find('}', at + 1);
            if (close == std::string_view::npos) {
                break;
            }
            out.push_back(text.substr(at + 1, close - at - 1));
            at = close + 1;
        }
        return out;
    }

    std::string SchemePhrase(std::string_view scheme)
    {
        std::string out{scheme};
        while (!out.empty()
               && (std::isspace(static_cast<unsigned char>(out.back())) || out.back() == '.' || out.back() == '!'
                   || out.back() == '?' || out.back() == ';' || out.back() == ',')) {
            out.pop_back();
        }
        std::size_t first = 0;
        while (first < out.size() && std::isspace(static_cast<unsigned char>(out[first]))) {
            ++first;
        }
        out.erase(0, first);
        if (!out.empty()) {
            out[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[0])));
        }
        return out;
    }

    std::string Render(std::string_view text,
                       const Lookup& lookup,
                       const std::function<std::string(std::string_view name)>& missing)
    {
        std::string out;
        out.reserve(text.size() + 64);
        std::size_t at = 0;
        while (at < text.size()) {
            const auto open = text.find('{', at);
            if (open == std::string_view::npos) {
                out.append(text.substr(at));
                break;
            }
            const auto close = text.find('}', open + 1);
            if (close == std::string_view::npos) {
                out.append(text.substr(at));
                break;
            }
            out.append(text.substr(at, open - at));
            const auto name = text.substr(open + 1, close - open - 1);
            auto value = lookup ? lookup(name) : std::nullopt;
            out.append(value ? *value : (missing ? missing(name) : std::string{}));
            at = close + 1;
        }
        return out;
    }

    std::string Words(const Verb& verb)
    {
        std::string out{verb.caption};
        for (auto& c : out) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    std::string Plain(std::string_view text)
    {
        // An id's characters: letters, digits and the underscore. Anything
        // else either side of it means it stands as a word of its own.
        const auto inWord = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
        std::string out;
        std::size_t at = 0;
        while (at < text.size()) {
            if (!inWord(text[at])) {
                out.push_back(text[at++]);
                continue;
            }
            auto end = at;
            while (end < text.size() && inWord(text[end])) {
                ++end;
            }
            const auto word = text.substr(at, end - at);
            const auto* verb = Find(word);
            out += verb != nullptr ? Words(*verb) : std::string{word};
            at = end;
        }
        return out;
    }

    const Slot* FindSlot(const Verb& verb, std::string_view name) noexcept
    {
        for (std::size_t i = 0; i < verb.slotCount; ++i) {
            if (verb.slots[i].name == name) {
                return &verb.slots[i];
            }
        }
        return nullptr;
    }

    std::string Caption(const PlotModel::Step& step)
    {
        if (const auto* verb = Find(step.verb)) {
            return std::string{verb->caption};
        }
        return step.label.empty() ? std::string{PlotModel::Verb(step.type)} : step.label;
    }

} // namespace NarrativeEngine::PlotVerbs
