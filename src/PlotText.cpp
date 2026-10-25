#include <PlotText.h>

#include <TextUtf8.h>

#include <array>
#include <deque>
#include <unordered_map>

namespace NarrativeEngine::PlotText
{
    namespace
    {
        using PlotVerbs::SlotKind;
        using PlotVerbs::TargetTemplate;
        using PlotVerbs::Template;
        using TextTemplate::Kind;

        constexpr std::string_view kDashboardSection = "Plot.Dashboard";
        constexpr std::string_view kMemorySection = "Plot.Memory";
        constexpr std::string_view kRememberedSection = "Plot.Remembered";

        // The nine memories every verb writes, in Template order.
        constexpr std::array kMemoryTemplates = {Template::AgentDispatched,
                                                 Template::MastermindDispatched,
                                                 Template::SelfDispatched,
                                                 Template::AgentSucceeded,
                                                 Template::AgentFailed,
                                                 Template::AgentTimedOut,
                                                 Template::MastermindToldFailed,
                                                 Template::MastermindToldTimedOut,
                                                 Template::MastermindToldSucceeded};

        constexpr std::array<std::string_view, PlotVerbs::kTargetTemplateCount> kTargetIds = {"failed",
                                                                                              "succeeded",
                                                                                              "noticed"};

        constexpr std::array kSchemeParams{TextKeys::Param{"scheme", Kind::Text}};
        constexpr std::array kOutcomeKeys{TextKeys::Key{"Plot.Outcome", "succeeded", kSchemeParams},
                                          TextKeys::Key{"Plot.Outcome", "failed", kSchemeParams},
                                          TextKeys::Key{"Plot.Outcome", "conceded", kSchemeParams},
                                          TextKeys::Key{"Plot.Outcome", "went_on", kSchemeParams}};

        // What a slot reads as before it is filled.
        constexpr TextKeys::Key kUnfilledPerson{"Plot.Unfilled", "person", {}};
        constexpr TextKeys::Key kUnfilledPlace{"Plot.Unfilled", "place", {}};
        constexpr TextKeys::Key kUnfilledThing{"Plot.Unfilled", "thing", {}};
        // A plot with no scheme, which reads least badly as this.
        constexpr TextKeys::Key kUnfilledScheme{"Plot.Unfilled", "scheme", {}};

        constexpr std::array kItemMadeParams{TextKeys::Param{"name", Kind::Text},
                                             TextKeys::Param{"article", Kind::Text}};
        constexpr std::array kItemAmmoParams{TextKeys::Param{"name", Kind::Text}};
        constexpr TextKeys::Key kItemMade{"Plot.Item", "made", kItemMadeParams};
        constexpr TextKeys::Key kItemAmmo{"Plot.Item", "ammo", kItemAmmoParams};

        constexpr std::array kTestKeys{TextKeys::Key{"Plot.Test", "brelyna.ambition", {}},
                                       TextKeys::Key{"Plot.Test", "brelyna.scheme", {}},
                                       TextKeys::Key{"Plot.Test", "brelyna.finish", {}},
                                       TextKeys::Key{"Plot.Test", "tolfdir.ambition", {}},
                                       TextKeys::Key{"Plot.Test", "tolfdir.scheme", {}},
                                       TextKeys::Key{"Plot.Test", "tolfdir.finish", {}},
                                       TextKeys::Key{"Plot.Test", "college", {}},
                                       TextKeys::Key{"Plot.Test", "journal", {}},
                                       TextKeys::Key{"Plot.Test", "debug.mastermind", {}},
                                       TextKeys::Key{"Plot.Test", "debug.ambition", {}},
                                       TextKeys::Key{"Plot.Test", "debug.scheme", {}},
                                       TextKeys::Key{"Plot.Test", "debug.step.locate", {}},
                                       TextKeys::Key{"Plot.Test", "debug.step.surveil", {}},
                                       TextKeys::Key{"Plot.Test", "debug.step.acquire", {}}};

        const TextKeys::Key kNoKey{"", "", {}};

        // Keys generated from the verb table. The deques keep every name and
        // parameter list at a fixed address, because a Key only views them.
        struct Storage
        {
            std::deque<std::string> names;
            std::deque<std::vector<TextKeys::Param>> params;
            std::vector<TextKeys::Key> keys;
            std::unordered_map<std::string, std::size_t> index;

            void Add(std::string_view section, std::string name, std::span<const TextKeys::Param> with)
            {
                const auto& stored = names.emplace_back(std::move(name));
                index.emplace(std::string{section} + "/" + stored, keys.size());
                keys.push_back({section, stored, with});
            }

            [[nodiscard]] const TextKeys::Key& Find(std::string_view section, std::string_view name) const
            {
                const auto it = index.find(std::string{section} + "/" + std::string{name});
                return it != index.end() ? keys[it->second] : kNoKey;
            }
        };

