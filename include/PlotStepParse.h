#pragma once

#include <LanguageTable.h>
#include <Localization.h>

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
    // the first Dragonborn and is still called Miraak. The language file's
    // [Matching] PlayerTitle check says the title in each language, so in
    // English "dragonborn" and "Dragonborn's" both count and "Dragonborne"
    // does not.
    //
    // And by name: any word of `playerName` three characters or longer,
    // whole and with its case, or carrying one of [Matching] Name.Suffixes,
    // so in English "Varian" and "Varian's" count and "variance" does not.
    // In a language written without spaces, anywhere in the text. NPCs' memories call the player by their
    // character's name, and a scheme written from those memories named
    // "Varian" -- the player, made Arch-Mage in that save -- with nothing to
    // say Varian was not somebody the simulation could reach.
    //
    // A plot that turns on the player cannot run: the simulation has
    // nobody to point it at. The ambition prompt forbids it; this refuses
    // the answer that does it anyway.
    [[nodiscard]] bool NamesThePlayer(std::string_view text,
                                      std::string_view playerName = {},
                                      const LanguageTable::Table& table = Localization::Get());
} // namespace NarrativeEngine::PlotStepParse
