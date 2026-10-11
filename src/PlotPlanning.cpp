#include <PlotPlanning.h>

#include <cctype>
#include <format>

namespace NarrativeEngine::PlotPlanning
{
    namespace
    {
        bool DependsOnAnotherSlot(const PlotVerbs::Slot& slot)
        {
            return slot.rule == PlotVerbs::Rule::MarkCommunity
                   || slot.rule == PlotVerbs::Rule::MarkOrMastermindSettlement;
        }

        PlotCandidates::Request RequestFor(const Input& input,
                                           const std::vector<PlotModel::SlotValue>& filled,
                                           const std::vector<const PlotModel::Step*>& earlier)
        {
            return PlotCandidates::Request{input.population,
                                           input.mastermind,
                                           input.world,
                                           filled,
                                           earlier,
                                           input.alive,
                                           input.occupation,
                                           input.scheme};
        }

        std::string Capitalised(std::string text)
        {
            if (!text.empty()) {
                text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
            }
            return text;
        }

        // A verb as an option on the list: its dashboard sentence with
        // every slot unfilled, which says what the verb does without
        // saying to whom.
        std::string VerbLine(const PlotVerbs::Verb& verb)
        {
            return Capitalised(PlotVerbs::Render(
                verb.Text(PlotVerbs::Template::Dashboard),
                [](std::string_view) { return std::nullopt; },
                [&verb](std::string_view name) {
                    const auto* slot = PlotVerbs::FindSlot(verb, name);
                    return std::string{slot != nullptr ? PlotVerbs::Unfilled(slot->kind) : "someone"};
                }));
        }

        std::string_view OutcomePhrase(PlotModel::StepOutcome outcome)
        {
            switch (outcome) {
            case PlotModel::StepOutcome::Succeeded:
                return "it worked";
            case PlotModel::StepOutcome::FailedCaught:
                return "it went wrong";
            case PlotModel::StepOutcome::FailedTimeout:
                return "it ran out of time";
            case PlotModel::StepOutcome::None:
                break;
            }
            return {};
        }

        // The plan so far as the model reads it: numbered, and for a step
        // that has already happened, how it went.
        nlohmann::json StepsSoFar(const std::vector<const PlotModel::Step*>& earlier)
        {
            nlohmann::json out = nlohmann::json::array();
            std::size_t n = 0;
            for (const auto* step : earlier) {
                if (step == nullptr) {
                    continue;
                }
                auto line = std::format("{}. {}", ++n, PlotModel::Label(*step));
                if (const auto how = OutcomePhrase(step->outcome); !how.empty()) {
                    line += std::format(" ({})", how);
                }
                out.push_back(std::move(line));
            }
            return out;
        }

        nlohmann::json Options(const std::vector<PlotCandidates::Candidate>& list)
        {
            nlohmann::json out = nlohmann::json::array();
            for (const auto& c : list) {
                out.push_back({{"key", c.key}, {"line", c.line}});
            }
            return out;
        }

        std::string_view KindId(PlotVerbs::SlotKind kind)
        {
            switch (kind) {
            case PlotVerbs::SlotKind::Person:
                return "person";
            case PlotVerbs::SlotKind::Place:
                return "place";
            case PlotVerbs::SlotKind::Thing:
                return "thing";
            }
            return "person";
        }
    } // namespace

    bool SessionCannotDecide(std::string_view error) noexcept
    {
        return error == SkyrimNetAPI::kDecisionUnavailable || error == SkyrimNetAPI::kDecisionNoRoute
               || error == "ai_disabled";
    }

    std::string Sketch(const PlotModel::Step& step, bool agentOpen)
    {
        const auto* verb = PlotVerbs::Find(step.verb);
        if (verb == nullptr) {
            return PlotModel::Label(step);
        }
        return Capitalised(PlotVerbs::Render(
            verb->Text(PlotVerbs::Template::Dashboard),
            [&step, agentOpen](std::string_view name) -> std::optional<std::string> {
                if (name == "agent") {
                    if (!step.actorName.empty()) {
                        return step.actorName;
                    }
                    return agentOpen ? std::nullopt : std::optional<std::string>{"someone"};
                }
                const auto* value = step.FindSlot(name);
                return value != nullptr ? std::optional<std::string>{value->text} : std::nullopt;
            },
            [](std::string_view name) { return std::format("[{}]", name); }));
    }

