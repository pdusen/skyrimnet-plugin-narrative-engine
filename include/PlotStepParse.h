#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include <PlotModel.h>

// PlotStepParse — the parts of a step that BOTH composition and
// adaptation have to read the same way.
//
// Birth and adaptation each have their own response parser, because
// they validate different things around the plan: birth checks a
// mastermind choice and an ambition, adaptation checks a concede/revise
// decision and that the new tail differs from the old one. What they
// share is the steps themselves, and a step is now more than a type and
// a sentence.
//
// This exists because they had already drifted. Adaptation was never
// taught about roles, so a revised step came back with no agent
// description at all and fell through to a random draw -- the old
// behaviour, silently, in the middle of a plot that had been casting
// deliberately up to that point. One definition means the next field a
// step gains cannot be added to one parser and forgotten in the other.
namespace NarrativeEngine::PlotStepParse
{
    struct RoleLimits
    {
        // A role's search sentence. Generous: the prompt asks for it to
        // be no narrower than the step requires, and this is here to
        // stop an essay reaching the index rather than to enforce that.
        std::size_t maxQuery = 200;

        // The agent's fallback label, asked for at one to three words.
        // Tight on purpose -- a model answering a whole clause here has
        // misread the field rather than merely overrun it.
        std::size_t maxLabel = 40;
    };

    // Does this text name the player?
    //
    // Only one word, deliberately. "Dragonborn" is what a model reaches
    // for when it means the player, and it is the one title no NPC in
    // Skyrim wears -- Miraak is the first Dragonborn and is still called
    // Miraak. So it is a reliable marker and costs nothing legitimate.
    //
    // Whole-word and case-insensitive, so "dragonborn" and "Dragonborn's"
    // both count and "Dragonborne" does not.
    //
    // A plot that turns on the player cannot run: the simulation has
    // nobody to point it at, and the last one that tried resolved "the
    // Dragonborn" to a Dremora Butler because that bio happened to say
    // the word. Both prompts now forbid it; this refuses the answer that
    // says it anyway.
    [[nodiscard]] bool NamesThePlayer(std::string_view text);

    // Read the `agent` object and the optional `target` string off one
    // step of a plan, into `step`.
    //
    // `where` names the step for the rejection message ("step 2",
    // "revised step 0"), which is the only thing the two callers phrase
    // differently.
    //
    // Both strings are sanitized here, at the point of extraction.
    // Returns false with `rejection` set when the agent is missing or
    // half-filled: a query with no label cannot degrade when nothing
    // matches, and a label with no query can never match anything.
    [[nodiscard]] bool ReadRoles(const nlohmann::json& raw,
                                 const RoleLimits& limits,
                                 const std::string& where,
                                 PlotModel::Step& step,
                                 std::string& rejection);
} // namespace NarrativeEngine::PlotStepParse
