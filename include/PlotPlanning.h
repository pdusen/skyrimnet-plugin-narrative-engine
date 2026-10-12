#pragma once

#include <PlotCandidates.h>
#include <PlotModel.h>
#include <PlotVerbs.h>
#include <SkyrimNetAPI.h>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

// PlotPlanning — laying out a scheme as a sequence of verbs, by decision.
//
// The scheme is already written. This decides how it gets done, backwards
// from the end, because every question it asks has to be one the decision
// model answers by matching rather than by planning:
//
//   1. The goal: which kind of step, once done, accomplishes the scheme.
//   2. Getting an item: a step that needs one nobody holds gets a theft or a
//      purchase of it placed just before it, from somebody not in that step.
//   3. Preparations: concrete steps from PlotVerbs::kPreparations, their
//      bound slots already filled, each just before the step it prepares --
//      asked until the model answers `ready` (offered from the second step)
//      or the plan reaches its cap. Only the goal and the steps that get
//      items are prepared; a preparation never gets one of its own.
//
// A forward "what comes next?" loop came first. The model answered it with
// whichever verb best matched the scheme, every time, to the cap; see
// Milestone 2's Step 9. So no question here asks what comes next.
//
// Every answer is a key from a list built here, so a plan cannot name a
// person, a place, a thing or a kind of step that does not exist, and no
// option or filler that would repeat a step is ever on a list.
//
// The decision call itself is a parameter. The plot worker binds it to
// SkyrimNetAPI::SendCustomDecision; a suite binds it to a script.
namespace NarrativeEngine::PlotPlanning
{
    // Each template asks exactly one question, under the id beside it.
    inline constexpr const char* kGoalTemplate = "narrative_engine/plot_goal";
    inline constexpr const char* kGoalQuestion = "goal";
    inline constexpr const char* kObtainTemplate = "narrative_engine/plot_obtain";
    inline constexpr const char* kObtainQuestion = "obtain";
    inline constexpr const char* kPrepareTemplate = "narrative_engine/plot_prepare";
    inline constexpr const char* kPrepareQuestion = "prepare";
    inline constexpr const char* kSlotTemplate = "narrative_engine/plot_slot";
    inline constexpr const char* kSlotQuestion = "pick";

    // The preparation answer that ends a plan.
    inline constexpr const char* kReady = "ready";

    // Adaptation's one question, and its three answers.
    inline constexpr const char* kAdaptTemplate = "narrative_engine/plot_adapt";
    inline constexpr const char* kAdaptQuestion = "next";
    inline constexpr const char* kSubstitute = "substitute";
    inline constexpr const char* kConcede = "concede";

    // How long a plan may run. A substitute plans what is left of it.
    inline constexpr std::size_t kPlanCap = 6;

    // The fewest steps any plan is given room for: a goal that needs an
    // item, and the step that gets it.
    inline constexpr std::size_t kPlanFloor = 2;

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

        // The verb the scheme ends in, when the call that wrote the scheme
        // also chose it. Planned last without asking, if it can be filled;
        // empty, or unfillable, asks the goal question instead.
        std::string goal;
        // That last step written out, naming its people and any item. Told
        // to every question, and whoever it names heads every list.
        std::string finish;

        PlotCasting::AlivePredicate alive;
        std::function<std::string(RE::FormID)> occupation;

        // Steps the plot already has ahead of the plan, oldest first. Empty
        // at birth; the plot's history when adaptation re-plans from a
        // failure. Nothing in it is offered again, and an item a step in it
        // got is bound rather than got a second time.
        std::vector<const PlotModel::Step*> before;

        std::size_t maxSteps = kPlanCap;

        // Ways the plot has given up on. No step of an abandoned verb
        // against its abandoned person is planned, in any position.
        std::vector<PlotModel::Abandoned> abandoned;
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
        // How many decisions were asked, answered or not.
        std::size_t decisions = 0;
    };

    // Plan from the goal back, after whatever `input.before` holds.
    [[nodiscard]] Outcome Plan(const Input& input, const Ask& ask);

    // Whether two steps are the same step: one verb, and every slot either
    // has filled the same way. Who carried it out does not enter into it.
    [[nodiscard]] bool SameStep(const PlotModel::Step& a, const PlotModel::Step& b);

    // The sentence a planned step reads as before anybody is cast, with
    // each unfilled slot shown as its name in brackets. What the plan so
    // far, and the slot being asked about, look like to the model.
    // With `agentOpen`, an uncast agent shows as [agent] too, for the
    // question that casts one.
    [[nodiscard]] std::string Sketch(const PlotModel::Step& step, bool agentOpen = false);

    // Whoever holds the item `step` needs: the agent of the plot's latest
    // step that got that very item and succeeded. 0 when there is none.
    [[nodiscard]] RE::FormID ItemHolder(const PlotModel::Plot& plot, const PlotModel::Step& step);

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

    // --- Adaptation ------------------------------------------------------

    enum class Revision : std::uint8_t
    {
        Substituted, // another way from the failed position on
        Conceded,    // given up
        CapHit       // out of attempts before anything was asked
    };

    struct Adaptation
    {
        Revision revision = Revision::Conceded;
        // Why a plot was conceded, for the trace.
        std::string reason;
        // How many steps a substitute put in place of the failed one.
        std::size_t newTail = 0;
        // How many decisions planning the substitute asked.
        std::size_t decisions = 0;
        // What the failure abandoned, newly; already on the plot.
        std::vector<PlotModel::Abandoned> abandoned;
    };

    // Plans what is left of a plot after a failure, from its history. The
    // plot worker binds this to PlotBirth::Replan.
    using Replanner = std::function<Outcome(const PlotModel::Plot&)>;

    // Whether the step that just failed -- at the plot's cursor -- was its
    // finish, the last step of the plan. Such a failure ends the plot rather
    // than being adapted around: the finish is what the scheme was for, and a
    // different act against somebody else is a different scheme.
    [[nodiscard]] bool FinishFailed(const PlotModel::Plot& plot) noexcept;

    // The person a step acts on: its verb's first person slot. 0 for a step
    // with no verb or no such slot filled.
    [[nodiscard]] RE::FormID TargetOf(const PlotModel::Step& step);

    // Whether `step` is there to serve `later`: one of its preparations,
    // with the bound person the same, or the step that gets the item it
    // uses.
    [[nodiscard]] bool Serves(const PlotModel::Step& step, const PlotModel::Step& later);

    // The route a failure abandons: the failed step -- the last entry in the
    // plot's history, still at its cursor -- and every later planned step it
    // served, directly or through another, short of the finish. A failed
    // finish is never adapted (FinishFailed), so the finish itself is never
    // part of a route.
    [[nodiscard]] std::vector<PlotModel::Abandoned> RouteOf(const PlotModel::Plot& plot);

    // Adapt after the step at the plot's cursor failed, which is already the
    // last entry in its history. Asks one decision -- another way, or give
    // up -- unless the cap is spent. Another way abandons the failed route
    // for good (RouteOf, onto `plot.abandoned`) and replaces the plan from
    // the cursor on; nothing behind the cursor ever changes. A failed
    // decision, an answer that was not offered, or another way that cannot
    // be planned all concede.
    //
    // There is no retry. A failure says the route it was on does not work,
    // and the same step with somebody else carrying it is the same route.
    [[nodiscard]] Adaptation Adapt(PlotModel::Plot& plot, int cap, const Ask& ask, const Replanner& replan);

    // Whether an error code means no decision will be answered this
    // session, however many more are asked.
    [[nodiscard]] bool SessionCannotDecide(std::string_view error) noexcept;
} // namespace NarrativeEngine::PlotPlanning
