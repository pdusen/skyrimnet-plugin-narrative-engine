#include <PlotMemories.h>

#include <GossipGraph.h>
#include <logger.h>
#include <SkyrimNetAPI.h>

#include <algorithm>
#include <format>

namespace NarrativeEngine::PlotMemories
{
    namespace
    {
        using PlotVerbs::Template;

        // How much each memory matters, and what kind it is. A failure is
        // the most a step writes, above a timeout: being caught is what
        // gets talked about. Hearing about something is knowledge; doing it,
        // sending somebody to do it, or giving it up is experience.
        struct Weight
        {
            float importance;
            const char* type;
        };

        Weight WeightOf(Template t)
        {
            switch (t) {
            case Template::AgentDispatched:
            case Template::MastermindDispatched:
            case Template::SelfDispatched:
                return {0.45f, "EXPERIENCE"};
            case Template::AgentSucceeded:
                return {0.55f, "EXPERIENCE"};
            case Template::AgentFailed:
                return {0.75f, "EXPERIENCE"};
            case Template::AgentTimedOut:
                return {0.55f, "EXPERIENCE"};
            case Template::MastermindToldFailed:
                return {0.7f, "KNOWLEDGE"};
            case Template::MastermindToldTimedOut:
                return {0.5f, "KNOWLEDGE"};
            case Template::Dashboard:
            case Template::Count:
                break;
            }
            return {0.5f, "EXPERIENCE"};
        }

        constexpr float kConcededImportance = 0.6f;

        // Everybody in the step: the mastermind, the agent and every person
        // slot.
        std::vector<RE::FormID> PeopleIn(const PlotModel::Plot& plot, const PlotModel::Step& step)
        {
            std::vector<RE::FormID> out;
            const auto add = [&out](RE::FormID npc) {
                if (npc != 0 && std::find(out.begin(), out.end(), npc) == out.end()) {
                    out.push_back(npc);
                }
            };
            add(plot.mastermind);
            add(step.actor);
            if (const auto* verb = PlotVerbs::Find(step.verb)) {
                for (std::size_t i = 0; i < verb->slotCount; ++i) {
                    if (verb->slots[i].kind != PlotVerbs::SlotKind::Person) {
                        continue;
                    }
                    if (const auto* value = step.FindSlot(verb->slots[i].name)) {
                        add(value->form);
                    }
                }
            }
            return out;
        }

        std::string PlaceOf(const PlotModel::Step& step)
        {
            if (const auto* verb = PlotVerbs::Find(step.verb)) {
                for (std::size_t i = 0; i < verb->slotCount; ++i) {
                    if (verb->slots[i].kind == PlotVerbs::SlotKind::Place) {
                        const auto* value = step.FindSlot(verb->slots[i].name);
                        return value != nullptr ? value->text : std::string{};
                    }
                }
            }
            return {};
        }

        Memory Make(const PlotModel::Plot& plot, const PlotModel::Step& step, RE::FormID keeper, Template t)
        {
            const auto weight = WeightOf(t);
            Memory m;
            m.npc = keeper;
            m.text = PlotVerbs::RenderStep(step, t, plot.scheme);
            m.importance = weight.importance;
            m.type = weight.type;
            for (const auto npc : PeopleIn(plot, step)) {
                if (npc != keeper) {
                    m.related.push_back(npc);
                }
            }
            m.location = PlaceOf(step);
            m.source = PlotVerbs::TemplateId(t);
            return m;
        }

        bool ByOwnHand(const PlotModel::Plot& plot, const PlotModel::Step& step)
        {
            return step.actor == plot.mastermind;
        }

        std::string JsonArray(const std::vector<std::string>& items)
        {
            std::string out = "[";
            for (std::size_t i = 0; i < items.size(); ++i) {
                out += (i == 0 ? "" : ",") + items[i];
            }
            return out + "]";
        }
    } // namespace

    std::vector<Memory> OnDispatch(const PlotModel::Plot& plot, const PlotModel::Step& step)
    {
        if (step.verb.empty() || step.actor == 0) {
            return {};
        }
        if (ByOwnHand(plot, step)) {
            return {Make(plot, step, plot.mastermind, Template::SelfDispatched)};
        }
        return {Make(plot, step, step.actor, Template::AgentDispatched),
                Make(plot, step, plot.mastermind, Template::MastermindDispatched)};
    }

    std::vector<Memory> OnResolve(const PlotModel::Plot& plot, const PlotModel::Step& step)
    {
        if (step.verb.empty() || step.actor == 0) {
            return {};
        }
        Template agent{};
        Template told{};
        switch (step.outcome) {
        case PlotModel::StepOutcome::Succeeded:
            // A success is the agent's alone: the template set has no
            // memory of hearing about one (open question 5).
            return {Make(plot, step, step.actor, Template::AgentSucceeded)};
        case PlotModel::StepOutcome::FailedCaught:
            agent = Template::AgentFailed;
            told = Template::MastermindToldFailed;
            break;
        case PlotModel::StepOutcome::FailedTimeout:
            agent = Template::AgentTimedOut;
            told = Template::MastermindToldTimedOut;
            break;
        case PlotModel::StepOutcome::None:
            return {};
        }
        std::vector<Memory> out{Make(plot, step, step.actor, agent)};
        if (!ByOwnHand(plot, step)) {
            out.push_back(Make(plot, step, plot.mastermind, told));
        }
        return out;
    }

    std::vector<Memory> OnConceded(const PlotModel::Plot& plot)
    {
        if (plot.mastermind == 0) {
            return {};
        }
        Memory m;
        m.npc = plot.mastermind;
        m.text = PlotVerbs::RenderConceded(plot.scheme);
        m.importance = kConcededImportance;
        m.type = "EXPERIENCE";
        m.source = "mastermind_conceded";
        return {m};
    }

    std::size_t Write(const std::vector<Memory>& memories)
    {
        const auto tags = JsonArray({std::format("\"{}\"", kTag)});
        std::size_t written = 0;
        for (const auto& m : memories) {
            const auto keeper = GossipGraph::ActorRefFor(m.npc);
            if (keeper == 0 || m.text.empty()) {
                logger::debug("PlotMemories: {} for 0x{:X} not written: no reference or no text", m.source, m.npc);
                continue;
            }
            std::vector<std::string> related;
            for (const auto npc : m.related) {
                if (const auto ref = GossipGraph::ActorRefFor(npc); ref != 0) {
                    related.push_back(std::to_string(ref));
                }
            }
            const int id = SkyrimNetAPI::AddMemory(
                keeper, m.text, m.importance, m.type, "", m.location, tags, related.empty() ? "" : JsonArray(related));
            if (id > 0) {
                ++written;
            } else {
                logger::debug("PlotMemories: SkyrimNet refused {} for 0x{:X}", m.source, m.npc);
            }
        }
        return written;
    }
} // namespace NarrativeEngine::PlotMemories
