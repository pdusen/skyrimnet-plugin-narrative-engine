#pragma once

#include <string_view>

// PlotStepParse — the one check every plot response parser makes on the
// free text a model writes: whether it names the player.
//
// Steps are verbs now and are never written by a model, so this is all that
// is left of what composition and adaptation once shared. What is still
// free text -- the ambition, the scheme and how it finishes -- goes through
// it.
namespace NarrativeEngine::PlotStepParse
{
    // Does this text name the player?
    //
    // By title: "Dragonborn" is what a model reaches for when it means the
    // player, and it is the one title no NPC in Skyrim wears -- Miraak is
    // the first Dragonborn and is still called Miraak. Whole-word and
    // case-insensitive, so "dragonborn" and "Dragonborn's" both count and
    // "Dragonborne" does not.
    //
    // And by name: any word of `playerName` three letters or longer,
    // whole-word and with its case, so "Varian" and "Varian's" count and
    // "variance" does not. NPCs' memories call the player by their
    // character's name, and a scheme written from those memories named
    // "Varian" -- the player, made Arch-Mage in that save -- with nothing to
    // say Varian was not somebody the simulation could reach.
    //
    // A plot that turns on the player cannot run: the simulation has
    // nobody to point it at. The ambition prompt forbids it; this refuses
    // the answer that does it anyway.
    [[nodiscard]] bool NamesThePlayer(std::string_view text, std::string_view playerName = {});
} // namespace NarrativeEngine::PlotStepParse
