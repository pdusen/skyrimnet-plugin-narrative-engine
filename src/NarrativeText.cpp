#include <NarrativeText.h>

#include <array>

namespace NarrativeEngine::NarrativeText
{
    namespace
    {
        using TextKeys::Key;
        using TextKeys::Param;
        using TextTemplate::Kind;

        constexpr std::array kHeardParams{Param{"teller", Kind::Person},
                                          Param{"kin", Kind::Text},
                                          Param{"hold", Kind::Text},
                                          Param{"band", Kind::Text}};
        constexpr std::array kToldParams{Param{"listeners", Kind::List}, Param{"band", Kind::Text}};
        constexpr std::array kLetterParams{Param{"recipient", Kind::Person}, Param{"body", Kind::Text}};
        constexpr std::array kVisitParams{Param{"player", Kind::Person}, Param{"visitor", Kind::Person}};

        constexpr std::array kHeardKeys{Key{"Gossip.Memory", "heard.kin_from_away", kHeardParams},
                                        Key{"Gossip.Memory", "heard.kin", kHeardParams},
                                        Key{"Gossip.Memory", "heard.household", kHeardParams},
                                        Key{"Gossip.Memory", "heard.settlement", kHeardParams},
                                        Key{"Gossip.Memory", "heard.rumor", kHeardParams}};
        constexpr Key kTold{"Gossip.Memory", "told", kToldParams};
        constexpr Key kUnknownPerson{"Gossip.Memory", "unknown.person", {}};
        constexpr Key kUnknownHold{"Gossip.Memory", "unknown.hold", {}};

        constexpr Key kLetterSent{"Letter.Memory", "sent", kLetterParams};
        constexpr Key kUnknownRecipient{"Letter.Memory", "unknown.recipient", {}};

        constexpr Key kWalkedOut{"Visit.Narration", "walked_out", kVisitParams};
        constexpr Key kValediction{"Visit.Narration", "valediction", kVisitParams};
        constexpr Key kValedictionIgnored{"Visit.Narration", "valediction.ignored", kVisitParams};
        constexpr Key kReEngage{"Visit.Narration", "reengage", kVisitParams};
        // Each fallback is for where the unnamed person stands in its
        // sentence: the player opening one is capitalised, and a visitor who
        // walked out is "their visitor" rather than "the sender".
        constexpr Key kUnknownPlayerOpening{"Visit.Narration", "unknown.player.opening", {}};
        constexpr Key kUnknownPlayer{"Visit.Narration", "unknown.player", {}};
        constexpr Key kUnknownVisitor{"Visit.Narration", "unknown.visitor", {}};
        constexpr Key kUnknownSender{"Visit.Narration", "unknown.sender", {}};

        // `person`, or, unnamed, a person called by `fallback`'s text.
        Person Named(const LanguageTable::Table& table, const Person& person, const Key& fallback)
        {
            return person.name.empty() ? Person{table.Render(fallback, {}), person.sex} : person;
        }
    } // namespace

    const std::vector<TextKeys::Key>& Keys()
    {
        static const std::vector<Key> keys = [] {
            std::vector<Key> out(kHeardKeys.begin(), kHeardKeys.end());
            for (const auto* key : {&kTold,
                                    &kUnknownPerson,
                                    &kUnknownHold,
                                    &kLetterSent,
                                    &kUnknownRecipient,
                                    &kWalkedOut,
                                    &kValediction,
                                    &kValedictionIgnored,
                                    &kReEngage,
                                    &kUnknownPlayerOpening,
                                    &kUnknownPlayer,
                                    &kUnknownVisitor,
                                    &kUnknownSender}) {
                out.push_back(*key);
            }
            return out;
        }();
        return keys;
    }

    std::string HeardIt(const LanguageTable::Table& table,
                        Heard how,
                        const Person& teller,
                        std::string_view kin,
                        std::string_view hold,
                        std::string_view band)
    {
        TextTemplate::Arguments args;
        args.Set("teller", Named(table, teller, kUnknownPerson))
            .Set("kin", std::string{kin})
            .Set("hold", hold.empty() ? table.Render(kUnknownHold, {}) : std::string{hold})
            .Set("band", std::string{band});
        return table.Render(kHeardKeys[static_cast<std::size_t>(how)], args);
    }

    std::string ToldIt(const LanguageTable::Table& table, const std::vector<Person>& listeners, std::string_view band)
    {
        TextTemplate::List names;
        names.reserve(listeners.size());
        for (const auto& listener : listeners) {
            names.push_back(Named(table, listener, kUnknownPerson).name);
        }
        TextTemplate::Arguments args;
        args.Set("listeners", std::move(names)).Set("band", std::string{band});
        return table.Render(kTold, args);
    }

    std::string LetterSent(const LanguageTable::Table& table, const Person& recipient, std::string_view body)
    {
        TextTemplate::Arguments args;
        args.Set("recipient", Named(table, recipient, kUnknownRecipient)).Set("body", std::string{body});
        return table.Render(kLetterSent, args);
    }

    std::string WalkedOut(const LanguageTable::Table& table, const Person& player, const Person& visitor)
    {
        TextTemplate::Arguments args;
        args.Set("player", Named(table, player, kUnknownPlayerOpening))
            .Set("visitor", Named(table, visitor, kUnknownVisitor));
        return table.Render(kWalkedOut, args);
    }

    std::string Valediction(const LanguageTable::Table& table, const Person& visitor, bool ignored)
    {
        TextTemplate::Arguments args;
        args.Set("visitor", Named(table, visitor, kUnknownSender));
        return table.Render(ignored ? kValedictionIgnored : kValediction, args);
    }

    std::string ReEngage(const LanguageTable::Table& table, const Person& visitor, const Person& player)
    {
        TextTemplate::Arguments args;
        args.Set("visitor", Named(table, visitor, kUnknownSender)).Set("player", Named(table, player, kUnknownPlayer));
        return table.Render(kReEngage, args);
    }
} // namespace NarrativeEngine::NarrativeText
