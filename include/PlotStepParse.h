#pragma once

#include <string_view>

// PlotStepParse — the one check every plot response parser makes on the
// free text a model writes: whether it names the player.
//
// Steps are verbs now and are never written by a model, so this is all that
// is left of what composition and adaptation once shared. What is still
// free text -- the ambition and the scheme -- goes through it.
namespace NarrativeEngine::PlotStepParse
{
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
    // the word. The ambition prompt forbids it; this refuses the answer
    // that says it anyway.
    [[nodiscard]] bool NamesThePlayer(std::string_view text);
} // namespace NarrativeEngine::PlotStepParse
