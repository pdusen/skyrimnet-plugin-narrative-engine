#pragma once

#include <string>
#include <vector>

#include <PlotCasting.h>
#include <PlotModel.h>
#include <PlotThread.h>

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
    };

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

    [[nodiscard]] AmbitionOutcome ParseAmbition(const std::string& response, const TextLimits& limits);

    // --- Call 3: how ------------------------------------------------------

    struct PlanOutcome
    {
        bool ok = false;
        std::string rejection;
        std::vector<PlotModel::Step> plan;
    };

    struct PlanLimits
    {
        // The whole plan. One step is an errand, not a scheme.
        //
        // NEITHER BOUND IS STATED IN THE PROMPT, and both have been
        // stated at some point. A range anchors hardest -- offering "2
        // to 6" produced 4, 4, 4, the midpoint every time -- but a bare
        // ceiling anchors too: naming six produced 6, 6, 4 on the run
        // after it. The prompt now says only "take as many steps as this
        // scheme needs", and these exist to catch an answer that has
        // gone wrong rather than to shape one that has not.
        //
        // The ceiling is correspondingly generous. Ten is not a target,
        // it is the point past which a plan has stopped being a scheme
        // and become a list.
        std::size_t minSteps = 2;
        std::size_t maxSteps = 10;
        // Asked for at about fifteen words. Same reasoning as the two
        // above: a generous ceiling, not a style rule.
        std::size_t maxDescription = 160;

        // A role's search sentence. Generous, like the others: the
        // prompt asks for it to be no narrower than the step requires,
        // and this is here to stop an essay reaching the index rather
        // than to enforce that.
        std::size_t maxRoleQuery = 200;

        // A role's fallback label, asked for at one to three words. This
        // one is TIGHT on purpose, because unlike the fields above the
        // label is rendered directly into a step's title where a long
        // string does not fit -- and a model that answers a whole clause
        // here has misread the field rather than merely overrun it.
        std::size_t maxRoleLabel = 40;
    };

    [[nodiscard]] PlanOutcome ParsePlan(const std::string& response, const PlanLimits& limits);

    // --- The engine-bound half --------------------------------------------

    // Run all three calls over `shortlist` and fill `plot` in, including
    // which of the shortlist ended up masterminding it.
    //
    // BLOCKS on the plot worker, three times, which is the point:
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
    [[nodiscard]] bool Compose(const PlotThread::Token& pt,
                               const PlotCasting::Population& population,
                               const std::vector<RE::FormID>& shortlist,
                               PlotModel::Plot& plot);
} // namespace NarrativeEngine::PlotBirth
