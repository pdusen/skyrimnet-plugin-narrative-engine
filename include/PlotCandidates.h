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
    struct Candidate
    {
        // c1, c2, ... in list order: what the decision question offers
        // and what its answer is matched against.
        std::string key;
        // What filling the slot with this candidate writes into the step.
        PlotModel::SlotValue value;
        // The one line the model reads: who or what this is, and why it
        // is on the list.
        std::string line;
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

        // The plot's scheme. Whoever it names is offered first, and is
        // offered at all for a slot that asks whom the mastermind knows: a
        // scheme against Maven shows they know of her, whatever the ladder
        // says. Empty names nobody.
        std::string_view scheme{};
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

    // A BoundItem slot's value: the item the latest earlier step whose verb
    // obtains one was given. Nothing when no earlier step obtains an item.
    [[nodiscard]] std::optional<PlotModel::SlotValue> Bind(const PlotVerbs::Slot& slot, const Request& request);

    // The list a slot is filled from. Empty when the rule admits nobody,
    // which is what keeps a verb from being offered at all. A bound slot's
    // list is its one bound value.
    [[nodiscard]] std::vector<Candidate> Build(const PlotVerbs::Slot& slot,
                                               const Request& request,
                                               const Limits& limits = {});

    // A person's line on a list: name, what they do, and how the
    // mastermind knows them or where they live.
    [[nodiscard]] std::string Describe(const Request& request, const PlotCasting::Member& member);

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