        Storage Build()
        {
            Storage s;
            for (const auto& verb : PlotVerbs::kVerbs) {
                // Every template of a verb may name its agent, its mastermind,
                // the scheme and its own slots, and nothing else.
                auto& params = s.params.emplace_back();
                params.push_back({"agent", Kind::Person});
                params.push_back({"mastermind", Kind::Person});
                params.push_back({"scheme", Kind::Text});
                for (std::size_t i = 0; i < verb.slotCount; ++i) {
                    params.push_back(
                        {verb.slots[i].name, verb.slots[i].kind == SlotKind::Person ? Kind::Person : Kind::Text});
                }
                const std::string id{verb.id};
                s.Add(kDashboardSection, id + ".line", params);
                s.Add(kDashboardSection, id + ".caption", {});
                for (const auto t : kMemoryTemplates) {
                    s.Add(kMemorySection, id + "." + std::string{PlotVerbs::TemplateId(t)}, params);
                }
                for (std::size_t i = 0; i < verb.slotCount; ++i) {
                    const auto& slot = verb.slots[i];
                    for (std::size_t r = 0; r < PlotVerbs::kTargetTemplateCount; ++r) {
                        if (slot.Remembers(static_cast<TargetTemplate>(r))) {
                            s.Add(kRememberedSection,
                                  id + "." + std::string{slot.name} + "." + std::string{kTargetIds[r]},
                                  params);
                        }
                    }
                }
            }
            for (const auto& key : kOutcomeKeys) {
                s.keys.push_back(key);
            }
            for (const auto& key : kTestKeys) {
                s.keys.push_back(key);
            }
            for (const auto* key :
                 {&kUnfilledPerson, &kUnfilledPlace, &kUnfilledThing, &kUnfilledScheme, &kItemMade, &kItemAmmo}) {
                s.keys.push_back(*key);
            }
            return s;
        }

        const Storage& Generated()
        {
            static const Storage storage = Build();
            return storage;
        }

        const TextKeys::Key& UnfilledKey(SlotKind kind)
        {
            switch (kind) {
            case SlotKind::Person:
                return kUnfilledPerson;
            case SlotKind::Place:
                return kUnfilledPlace;
            case SlotKind::Thing:
                return kUnfilledThing;
            }
            return kUnfilledPerson;
        }

        // What every placeholder of `verb`'s templates is filled with, from
        // `step`. Anything with no value is left out, for `Missing` to name.
        TextTemplate::Arguments ArgumentsFor(const PlotModel::Step& step,
                                             const PlotVerbs::Verb& verb,
                                             std::string_view scheme,
                                             const SexOf& sexOf)
        {
            const auto sex = [&sexOf](RE::FormID npc) {
                return sexOf && npc != 0 ? sexOf(npc) : TextTemplate::Sex::Other;
            };
            TextTemplate::Arguments args;
            if (!step.actorName.empty()) {
                args.Set("agent", TextTemplate::Person{step.actorName, sex(step.actor)});
            }
            if (!scheme.empty()) {
                args.Set("scheme", std::string{scheme});
            }
            for (const auto& value : step.slots) {
                if (value.text.empty()) {
                    continue;
                }
                if (value.name == "mastermind") {
                    args.Set("mastermind", TextTemplate::Person{value.text, sex(value.form)});
                } else if (const auto* slot = PlotVerbs::FindSlot(verb, value.name)) {
                    if (slot->kind == SlotKind::Person) {
                        args.Set(value.name, TextTemplate::Person{value.text, sex(value.form)});
                    } else {
                        args.Set(value.name, value.text);
                    }
                }
            }
            return args;
        }

        // An unfilled slot reads by its kind; the people every verb names
        // read as somebody; a missing scheme reads as "it".
        auto Missing(const LanguageTable::Table& table, const PlotVerbs::Verb* verb)
        {
            return [&table, verb](std::string_view name) {
                if (verb != nullptr) {
                    if (const auto* slot = PlotVerbs::FindSlot(*verb, name)) {
                        return table.Render(UnfilledKey(slot->kind), {});
                    }
                }
                return table.Render(name == "scheme" ? kUnfilledScheme : kUnfilledPerson, {});
            };
        }

        std::string RenderVerbKey(const LanguageTable::Table& table,
                                  const TextKeys::Key& key,
                                  const PlotModel::Step& step,
                                  const PlotVerbs::Verb& verb,
                                  std::string_view scheme,
                                  const SexOf& sexOf)
        {
            // A sentence can open with a slot -- "{agent} watches..." -- and an
            // unfilled one reads as "someone", so the first letter is not the
            // template's to guarantee.
            return table.Render(key, ArgumentsFor(step, verb, scheme, sexOf), true, Missing(table, &verb));
        }
    } // namespace

    const std::vector<TextKeys::Key>& Keys()
    {
        return Generated().keys;
    }

