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

    std::string_view Unfilled(SlotKind kind) noexcept
    {
        switch (kind) {
        case SlotKind::Person:
            return "someone";
        case SlotKind::Place:
            return "somewhere";
        case SlotKind::Thing:
            return "something";
        }
        return "someone";
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

    std::string RenderStep(const PlotModel::Step& step, Template t, std::string_view scheme)
    {
        const Verb* verb = Find(step.verb);
        if (verb == nullptr) {
            return {};
        }
        const auto lookup = [&](std::string_view name) -> std::optional<std::string> {
            if (name == "agent") {
                return step.actorName.empty() ? std::nullopt : std::optional<std::string>{step.actorName};
            }
            if (name == "scheme") {
                return scheme.empty() ? std::nullopt : std::optional<std::string>{SchemePhrase(scheme)};
            }
            const auto* value = step.FindSlot(name);
            return value != nullptr && !value->text.empty() ? std::optional<std::string>{value->text} : std::nullopt;
        };
        const auto missing = [verb](std::string_view name) {
            if (const auto* slot = FindSlot(*verb, name)) {
                return std::string{Unfilled(slot->kind)};
            }
            // `agent` and `mastermind` are people; `scheme` only goes
            // missing on a plot with no scheme, which reads least badly as
            // this.
            return std::string{name == "scheme" ? "it" : "someone"};
        };
        auto out = Render(verb->Text(t), lookup, missing);
        // A sentence can open with a slot -- "{agent} watches..." -- and an
        // unfilled one reads as "someone", so the first letter is not the
        // template's to guarantee.
        if (!out.empty()) {
            out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
        }
        return out;
    }

    std::string Caption(const PlotModel::Step& step)
    {
        if (const auto* verb = Find(step.verb)) {
            return std::string{verb->caption};
        }
        return step.label.empty() ? std::string{PlotModel::Verb(step.type)} : step.label;
    }

    std::string RenderConceded(std::string_view scheme)
    {
        const auto phrase = SchemePhrase(scheme);
        return Render(
            kConcededTemplate,
            [&](std::string_view name) -> std::optional<std::string> {
                return name == "scheme" && !phrase.empty() ? std::optional<std::string>{phrase} : std::nullopt;
            },
            [](std::string_view) { return std::string{"it"}; });
    }
} // namespace NarrativeEngine::PlotVerbs
