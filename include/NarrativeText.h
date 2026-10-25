#pragma once

#include <LanguageTable.h>
#include <TextKeys.h>
#include <TextTemplate.h>

#include <string>
#include <string_view>
#include <vector>

// The sentences NarrativeEngine writes into SkyrimNet outside of plots: how a
// listener remembers hearing a rumor and a teller telling it, the letter
// writer's memory of sending one, and the narration a visit ends, resumes or
// is walked out of with. Each is read by the player in SkyrimNet's own views,
// and quoted back to them by NPCs.
//
// The words are the language table's ([Gossip.Memory], [Letter.Memory],
// [Visit.Narration]); a person whose name is empty reads as the fallback for
// where they stand in the sentence.
namespace NarrativeEngine::NarrativeText
{
    using TextTemplate::Person;

    // Every key this module renders.
    [[nodiscard]] const std::vector<TextKeys::Key>& Keys();

    // How a listener came to hear a rumor, from what ties them to the teller.
    enum class Heard
    {
        KinFromAway, // family, from another hold: news brought home
        Kin,         // family, in the same hold
        Household,   // they share a roof
        Settlement,  // they live in the same town
        Rumor,       // nothing closer: talk going round
    };

    // `kin` is the game's own word for the teller ("sister"), `hold` the
    // teller's hold for KinFromAway, `band` the rumor's words.
    [[nodiscard]] std::string HeardIt(const LanguageTable::Table& table,
                                      Heard how,
                                      const Person& teller,
                                      std::string_view kin,
                                      std::string_view hold,
                                      std::string_view band);

    [[nodiscard]] std::string ToldIt(const LanguageTable::Table& table,
                                     const std::vector<Person>& listeners,
                                     std::string_view band);

    // The letter writer's memory of sending it, followed by the letter.
    [[nodiscard]] std::string LetterSent(const LanguageTable::Table& table,
                                         const Person& recipient,
                                         std::string_view body);

    // The player walked away from a visit mid-conversation.
    [[nodiscard]] std::string WalkedOut(const LanguageTable::Table& table, const Person& player, const Person& visitor);

    // The visitor takes their leave; `ignored` when the player never engaged.
    [[nodiscard]] std::string Valediction(const LanguageTable::Table& table, const Person& visitor, bool ignored);

    // The visitor turns back to the player after an interruption.
    [[nodiscard]] std::string ReEngage(const LanguageTable::Table& table, const Person& visitor, const Person& player);
} // namespace NarrativeEngine::NarrativeText
