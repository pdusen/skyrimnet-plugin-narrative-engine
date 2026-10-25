#include <PlotMemories.h>

#include <GossipGraph.h>
#include <logger.h>
#include <PlotCandidates.h>
#include <PlotPopulation.h>
#include <PlotText.h>
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
            case Template::MastermindToldSucceeded:
                return {0.5f, "KNOWLEDGE"};
            case Template::Dashboard:
            case Template::MastermindTask:
            case Template::AgentTask:
            case Template::Count:
                break;
            }
            return {0.5f, "EXPERIENCE"};
        }

        constexpr float kConcededImportance = 0.6f;
        // The plan done is the most a mastermind has to show; lost at the
        // finish is above giving up, since it was lost rather than let go.
        constexpr float kSucceededImportance = 0.7f;
        constexpr float kFailedImportance = 0.65f;

        // A person's sex, for templates that agree with it: what the plot
        // population read off their base record at session start.
        TextTemplate::Sex SexOf(RE::FormID npc)
        {
            const auto* member = PlotPopulation::Get().Find(npc);
            if (member == nullptr) {
                return TextTemplate::Sex::Other;
            }
            return member->female ? TextTemplate::Sex::Female : TextTemplate::Sex::Male;
        }

        // The mastermind's memory of the plot as a whole: theirs alone,
        // relating nobody, since the step memories before it say who.
        Memory PlotLine(const LanguageTable::Table& table,
                        const PlotModel::Plot& plot,
                        PlotText::Outcome outcome,
                        float importance,
                        std::string_view source)
        {
            Memory m;
            m.npc = plot.mastermind;
            m.text = PlotText::PlotOutcome(table, outcome, plot.scheme);
            m.importance = importance;
            m.type = "EXPERIENCE";
            m.source = source;
            return m;
        }

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

        Memory Make(const LanguageTable::Table& table,
                    const PlotModel::Plot& plot,
                    const PlotModel::Step& step,
                    RE::FormID keeper,
                    Template t)
        {
            const auto weight = WeightOf(t);
            Memory m;
            m.npc = keeper;
            m.text = PlotText::Memory(table, step, t, plot.scheme, SexOf);
            m.importance = weight.importance;
            m.type = weight.type;
            for (const auto npc : PeopleIn(plot, step)) {
                if (npc != keeper) {
                    m.related.push_back(npc);
                }
            }
            m.location = PlaceOf(step);
            m.source = std::string{PlotVerbs::TemplateId(t)};
            return m;
        }

        bool ByOwnHand(const PlotModel::Plot& plot, const PlotModel::Step& step)
        {
            return step.actor == plot.mastermind;
        }

        // Being caught at it, or turned away, is what gets talked about:
        // level with the agent's own failure. Having it done to one openly is
        // level with the agent's success.
        constexpr float kTargetFailedImportance = 0.75f;
        constexpr float kTargetSucceededImportance = 0.55f;
        // Being wronged by nobody one can name lingers.
        constexpr float kTargetNoticedImportance = 0.6f;

        // Everybody a person slot's template names, besides its keeper: the
        // agent, the mastermind where the verb's sentence puts them, and
        // anybody else in the step it mentions. Somebody remembers who they
        // dealt with, and nobody they did not.
        std::vector<RE::FormID> NamedIn(const LanguageTable::Table& table,
                                        const PlotModel::Plot& plot,
                                        const PlotModel::Step& step,
                                        std::string_view slotName,
                                        PlotVerbs::TargetTemplate t,
                                        RE::FormID keeper)
        {
            std::vector<RE::FormID> out;
            for (const auto& name : PlotText::RememberedNames(table, step, slotName, t)) {
                RE::FormID npc = 0;
                if (name == "agent") {
                    npc = step.actor;
                } else if (name == "mastermind") {
                    npc = plot.mastermind;
                } else if (const auto* value = step.FindSlot(name)) {
                    const auto* verb = PlotVerbs::Find(step.verb);
                    const auto* slot = verb != nullptr ? PlotVerbs::FindSlot(*verb, name) : nullptr;
                    npc = slot != nullptr && slot->kind == PlotVerbs::SlotKind::Person ? value->form : 0;
                }
                if (npc != 0 && npc != keeper && std::find(out.begin(), out.end(), npc) == out.end()) {
                    out.push_back(npc);
                }
            }
            return out;
        }

        // The people a step reached, each remembering it from their own
        // slot's template: everybody whose awareness fired for the outcome,
        // and on a success anybody it was done to quietly who would notice
        // what it left behind -- without knowing who. A timeout reaches
        // nobody.
        void Targets(const LanguageTable::Table& table,
                     const PlotModel::Plot& plot,
                     const PlotModel::Step& step,
                     std::vector<Memory>& out)
        {
            const auto* verb = PlotVerbs::Find(step.verb);
            const bool succeeded = step.outcome == PlotModel::StepOutcome::Succeeded;
            if (verb == nullptr || (!succeeded && step.outcome != PlotModel::StepOutcome::FailedCaught)) {
                return;
            }
            const auto t = succeeded ? PlotVerbs::TargetTemplate::Succeeded : PlotVerbs::TargetTemplate::Failed;
            for (std::size_t i = 0; i < verb->slotCount; ++i) {
                const auto& slot = verb->slots[i];
                if (slot.kind != PlotVerbs::SlotKind::Person) {
                    continue;
                }
                if (succeeded && !PlotCandidates::AwarenessFired(slot.awareness, step.outcome)
                    && slot.Remembers(PlotVerbs::TargetTemplate::Noticed)) {
                    const auto* value = step.FindSlot(slot.name);
                    if (value != nullptr && value->form != 0 && value->form != step.actor
                        && value->form != plot.mastermind) {
                        // Nothing related: the memory's related actors would
                        // tell them exactly who they do not know did it.
                        Memory m;
                        m.npc = value->form;
                        m.text =
                            PlotText::Remembered(table, step, slot.name, PlotVerbs::TargetTemplate::Noticed, SexOf);
                        m.importance = kTargetNoticedImportance;
                        m.type = "EXPERIENCE";
                        m.location = PlaceOf(step);
                        m.source = "target_noticed";
                        out.push_back(std::move(m));
                    }
                    continue;
                }
                if (!slot.Remembers(t) || !PlotCandidates::AwarenessFired(slot.awareness, step.outcome)) {
                    continue;
                }
                const auto* value = step.FindSlot(slot.name);
                if (value == nullptr || value->form == 0 || value->form == step.actor
                    || value->form == plot.mastermind) {
                    continue;
                }
                Memory m;
                m.npc = value->form;
                m.text = PlotText::Remembered(table, step, slot.name, t, SexOf);
                m.importance = succeeded ? kTargetSucceededImportance : kTargetFailedImportance;
                m.type = "EXPERIENCE";
                m.related = NamedIn(table, plot, step, slot.name, t, m.npc);
                m.location = PlaceOf(step);
                m.source = succeeded ? "target_succeeded" : "target_failed";
                out.push_back(std::move(m));
            }
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

    std::vector<Memory> OnDispatch(const LanguageTable::Table& table,
                                   const PlotModel::Plot& plot,
                                   const PlotModel::Step& step)
    {
        if (step.verb.empty() || step.actor == 0) {
            return {};
        }
        if (ByOwnHand(plot, step)) {
            return {Make(table, plot, step, plot.mastermind, Template::SelfDispatched)};
        }
        return {Make(table, plot, step, step.actor, Template::AgentDispatched),
                Make(table, plot, step, plot.mastermind, Template::MastermindDispatched)};
    }

    std::vector<Memory> OnResolve(const LanguageTable::Table& table,
                                  const PlotModel::Plot& plot,
                                  const PlotModel::Step& step)
    {
        if (step.verb.empty() || step.actor == 0) {
            return {};
        }
        switch (step.outcome) {
        case PlotModel::StepOutcome::Succeeded: {
            std::vector<Memory> out{Make(table, plot, step, step.actor, Template::AgentSucceeded)};
            if (!ByOwnHand(plot, step)) {
                out.push_back(Make(table, plot, step, plot.mastermind, Template::MastermindToldSucceeded));
            }
            Targets(table, plot, step, out);
            return out;
        }
        case PlotModel::StepOutcome::FailedCaught:
        case PlotModel::StepOutcome::FailedTimeout: {
            // The agent's own, at once. A mastermind who did it themselves
            // is the agent, and keeps theirs until the plot answers it.
            std::vector<Memory> out;
            if (!ByOwnHand(plot, step)) {
                out.push_back(Make(table,
                                   plot,
                                   step,
                                   step.actor,
                                   step.outcome == PlotModel::StepOutcome::FailedCaught ? Template::AgentFailed
                                                                                        : Template::AgentTimedOut));
            }
            Targets(table, plot, step, out);
            return out;
        }
        case PlotModel::StepOutcome::None:
            break;
        }
        return {};
    }

    std::vector<Memory> OnFailureAnswered(const LanguageTable::Table& table,
                                          const PlotModel::Plot& plot,
                                          const PlotModel::Step& step,
                                          bool wentOn)
    {
        if (step.verb.empty() || step.actor == 0 || plot.mastermind == 0) {
            return {};
        }
        const bool caught = step.outcome == PlotModel::StepOutcome::FailedCaught;
        if (!caught && step.outcome != PlotModel::StepOutcome::FailedTimeout) {
            return {};
        }
        const auto t = ByOwnHand(plot, step)
                           ? (caught ? Template::AgentFailed : Template::AgentTimedOut)
                           : (caught ? Template::MastermindToldFailed : Template::MastermindToldTimedOut);
        auto m = Make(table, plot, step, plot.mastermind, t);
        if (wentOn) {
            // The failure's own full stop gives way to the clause's.
            while (!m.text.empty() && (m.text.back() == '.' || m.text.back() == ' ')) {
                m.text.pop_back();
            }
            m.text += PlotText::PlotOutcome(table, PlotText::Outcome::WentOn, plot.scheme);
        }
        return {m};
    }

    std::vector<Memory> OnSucceeded(const LanguageTable::Table& table, const PlotModel::Plot& plot)
    {
        if (plot.mastermind == 0) {
            return {};
        }
        return {PlotLine(table, plot, PlotText::Outcome::Succeeded, kSucceededImportance, "mastermind_succeeded")};
    }

    std::vector<Memory> OnFinishFailed(const LanguageTable::Table& table, const PlotModel::Plot& plot)
    {
        if (plot.mastermind == 0) {
            return {};
        }
        return {PlotLine(table, plot, PlotText::Outcome::Failed, kFailedImportance, "mastermind_failed")};
    }

    std::vector<Memory> OnConceded(const LanguageTable::Table& table, const PlotModel::Plot& plot)
    {
        if (plot.mastermind == 0) {
            return {};
        }
        Memory m;
        m.npc = plot.mastermind;
        m.text = PlotText::PlotOutcome(table, PlotText::Outcome::Conceded, plot.scheme);
        m.importance = kConcededImportance;
        m.type = "EXPERIENCE";
        m.source = "mastermind_conceded";
        return {m};
    }

    std::vector<Memory> Combine(const std::vector<Memory>& memories)
    {
        std::vector<Memory> out;
        // The importance each combined memory's type was taken from.
        std::vector<float> typedBy;
        for (const auto& m : memories) {
            const auto it =
                std::find_if(out.begin(), out.end(), [&m](const Memory& kept) { return kept.npc == m.npc; });
            if (it == out.end()) {
                out.push_back(m);
                typedBy.push_back(m.importance);
                continue;
            }
            auto& kept = *it;
            kept.text += (kept.text.empty() || m.text.empty() ? "" : " ") + m.text;
            if (m.importance > typedBy[static_cast<std::size_t>(it - out.begin())]) {
                typedBy[static_cast<std::size_t>(it - out.begin())] = m.importance;
                kept.type = m.type;
            }
            kept.importance = std::max(kept.importance, m.importance);
            for (const auto npc : m.related) {
                if (npc != kept.npc && std::find(kept.related.begin(), kept.related.end(), npc) == kept.related.end()) {
                    kept.related.push_back(npc);
                }
            }
            if (kept.location.empty()) {
                kept.location = m.location;
            }
            kept.source += "+" + m.source;
        }
        return out;
    }

    std::size_t Write(const std::vector<Memory>& memories)
    {
        const auto tags = JsonArray({std::format("\"{}\"", kTag)});
        std::size_t written = 0;
        for (const auto& m : memories) {
            // Only the living remember. A keeper may have died since the step
            // began, and a memory written to the dead would surface the day
            // somebody resurrects them, about a plot long over.
            if (!PlotPopulation::IsAlive(m.npc)) {
                logger::debug("PlotMemories: {} for 0x{:X} not written: they are not alive", m.source, m.npc);
                continue;
            }
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
