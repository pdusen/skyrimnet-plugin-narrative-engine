#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <PlotCasting.h>
#include <PlotModel.h>
#include <PlotPlanning.h>
#include <PlotThread.h>
#include <PlotVerbs.h>

// PlotBirth — inventing one scheme, in three questions instead of one.
//
// ---------------------------------------------------------------------
// WHY THREE CALLS
//
// This was one call. It was handed a mastermind, a menu of people, a
// menu of objects, a menu of factions and the step manifest, and asked
// for an agenda, an objective and a route in a single JSON object.
//
// It produced the same plot every time. Not similar plots — the same
// four steps against different nouns, run after run, across masterminds
// in different holds with different lives. Fixing the inputs helped (the
// character profile was missing entirely for a while, which is its own
// story) and fixing the prompt helped, but the shape of the failure
// never changed, because the shape of the ASK never changed: a model
// with one turn to answer everything answers the easy part well and
// falls back to a template for the rest.
//
// So it is three questions now, each small enough to be answered on its
// own merits:
//
//   1. WHO. The plugin draws a weighted shortlist of eligible people;
//      the model picks which of them is the one with something to hide.
//   2. WHY and WHAT. Given that person's profile and recent memories:
//      their lifelong agenda, and one concrete scheme that is a stepping
//      stone toward it.
//   3. HOW. Given the agenda and the scheme: the ordered steps.
//
// Each call sees the previous answers, so the chain narrows rather than
// making three independent guesses.
//
// ---------------------------------------------------------------------
// NO MENUS, FOR NOW
//
// None of these calls is offered a list of people, objects or factions
// to name. Steps come back as a type plus a sentence, and their targets
// are unresolved.
//
// That is a deliberate, temporary loss. Menus were what made a plan
// EXECUTABLE — a step whose target is a resolved form has somebody to
// travel to and something to put in an inventory — and validating an
// index against a menu is what kept the model from inventing people.
// Both have to come back before plots can touch the world. They are out
// right now because the question on the table is whether decomposing the
// ASK produces better schemes, and menus are a large confound: half the
// sameness could always have been the item pool rather than the prompt.
//
// See docs/prior-art/NAME_RESOLUTION_FAILURE_MODES.md for what accepting
// free-text names cost the project this one learned from. Nothing here
// accepts a name as an entity — the sentences are DESCRIPTIONS, rendered
// and read, never resolved.
namespace NarrativeEngine::PlotBirth
{
    // --- Call 1: who ------------------------------------------------------

    struct CastOutcome
    {
        bool ok = false;
        std::string rejection;
        // Index into the shortlist the prompt was rendered from,
        // recovered from the `form_id` the model named. A membership
        // test against a small closed list, which is the one piece of
        // this pipeline that still validates the way the menus did.
        std::size_t choice = 0;
    };

    // Pure. Response text in, a shortlist index out.
    //
    // The model answers with a FORM ID, not a position -- the pattern
    // narrative_engine_action_select already uses for choosing a letter
    // or visit sender. Positions are free to get subtly wrong, and an
    // off-by-one silently hands the scheme to the wrong person with
    // nothing downstream ever looking incorrect; a form id either is one
    // of the five it was given or it is not.
    //
    // Read as a hex STRING ("0x13BB3"), which is how the other prompts
    // render one, with a bare-hex and a plain-number spelling accepted
    // as well. Leniency is safe here precisely because the membership
    // test follows: a mis-read cannot select the wrong candidate, only
    // fail to select any.
    [[nodiscard]] CastOutcome ParseCast(const std::string& response, const std::vector<RE::FormID>& shortlist);

    // True when this NPC can be put in front of the casting call at all:
    // SkyrimNet resolves them to a UUID, so a profile actually renders.
    //
    // SkyrimNet returns 0 for most NPCs -- roughly four in five, on the
    // runs measured -- and a candidate it cannot describe renders as a
    // name and a hold while the others get a paragraph each. That is not
    // a choice between five people, it is a choice between the one the
    // model was told about and four it was not, and the first run under
    // this pipeline picked the described candidate three times out of
    // four.
    //
    // Engine-bound; safe from the plot worker, like the other reads
    // here.
    [[nodiscard]] bool IsCastable(RE::FormID npc);

    // --- Call 2: why, and what --------------------------------------------

    struct AmbitionOutcome
    {
        bool ok = false;
        std::string rejection;

        // The AGENDA: the larger thing this person is ultimately after,
        // and deliberately not something this system can ever finish.
        // "Eliminate Talos worship in Skyrim." "Become Queen of Skyrim."
        std::string ambition;

        // The concrete thing this one scheme exists to achieve: a
        // stepping stone toward the agenda that does not reach it.
        std::string scheme;