    const TextKeys::Key& MemoryKey(const PlotVerbs::Verb& verb, PlotVerbs::Template t)
    {
        return Generated().Find(kMemorySection, std::string{verb.id} + "." + std::string{PlotVerbs::TemplateId(t)});
    }

    const TextKeys::Key& RememberedKey(const PlotVerbs::Verb& verb,
                                       const PlotVerbs::Slot& slot,
                                       PlotVerbs::TargetTemplate t)
    {
        return Generated().Find(kRememberedSection,
                                std::string{verb.id} + "." + std::string{slot.name} + "."
                                    + std::string{kTargetIds[static_cast<std::size_t>(t)]});
    }

    const TextKeys::Key& LineKey(const PlotVerbs::Verb& verb)
    {
        return Generated().Find(kDashboardSection, std::string{verb.id} + ".line");
    }

    const TextKeys::Key& CaptionKey(const PlotVerbs::Verb& verb)
    {
        return Generated().Find(kDashboardSection, std::string{verb.id} + ".caption");
    }

    const TextKeys::Key& OutcomeKey(Outcome outcome)
    {
        return kOutcomeKeys[static_cast<std::size_t>(outcome)];
    }

    std::string Memory(const LanguageTable::Table& table,
                       const PlotModel::Step& step,
                       PlotVerbs::Template t,
                       std::string_view scheme,
                       const SexOf& sexOf)
    {
        const auto* verb = PlotVerbs::Find(step.verb);
        return verb != nullptr ? RenderVerbKey(table, MemoryKey(*verb, t), step, *verb, scheme, sexOf) : std::string{};
    }

    std::string Remembered(const LanguageTable::Table& table,
                           const PlotModel::Step& step,
                           std::string_view slot,
                           PlotVerbs::TargetTemplate t,
                           const SexOf& sexOf)
    {
        const auto* verb = PlotVerbs::Find(step.verb);
        const auto* found = verb != nullptr ? PlotVerbs::FindSlot(*verb, slot) : nullptr;
        if (found == nullptr || !found->Remembers(t)) {
            return {};
        }
        return RenderVerbKey(table, RememberedKey(*verb, *found, t), step, *verb, {}, sexOf);
    }

    std::vector<std::string> RememberedNames(const LanguageTable::Table& table,
                                             const PlotModel::Step& step,
                                             std::string_view slot,
                                             PlotVerbs::TargetTemplate t)
    {
        const auto* verb = PlotVerbs::Find(step.verb);
        const auto* found = verb != nullptr ? PlotVerbs::FindSlot(*verb, slot) : nullptr;
        if (found == nullptr || !found->Remembers(t)) {
            return {};
        }
        const auto* message = table.Find(RememberedKey(*verb, *found, t));
        return message != nullptr ? TextTemplate::PlaceholderNames(*message) : std::vector<std::string>{};
    }

    std::string PlotOutcome(const LanguageTable::Table& table, Outcome outcome, std::string_view scheme)
    {
        TextTemplate::Arguments args;
        if (!scheme.empty()) {
            args.Set("scheme", std::string{scheme});
        }
        return table.Render(OutcomeKey(outcome), args, false, Missing(table, nullptr));
    }

    std::string DashboardLine(const LanguageTable::Table& table, const PlotModel::Step& step)
    {
        const auto* verb = PlotVerbs::Find(step.verb);
        return verb != nullptr ? RenderVerbKey(table, LineKey(*verb), step, *verb, {}, {}) : PlotModel::Label(step);
    }

    std::string DashboardCaption(const LanguageTable::Table& table, const PlotModel::Step& step)
    {
        const auto* verb = PlotVerbs::Find(step.verb);
        return verb != nullptr ? table.Render(CaptionKey(*verb), {}) : PlotVerbs::Caption(step);
    }

    std::string TestText(const LanguageTable::Table& table, TestLine line)
    {
        return table.Render(kTestKeys[static_cast<std::size_t>(line)], {});
    }

    std::string ItemPhrase(const LanguageTable::Table& table, std::string_view name, bool ammo)
    {
        if (name.empty()) {
            return {};
        }
        TextTemplate::Arguments args;
        args.Set("name", std::string{name});
        if (ammo) {
            return table.Render(kItemAmmo, args);
        }
        const auto first = TextUtf8::DecodeAt(name, 0).codePoint;
        const bool vowel = first == U'a' || first == U'e' || first == U'i' || first == U'o' || first == U'u'
                           || first == U'A' || first == U'E' || first == U'I' || first == U'O' || first == U'U';
        const auto& grammar = table.Grammar();
        args.Set("article", vowel ? grammar.indefiniteArticleBeforeVowel : grammar.indefiniteArticle);
        return table.Render(kItemMade, args);
    }
} // namespace NarrativeEngine::PlotText
