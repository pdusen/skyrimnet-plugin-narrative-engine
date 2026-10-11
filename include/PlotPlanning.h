#pragma once

#include <PlotCandidates.h>
#include <PlotModel.h>
#include <PlotVerbs.h>
#include <SkyrimNetAPI.h>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// PlotPlanning — laying out a scheme as a sequence of verbs, by decision.
//
// The scheme is already written. This decides how it gets done: which verb
// comes next, then what fills each of that verb's slots, one decision at a
// time, until the model says the steps so far accomplish the scheme or the
// plan reaches its cap. Every answer is a key from a list built here, so a
// plan cannot name a person, a place, a thing or a kind of step that does
// not exist.
//
// The decision call itself is a parameter. The plot worker binds it to
// SkyrimNetAPI::SendCustomDecision; a suite binds it to a script.
namespace NarrativeEngine::PlotPlanning
{
    inline constexpr const char* kVerbTemplate = "narrative_engine/plot_verb";
    inline constexpr const char* kSlotTemplate = "narrative_engine/plot_slot";

    // Both templates ask exactly one question under these ids.
    inline constexpr const char* kVerbQuestion = "verb";
    inline constexpr const char* kSlotQuestion = "pick";

    // The option that ends a plan. Offered from the second position on: one
    // step is an errand, not a scheme.
    inline constexpr const char* kDone = "done";

    using Ask = std::function<SkyrimNetAPI::DecisionResult(std::string_view templateName, const nlohmann::json& ctx)>;

    struct Input
    {
        const PlotCasting::Population& population;
        const PlotMenus::World& world;

        RE::FormID mastermind = 0;
        std::string mastermindName;
        // "who leads it", "who is senior in it", or empty.
        std::string standing;
        std::string ambition;
        std::string scheme;
        // The mastermind's biography sections, as PlotBirth renders them.
        nlohmann::json bio = nlohmann::json::object();

        PlotCasting::AlivePredicate alive;
        std::function<std::string(RE::FormID)> occupation;

        // Steps the plot already has ahead of the first position planned
        // here, oldest first. Empty at birth; the plot's history when
        // adaptation re-plans from a failure.
        std::vector<const PlotModel::Step*> before;

        std::size_t maxSteps = 6;
    };

    struct Outcome
    {
        bool ok = false;
        // A decision that could not be asked or was not answered: the
        // SkyrimNetAPI error code. A caller treats `unavailable`,
        // `no_decisions_route` and `ai_disabled` as "this session cannot
        // plan" rather than as one bad plot.
        std::string error;
        // A plan that came back wrong.
        std::string rejection;
        std::vector<PlotModel::Step> plan;
    };

    // Whether `verb` can be offered with these steps behind it: every slot
    // it would ask has somebody to ask about, and every slot it binds has
    // something to bind to. A slot whose candidates depend on another slot
    // of the same step is judged once that one is filled.
    [[nodiscard]] bool Offerable(const PlotVerbs::Verb& verb,
                                 const Input& input,
                                 const std::vector<const PlotModel::Step*>& earlier);

    // Plan from the first open position to the end.
    [[nodiscard]] Outcome Plan(const Input& input, const Ask& ask);

    // Fill one verb's slots, given the steps ahead of it. On success `step`
    // carries the verb, its profile, the mastermind and every slot. A slot
    // with one candidate is filled without asking.
    [[nodiscard]] bool FillSlots(const PlotVerbs::Verb& verb,
                                 const Input& input,
                                 const std::vector<const PlotModel::Step*>& earlier,
                                 const Ask& ask,
                                 PlotModel::Step& step,
                                 std::string& error,
                                 std::string& rejection);

    // The sentence a planned step reads as before anybody is cast, with
    // each unfilled slot shown as its name in brackets. What the plan so
    // far, and the slot being asked about, look like to the model.
    // With `agentOpen`, an uncast agent shows as [agent] too, for the
    // question that casts one.
    [[nodiscard]] std::string Sketch(const PlotModel::Step& step, bool agentOpen = false);

    // Whoever holds the item a step binds: the agent of the plot's latest
    // step that obtained one and succeeded. 0 when there is none.
    [[nodiscard]] RE::FormID ItemHolder(const PlotModel::Plot& plot);

    // One agent a step could be given, with the line the model reads.
    struct AgentOption
    {
        RE::FormID npc = 0;
        std::string line;
    };

    // Choose the agent from the ladder's eligible list, best rung first.
    // One option is taken without asking. Several are decided, with the
    // step's sentence showing [agent] where they go. A decision that fails,
    // or names an option that was not offered, takes the first -- the
    // ladder's own order -- rather than stalling a plot already under way;
    // `error` then says why. 0 only for an empty list.
    [[nodiscard]] RE::FormID PickAgent(const PlotModel::Plot& plot,
                                       const PlotModel::Step& step,
                                       const std::vector<AgentOption>& options,
                                       const Ask& ask,
                                       std::string& error);

    // Whether an error code means no decision will be answered this
    // session, however many more are asked.
    [[nodiscard]] bool SessionCannotDecide(std::string_view error) noexcept;
} // namespace NarrativeEngine::PlotPlanning