    bool CanRetry(const PlotModel::Step& failed)
    {
        const auto* verb = PlotVerbs::Find(failed.verb);
        return verb != nullptr && !verb->agentBoundToItem;
    }

    PlotModel::Step Retried(const PlotModel::Step& failed)
    {
        PlotModel::Step step;
        step.type = failed.type;
        step.verb = failed.verb;
        step.slots = failed.slots;
        step.target = failed.target;
        step.targetName = failed.targetName;
        return step;
    }

    std::unordered_set<RE::FormID> FailedAgents(const PlotModel::Plot& plot, const PlotModel::Step& step)
    {
        const auto same = [&step](const PlotModel::Step& other) {
            if (other.verb != step.verb || other.slots.size() != step.slots.size()) {
                return false;
            }
            for (std::size_t i = 0; i < step.slots.size(); ++i) {
                if (other.slots[i].name != step.slots[i].name || other.slots[i].form != step.slots[i].form) {
                    return false;
                }
            }
            return true;
        };
        std::unordered_set<RE::FormID> out;
        for (const auto& past : plot.history) {
            if (past.actor != 0 && past.outcome != PlotModel::StepOutcome::Succeeded && same(past)) {
                out.insert(past.actor);
            }
        }
        return out;
    }

    Adaptation Adapt(PlotModel::Plot& plot, int cap, const Ask& ask, const Replanner& replan)
    {
        // Checked before asking. Whatever the model would say, a plot that
        // has spent its attempts stops, which is what makes "it would not
        // concede; the cap did" an outcome of its own.
        if (plot.adaptations >= cap || plot.history.empty()) {
            return {Revision::CapHit, "the adaptation cap was reached", 0};
        }
        ++plot.adaptations;
        const PlotModel::Step failed = plot.history.back();

        nlohmann::json options = nlohmann::json::array();
        const bool retryable = CanRetry(failed) && plot.cursor < plot.plan.size();
        if (retryable) {
            options.push_back(
                {{"key", kRetry}, {"line", "Try the same step again, with somebody else carrying it out."}});
        }
        options.push_back(
            {{"key", kSubstitute}, {"line", "Try a different way, and plan the rest of the scheme again from here."}});
        options.push_back({{"key", kConcede}, {"line", "Give the scheme up."}});

        std::vector<const PlotModel::Step*> history;
        for (const auto& past : plot.history) {
            history.push_back(&past);
        }
        nlohmann::json ctx;
        ctx["mastermind"] = {{"name", plot.mastermindName}};
        ctx["ambition"] = plot.ambition;
        ctx["scheme"] = plot.scheme;
        ctx["steps"] = StepsSoFar(history);
        ctx["failed"] = std::format("{} ({})", PlotModel::Label(failed), OutcomePhrase(failed.outcome));
        ctx["attempt"] = plot.adaptations;
        ctx["max_attempts"] = cap;
        ctx["options"] = std::move(options);

        const auto result = ask(kAdaptTemplate, ctx);
        if (!result.ok) {
            return {Revision::Conceded, std::format("the decision failed ({})", result.error), 0};
        }
        const auto key = result.Choice(kAdaptQuestion);

        if (key == kRetry && retryable) {
            plot.plan[plot.cursor] = Retried(failed);
            return {Revision::Retried, {}, 1};
        }
        if (key == kSubstitute) {
            auto tail = replan(plot);
            if (!tail.ok) {
                return {Revision::Conceded,
                        std::format("no other way could be planned ({})",
                                    !tail.error.empty() ? tail.error : tail.rejection),
                        0};
            }
            plot.plan.erase(plot.plan.begin() + static_cast<std::ptrdiff_t>(plot.cursor), plot.plan.end());
            const auto added = tail.plan.size();
            for (auto& step : tail.plan) {
                plot.plan.push_back(std::move(step));
            }
            return {Revision::Substituted, {}, added};
        }
        if (key == kConcede) {
            return {Revision::Conceded, "they chose to give it up", 0};
        }
        return {Revision::Conceded, std::format("answered '{}', which was not offered", key), 0};
    }

