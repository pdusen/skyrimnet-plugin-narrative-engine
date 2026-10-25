#pragma once

#include <PlotCasting.h>
#include <PlotMenus.h>
#include <PlotModel.h>
#include <PlotVerbs.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// PlotCandidates — the short list a verb slot is filled from.
//
// Every slot of a verb names a Rule (PlotVerbs.h). This turns a rule into
// the people, places or things that rule admits, ordered by how plausibly
// the mastermind would reach for them, cut to a list a model can read in
// one go, and keyed c1, c2, ... for a decision question to answer with.
//
// Every person on every list has already passed the rules no slot has to
// state: alive, not the mastermind, and not already filling another slot
// of the same step. Children, ghosts and the player never reach the
// population in the first place.
//
// Pure. Everything engine-bound -- liveness, what somebody does for a
// living, settlement names -- arrives through the Request, so a suite can
// drive it against a fabricated province.
namespace NarrativeEngine::PlotCandidates
{
    // Why somebody is on a list, beside their name: what the model reads of
    // them, which plot_slot.prompt words. Empty for a place or a thing, whose
    // name is all there is to say.
    struct Why
    {
        // What they do, from their biography. May be empty.
        std::string occupation;
        // How the mastermind knows them, as a PlotMenus::RelationId, or
        // empty for a stranger.
        std::string relation;
        // Where a stranger lives, when that is known.
        std::string lives;
        // The scheme, or how it finishes, names them.
        bool named = false;
    };

    struct Candidate
    {
        // c1, c2, ... in list order: what the decision question offers
        // and what its answer is matched against.
        std::string key;
        // What filling the slot with this candidate writes into the step,
        // and the name the model reads.
        PlotModel::SlotValue value;
        Why why;
    };

    struct Request
    {
        const PlotCasting::Population& population;
        RE::FormID mastermind = 0;
        const PlotMenus::World& world;

        // This step's slots filled so far, in fill order. A dependent slot
        // reads its anchor from here (ask_around's place, from its mark),
        // and nobody already in one slot is offered for another.
        const std::vector<PlotModel::SlotValue>& filled;

        // The plot's earlier steps, oldest first: history, then plan up to
        // this position. Bound items come from here, and so does who an
        // earlier step reached.
        const std::vector<const PlotModel::Step*>& earlier;

        // Empty means everybody is alive.
        PlotCasting::AlivePredicate alive;

        // One short phrase saying what somebody does, from their biography.
        // Empty, or an empty answer, leaves it off the line.
        std::function<std::string(RE::FormID)> occupation;

        // The text whose people are offered first, and offered at all for a
        // slot that asks whom the mastermind knows: the scheme and how it
        // finishes. A plot against Maven shows its mastermind knows of her,
        // whatever the ladder says. Empty names nobody.
        std::string_view naming{};

        // People no slot may be filled with, on top of the unstated rules:
        // the people of the step a preparation or a theft serves, so the
        // bribe before a theft never pays the person being robbed.
        std::unordered_set<RE::FormID> excluded{};
    };

    struct Limits
    {
        // Twelve people, as PlotMenus settled on: a list a person could
        // read, and short enough that the decision API's one-line options
        // stay a list rather than a page.
        std::size_t people = 12;
        std::size_t places = 8;
        // More than the curated pool holds, so every item is offered: a cut
        // list silently loses whatever the pool happens to list last.
        std::size_t things = 64;
    };

    // Whether a slot is chosen by asking. A bound slot is not: it is
    // whatever it is bound to.
    [[nodiscard]] bool Asks(const PlotVerbs::Slot& slot) noexcept;

    // A BoundItem slot's value: the item the latest earlier step that
    // obtains one actually got, which means it succeeded. Nothing when no
    // earlier step has, which is always the case for a plan not yet run.
    [[nodiscard]] std::optional<PlotModel::SlotValue> Bind(const PlotVerbs::Slot& slot, const Request& request);

    // The list a slot is filled from. Empty when the rule admits nobody,
    // which is what keeps a verb from being offered at all. A bound slot's
    // list is its one bound value.
    [[nodiscard]] std::vector<Candidate> Build(const PlotVerbs::Slot& slot,
                                               const Request& request,
                                               const Limits& limits = {});

    // A goal's roles, as the scheme call named them, resolved to who and what
    // fills each slot.
    struct Resolution
    {
        // In the goal's slot order, each naming its form and how it is said.
        std::vector<PlotModel::SlotValue> values;
        // Empty when every role resolved; otherwise a sentence for each that
        // did not, joined, for the scheme call's retry to act on.
        std::string problem;
    };

    // Resolve the roles `named` gives a goal -- each a slot's name and the
    // words the scheme call wrote for it, with no form -- and check each
    // against its slot's rule, in slot order, so a denunciation's authority
    // is judged against the mark already resolved.
    //
    // A person is whoever the words name among the living: by the words of
    // their name, a word fewer people carry counting for more, so a whole
    // name or a rare one settles it and "Jarl Idgrod" is two people. A place
    // is the settlement whose name the words hold; a thing, whatever the slot
    // could be filled with whose name they hold, the pool's and what the
    // mastermind's people can make alike. Being named does not make somebody
    // known to the mastermind, as it does for a list planning offers.
    [[nodiscard]] Resolution ResolveRoles(const PlotVerbs::Verb& goal,
                                          const std::vector<PlotModel::SlotValue>& named,
                                          const Request& request);

    // Whether somebody the mastermind could send, or the mastermind, is able
    // to make this thing by this craft. Who is busy today does not matter:
    // a plan is laid before its steps are cast.
    [[nodiscard]] bool CanBeMade(const Request& request, PlotRecipes::Craft craft, RE::FormID form);

    // A few things the mastermind's craftsmen can make, as they are said: up
    // to `perCraft` of each craft, spread from the cheapest to the dearest.
    // What the scheme call is shown, so a finish can name one.
    [[nodiscard]] std::vector<std::string> MadeExamples(const Request& request, std::size_t perCraft);

    // Why a person is on a list: what they do, and how the mastermind knows
    // them or where they live.
    [[nodiscard]] Why Describe(const Request& request, const PlotCasting::Member& member);

    // Everybody `text` names. A person is named when a word of their name
    // stands in the text as a whole word -- a trailing 's dropped -- and no
    // more than two people's names carry that word: "Maven" names Maven
    // Black-Briar, while "Black-Briar", her whole family's, names nobody.
    // Case matters, so "Hulda" is a name and "hulda" is not.
    [[nodiscard]] std::unordered_set<RE::FormID> NamedIn(const PlotCasting::Population& population,
                                                         std::string_view text);

    // The step whose people a later step treats as aware of this one, and
    // how: whether each person slot's awareness fired for the outcome the
    // step actually had. A timeout reaches nobody.
    [[nodiscard]] bool AwarenessFired(PlotVerbs::Awareness awareness, PlotModel::StepOutcome outcome) noexcept;
} // namespace NarrativeEngine::PlotCandidates