        // The id of the verb the scheme ends in: the step that, once done,
        // means the scheme is done. Always one PlotVerbs::Find knows.
        std::string goal;

        // That last step written out as an act, naming its people and any
        // item: "steal the Shatter-Shield logbook from Suvaris Atheron".
        // The scheme says what it is all for; this says how it ends.
        std::string finish;

        // Who and what fills each slot of the goal, in its slot order: the
        // scheme call's `roles`, resolved. Planning fills the goal from them.
        std::vector<PlotModel::SlotValue> roles;
    };

    // Resolves the roles a scheme call named for its goal: `named` holds each
    // slot's name and the words written for it. Fills `resolved` and returns
    // nothing when every role fits, else why not, for the retry.
    using RoleResolver = std::function<std::string(const PlotVerbs::Verb& goal,
                                                   const std::vector<PlotModel::SlotValue>& named,
                                                   std::vector<PlotModel::SlotValue>& resolved)>;

    struct TextLimits
    {
        // Hard ceilings, applied AFTER sanitizing.
        //
        // Both are asked for at about twenty words. The ceilings are
        // well clear of that, because a rejection here costs the calls
        // already spent -- they exist to stop an essay reaching the
        // co-save and the dashboard, not to enforce the word count.
        std::size_t maxAmbition = 300;
        std::size_t maxScheme = 300;
    };

    // `playerName` is the player character's name; an answer that names them
    // is refused, as one saying "Dragonborn" always was.
    //
    // `roles` must name every slot of the goal, each as a string. With a
    // resolver they are resolved into `AmbitionOutcome::roles`, and an answer
    // whose roles do not fit is refused with the resolver's reason; without
    // one, only their presence is checked. Who and what the finish's prose
    // names is not read: prose does not say which name fills which role.
    [[nodiscard]] AmbitionOutcome ParseAmbition(const std::string& response,
                                                const TextLimits& limits,
                                                std::string_view playerName = {},
                                                const RoleResolver& resolveRoles = {});

    // --- Call 3: how ------------------------------------------------------
    //
    // No longer a prompt with a parser. The plan is laid out by decisions,
    // from the goal back; see PlotPlanning.

    // A scheme as the dispatch memories quote it: "as part of my plan to
    // {scheme}". True when the scheme opens with a verb, as the ambition
    // prompt asks; false for one that opens with a pronoun, an article, or a
    // name followed by what they mean to do ("Korir plans to ..."), each of
    // which would read as broken grammar in every memory the plot writes.
    [[nodiscard]] bool SchemeOpensWithVerb(std::string_view scheme);

    // --- The engine-bound half --------------------------------------------

    // What somebody does, as one short phrase for their line on a decision's
    // list: the first sentence of their biography's occupation section.
    [[nodiscard]] std::string Occupation(RE::FormID npc);

    // Plan what is left of `plot` after a failure, from its history, by the
    // same decisions birth uses. What adaptation's substitute runs.
    [[nodiscard]] PlotPlanning::Outcome Replan(const PlotThread::Token& pt,
                                               const PlotCasting::Population& population,
                                               const PlotModel::Plot& plot);

    // False once SkyrimNet has said this session cannot answer a decision.
    // Every birth plans by decision, so the caller skips birth outright
    // rather than spending two chat calls on a plot that cannot be planned.
    [[nodiscard]] bool CanDecide() noexcept;

    // Clears the latch above, so a session with a decision provider newly
    // set up gets its plots.
    void OnSessionStart() noexcept;

    // The player character's name, as the plugin thread last read it. The
    // plot worker never touches the player; birth reads this instead, to
    // tell the ambition call who the player is and to refuse an answer
    // that names them.
    void SetPlayerName(std::string name);
    [[nodiscard]] std::string PlayerName();

    // Run the two chat calls and the planning decisions over `shortlist`
    // and fill `plot` in, including which of the shortlist ended up
    // masterminding it.
    //
    // BLOCKS on the plot worker, call after call, which is the point:
    // nothing else runs there, so there is nobody to yield to and no
    // reason to hand the rest of the work to a callback. Same argument
    // GossipContent makes for its own calls.
    //
    // Each call is asked TWICE at most: a refused answer is put back to
    // the model with the reason it was refused, once. See AskTwice.
    //
    // Returns false when any call failed or was still wrong on its
    // retry, having logged why and at which stage. The caller frees the
    // slot and spends no plot id, so a birth that dies at call 3 costs
    // call budget and nothing else.
    //
    // `decisions` is how many decisions planning asked, kept or not, for
    // the trace.
    [[nodiscard]] bool Compose(const PlotThread::Token& pt,
                               const PlotCasting::Population& population,
                               const std::vector<RE::FormID>& shortlist,
                               PlotModel::Plot& plot,
                               std::size_t& decisions);
} // namespace NarrativeEngine::PlotBirth