    RE::FormID ItemHolder(const PlotModel::Plot& plot)
    {
        for (auto it = plot.history.rbegin(); it != plot.history.rend(); ++it) {
            const auto* verb = PlotVerbs::Find(it->verb);
            if (verb != nullptr && verb->obtainsItem && it->outcome == PlotModel::StepOutcome::Succeeded) {
                return it->actor;
            }
        }
        return 0;
    }

    RE::FormID PickAgent(const PlotModel::Plot& plot,
                         const PlotModel::Step& step,
                         const std::vector<AgentOption>& options,
                         const Ask& ask,
                         std::string& error)
    {
        if (options.empty()) {
            return 0;
        }
        if (options.size() == 1) {
            return options.front().npc;
        }

        nlohmann::json list = nlohmann::json::array();
        for (std::size_t i = 0; i < options.size(); ++i) {
            list.push_back({{"key", std::format("c{}", i + 1)}, {"line", options[i].line}});
        }
        nlohmann::json ctx;
        ctx["mastermind"] = {{"name", plot.mastermindName}};
        ctx["scheme"] = plot.scheme;
        ctx["step"] = Sketch(step, true);
        ctx["slot"] = "agent";
        ctx["kind"] = "person";
        ctx["options"] = std::move(list);

        const auto result = ask(kSlotTemplate, ctx);
        if (!result.ok) {
            error = result.error;
            return options.front().npc;
        }
        const auto key = result.Choice(kSlotQuestion);
        for (std::size_t i = 0; i < options.size(); ++i) {
            if (key == std::format("c{}", i + 1)) {
                return options[i].npc;
            }
        }
        error = std::format("answered '{}', which was not offered", key);
        return options.front().npc;
    }

    bool Offerable(const PlotVerbs::Verb& verb, const Input& input, const std::vector<const PlotModel::Step*>& earlier)
    {
        const std::vector<PlotModel::SlotValue> none;
        const auto request = RequestFor(input, none, earlier);
        for (std::size_t i = 0; i < verb.slotCount; ++i) {
            const auto& slot = verb.slots[i];
            if (!PlotCandidates::Asks(slot)) {
                if (!PlotCandidates::Bind(slot, request)) {
                    return false;
                }
                continue;
            }
            if (DependsOnAnotherSlot(slot)) {
                continue;
            }
            if (PlotCandidates::Build(slot, request).empty()) {
                return false;
            }
        }
        return true;
    }

    bool FillSlots(const PlotVerbs::Verb& verb,
                   const Input& input,
                   const std::vector<const PlotModel::Step*>& earlier,
                   const Ask& ask,
                   PlotModel::Step& step,
                   std::string& error,
                   std::string& rejection)
    {
        step = PlotModel::Step{};
        step.verb = std::string{verb.id};
        step.type = verb.profile;
        step.slots.push_back({"mastermind", input.mastermind, input.mastermindName});

        for (std::size_t i = 0; i < verb.slotCount; ++i) {
            const auto& slot = verb.slots[i];
            const auto request = RequestFor(input, step.slots, earlier);

            if (!PlotCandidates::Asks(slot)) {
                auto bound = PlotCandidates::Bind(slot, request);
                if (!bound) {
                    rejection = std::format("{}: nothing earlier to bind {{{}}} to", verb.id, slot.name);
                    return false;
                }
                step.slots.push_back(std::move(*bound));
                continue;
            }

            const auto list = PlotCandidates::Build(slot, request);
            if (list.empty()) {
                rejection = std::format("{}: nobody or nothing can fill {{{}}}", verb.id, slot.name);
                return false;
            }

            const PlotCandidates::Candidate* chosen = &list.front();
            if (list.size() > 1) {
                nlohmann::json ctx;
                ctx["mastermind"] = {{"name", input.mastermindName}};
                ctx["scheme"] = input.scheme;
                ctx["step"] = Sketch(step);
                ctx["slot"] = slot.name;
                ctx["kind"] = KindId(slot.kind);
                ctx["options"] = Options(list);

                const auto result = ask(kSlotTemplate, ctx);
                if (!result.ok) {
                    error = result.error;
                    return false;
                }
                const auto key = result.Choice(kSlotQuestion);
                chosen = nullptr;
                for (const auto& c : list) {
                    if (c.key == key) {
                        chosen = &c;
                    }
                }
                if (chosen == nullptr) {
                    rejection =
                        std::format("{}: {{{}}} was answered '{}', which was not offered", verb.id, slot.name, key);
                    return false;
                }
            }
            step.slots.push_back(chosen->value);
        }

        // The person the step is done to, for the resolution model, which
        // sizes a step by its target's importance and distance and holds a
        // conspicuous one while that target is near the player. The first
        // person slot is the one the verb acts on.
        for (std::size_t i = 0; i < verb.slotCount; ++i) {
            if (verb.slots[i].kind == PlotVerbs::SlotKind::Person) {
                if (const auto* value = step.FindSlot(verb.slots[i].name)) {
                    step.target = value->form;
                    step.targetName = value->text;
                }
                break;
            }
        }
        return true;
    }

    Outcome Plan(const Input& input, const Ask& ask)
    {
        Outcome out;
        // Reserved up front: `earlier` holds pointers into the plan, which a
        // reallocation would leave dangling.
        out.plan.reserve(input.maxSteps);
        std::vector<const PlotModel::Step*> earlier = input.before;

        for (std::size_t position = 0; position < input.maxSteps; ++position) {
            std::vector<const PlotVerbs::Verb*> offered;
            nlohmann::json options = nlohmann::json::array();
            for (const auto& verb : PlotVerbs::kVerbs) {
                if (Offerable(verb, input, earlier)) {
                    offered.push_back(&verb);
                    options.push_back({{"key", verb.id}, {"line", VerbLine(verb)}});
                }
            }
            const bool mayStop = !out.plan.empty();
            if (mayStop) {
                options.push_back({{"key", kDone}, {"line", "Nothing more: the steps so far accomplish the scheme."}});
            }
            if (offered.empty()) {
                break;
            }

            nlohmann::json ctx;
            ctx["mastermind"] = {{"name", input.mastermindName}, {"standing", input.standing}};
            ctx["bio"] = input.bio;
            ctx["ambition"] = input.ambition;
            ctx["scheme"] = input.scheme;
            ctx["steps"] = StepsSoFar(earlier);
            ctx["options"] = std::move(options);

            const auto result = ask(kVerbTemplate, ctx);
            if (!result.ok) {
                out.error = result.error;
                out.plan.clear();
                return out;
            }
            const auto key = result.Choice(kVerbQuestion);
            if (mayStop && key == kDone) {
                break;
            }
            const PlotVerbs::Verb* verb = nullptr;
            for (const auto* candidate : offered) {
                if (candidate->id == key) {
                    verb = candidate;
                }
            }
            if (verb == nullptr) {
                out.rejection = std::format("step {} was answered '{}', which was not offered", position + 1, key);
                out.plan.clear();
                return out;
            }

            PlotModel::Step step;
            if (!FillSlots(*verb, input, earlier, ask, step, out.error, out.rejection)) {
                out.plan.clear();
                return out;
            }
            out.plan.push_back(std::move(step));
            earlier.push_back(&out.plan.back());
        }

        if (out.plan.empty()) {
            out.rejection = "no verb could be offered";
            return out;
        }
        out.ok = true;
        return out;
    }
} // namespace NarrativeEngine::PlotPlanning
