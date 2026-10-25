#pragma once

#include <PlotModel.h>
#include <PlotRecipes.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// PlotVerbs — the closed set of things a plot step can be.
//
// A step is one of these verbs with its slots filled. Each verb fixes the
// people, places and things it needs and the rule each is admitted by, who
// would know it happened, which of the nine resolution profiles sizes and
// rolls it, and which memories it writes. The words of those memories, and
// of the dashboard's line for it, are in the language files (see PlotText).
// Nothing about a step is written by a model: the model only ever chooses a
// verb from this list and a filler for each slot from a list of candidates.
//
// See docs/implementation/PHASE_17_MILESTONE_2_RIGID_STEPS.md, whose
// template blocks this table and english.ini's [Plot.*] sections were
// generated from. Since Phase 18 Step 5 the shipped language files, not that
// document, are where a memory's words are changed.
namespace NarrativeEngine::PlotVerbs
{
    enum class SlotKind : std::uint8_t
    {
        Person,
        Place,
        Thing
    };

    // Who else knows a step happened. Person slots only; every other slot
    // carries Unset, and the suite asserts both halves.
    //
    // A timeout reaches nobody, so it never triggers any of these.
    enum class Awareness : std::uint8_t
    {
        Unset,
        Never,
        Failure, // only when the step fails: a watch's mark, if they spotted the watcher
        Contact, // whenever the attempt reached them, on success or failure
        Success  // only when the step succeeds: the accused, once an authority acts on it
    };

    // What a slot's candidates are admitted by. Declared with the verbs so
    // the table says everything about a step in one place; PlotCandidates
    // turns each into a list.
    enum class Rule : std::uint8_t
    {
        AnyPerson,                  // anybody the plot population admits
        KnownToMastermind,          // a tie, a shared organisation, or the same hold
        NotHostileKnown,            // known to the mastermind, and not their enemy
        TradesForLiving,            // somebody who trades or works for a living
        RosteredStanding,           // standing in a rostered faction
        AuthorityOverMark,          // the mark's (else the mastermind's) superiors, else the hold's court
        AwareOfEarlierStep,         // somebody an earlier step reached, else known to the mastermind
        MarkCommunity,              // a settlement where the mark, or someone close to them, lives
        MarkOrMastermindSettlement, // the mark's settlement or the mastermind's
        ItemPool,                   // the curated Acquire pool
        DocumentPool,               // the pool's documents alone
        BoundItem,                  // whatever an earlier step of this plot obtained
        PoolOrMade,                 // the pool, and what the mastermind's craftsmen can make
        // What a craftsman the mastermind can send is able to make, one rule
        // per craft. See PlotRecipes.
        SmithRecipe,
        ConcoctRecipe,
        EnchantRecipe,
        SetRecipe,
        FletchRecipe
    };

    // The craft a rule lists the recipes of, or none.
    [[nodiscard]] std::optional<PlotRecipes::Craft> CraftOf(Rule rule) noexcept;

    // What the person in a slot remembers of a step that reached them, in
    // this order. First person and past tense, like the verb's own
    // templates; each is carried only where the slot's awareness can fire
    // for it, and empty otherwise.
    enum class TargetTemplate : std::uint8_t
    {
        Failed,    // it went wrong in front of them: they caught the agent, or turned them away
        Succeeded, // it was done to them openly
        Noticed,   // it was done to them quietly, and left something they would notice -- but not who

        Count
    };

    inline constexpr std::size_t kTargetTemplateCount = static_cast<std::size_t>(TargetTemplate::Count);

    struct Slot
    {
        std::string_view name;
        SlotKind kind = SlotKind::Person;
        Rule rule = Rule::AnyPerson;
        Awareness awareness = Awareness::Unset;
        // What the slot is for, in a few words -- "whom it is shown to" -- is
        // the prompts' to say, in components/narrative_engine/plot_role.prompt.
        // Which TargetTemplate memories its person keeps. Per slot rather
        // than per verb, because two people a step reaches -- a
        // denunciation's accused and the authority -- remember the same act
        // differently. The words are in the language files'
        // [Plot.Remembered] section; see PlotText.
        std::array<bool, kTargetTemplateCount> remembers{};

        [[nodiscard]] constexpr bool Remembers(TargetTemplate t) const noexcept
        {
            return remembers[static_cast<std::size_t>(t)];
        }
    };

    // The twelve templates every verb has, in this order. The first is the
    // dashboard's present-tense line; the next nine are first-person, past
    // tense memories, the last of them added by Milestone 3; the final two
    // are the work alone, for the plot each person in it is told of.
    //
    // The line and the memories are in the language files and rendered by
    // PlotText; the prompts word the line themselves, in
    // components/narrative_engine/plot_step.prompt. Verb::Text holds only
    // the two task phrases, which plot awareness still reads.
    enum class Template : std::uint8_t
    {
        Dashboard,
        AgentDispatched,
        MastermindDispatched,
        SelfDispatched,
        AgentSucceeded,
        AgentFailed,
        AgentTimedOut,
        MastermindToldFailed,
        MastermindToldTimedOut,
        MastermindToldSucceeded,
        // The work itself, as a phrase rather than a sentence: "steal {item}
        // from {mark}", to follow "You sent Aeta to" or "You need to". The
        // mastermind's says "your side" and the agent's "their side", because
        // the two are told it from either end. See PlotAwareness.
        MastermindTask,
        AgentTask,

        Count
    };

    inline constexpr std::size_t kTemplateCount = static_cast<std::size_t>(Template::Count);
    inline constexpr std::size_t kMaxSlots = 3;

    struct Verb
    {
        // Stable snake_case id. What the co-save records and what a
        // decision answers with.
        std::string_view id;
        // The caption under the step's node on the chain.
        std::string_view caption;
        // Which of the nine resolution rows sizes, rolls and gates it.
        PlotModel::StepType profile = PlotModel::StepType::Locate;
        // In the order they are filled: a slot whose candidates depend on
        // another comes after it.
        std::array<Slot, kMaxSlots> slots{};
        std::size_t slotCount = 0;
        // The agent is whoever obtained the bound item, not a fresh cast:
        // the item is in their hands.
        bool agentBoundToItem = false;
        // A later BoundItem slot can bind to what this verb obtains.
        bool obtainsItem = false;
        // What plot awareness tells each person in a plot of their part.
        std::string_view mastermindTask;
        std::string_view agentTask;
        // Whether a scheme can end in it. A verb that only makes, finds or
        // hides something cannot: what it got is for a later step, and a
        // forgery nobody is shown, or a ledger nobody reads, finishes
        // nothing.
        bool finishes = true;

        // MastermindTask or AgentTask; empty for the line or a memory, whose
        // words are not in C++.
        [[nodiscard]] constexpr std::string_view Text(Template t) const noexcept
        {
            switch (t) {
            case Template::MastermindTask:
                return mastermindTask;
            case Template::AgentTask:
                return agentTask;
            default:
                return {};
            }
        }
    };

    inline constexpr std::array<Verb, 24> kVerbs{{
        Verb{
            "watch",
            "Watch",
            PlotModel::StepType::Surveil,
            {{Slot{"mark", SlotKind::Person, Rule::KnownToMastermind, Awareness::Failure, {{true, false, false}}}}},
            1,
            false,
            false,
            "keep watch on {mark} and learn their habits",
            "keep watch on {mark} and learn their habits",
        },
        Verb{
            "ask_around",
            "Ask Around",
            PlotModel::StepType::Locate,
            {{Slot{"mark", SlotKind::Person, Rule::KnownToMastermind, Awareness::Failure, {{true, false, false}}},
              Slot{"place", SlotKind::Place, Rule::MarkCommunity, Awareness::Unset}}},
            2,
            false,
            false,
            "ask around {place} about {mark}",
            "go around {place} asking questions about {mark}",
        },
        Verb{
            "steal",
            "Steal",
            PlotModel::StepType::Acquire,
            {{Slot{"item", SlotKind::Thing, Rule::ItemPool, Awareness::Unset},
              Slot{"mark", SlotKind::Person, Rule::KnownToMastermind, Awareness::Failure, {{true, false, true}}}}},
            2,
            false,
            true,
            "steal {item} from {mark}",
            "steal {item} from {mark}",
        },
        Verb{
            "buy",
            "Buy",
            PlotModel::StepType::Acquire,
            {{Slot{"item", SlotKind::Thing, Rule::ItemPool, Awareness::Unset},
              Slot{"seller", SlotKind::Person, Rule::TradesForLiving, Awareness::Contact, {{true, true, false}}}}},
            2,
            false,
            true,
            "buy {item} from {seller}",
            "buy {item} from {seller}",
        },
        Verb{
            "deliver",
            "Deliver",
            PlotModel::StepType::Deliver,
            {{Slot{"item", SlotKind::Thing, Rule::BoundItem, Awareness::Unset},
              Slot{"recipient", SlotKind::Person, Rule::AnyPerson, Awareness::Contact, {{true, true, false}}}}},
            2,
            true,
            false,
            "bring {item} to {recipient}",
            "bring {item} to {recipient}",
        },
        Verb{
            "plant",
            "Plant",
            PlotModel::StepType::Conceal,
            {{Slot{"item", SlotKind::Thing, Rule::BoundItem, Awareness::Unset},
              Slot{"mark", SlotKind::Person, Rule::AnyPerson, Awareness::Failure, {{true, false, false}}}}},
            2,
            true,
            false,
            "hide {item} among {mark}'s belongings",
            "hide {item} among {mark}'s belongings",
            false,
        },
        Verb{
            "bribe",
            "Bribe",
            PlotModel::StepType::Recruit,
            {{Slot{"mark", SlotKind::Person, Rule::RosteredStanding, Awareness::Contact, {{true, true, false}}}}},
            1,
            false,
            false,
            "pay {mark} to look the other way",
            "pay {mark} to look the other way",
        },
        Verb{
            "win_over",
            "Win Over",
            PlotModel::StepType::Recruit,
            {{Slot{"mark", SlotKind::Person, Rule::NotHostileKnown, Awareness::Contact, {{true, true, false}}}}},
            1,
            false,
            false,
            "win {mark} over to your side",
            "win {mark} over to their side",
        },
        Verb{
            "threaten",
            "Threaten",
            PlotModel::StepType::Recruit,
            {{Slot{"mark", SlotKind::Person, Rule::KnownToMastermind, Awareness::Contact, {{true, true, false}}}}},
            1,
            false,
            false,
            "warn {mark} to stay out of your affairs",
            "warn {mark} to stay out of their affairs",
        },
        Verb{
            "spread_rumors",
            "Spread Rumors",
            PlotModel::StepType::Discredit,
            {{Slot{"mark", SlotKind::Person, Rule::AnyPerson, Awareness::Failure, {{true, false, true}}},
              Slot{"place", SlotKind::Place, Rule::MarkOrMastermindSettlement, Awareness::Unset}}},
            2,
            false,
            false,
            "spread ugly talk about {mark} around {place}",
            "spread ugly talk about {mark} around {place}",
        },
        Verb{
            "denounce",
            "Denounce",
            PlotModel::StepType::Discredit,
            {{Slot{"mark", SlotKind::Person, Rule::AnyPerson, Awareness::Success, {{false, true, false}}},
              Slot{"authority", SlotKind::Person, Rule::AuthorityOverMark, Awareness::Contact, {{true, true, false}}}}},
            2,
            false,
            false,
            "denounce {mark} to {authority}",
            "denounce {mark} to {authority}",
        },
        Verb{
            "sabotage",
            "Sabotage",
            PlotModel::StepType::Sabotage,
            {{Slot{"mark", SlotKind::Person, Rule::TradesForLiving, Awareness::Failure, {{true, false, true}}}}},
            1,
            false,
            false,
            "sabotage {mark}'s livelihood",
            "sabotage {mark}'s livelihood",
        },
        Verb{
            "silence",
            "Silence",
            PlotModel::StepType::Conceal,
            {{Slot{"witness", SlotKind::Person, Rule::AwareOfEarlierStep, Awareness::Contact, {{true, true, false}}}}},
            1,
            false,
            false,
            "pay {witness} to keep quiet",
            "pay {witness} to keep quiet",
        },
        // Falsify, not forge: a model reads "forge" as smithing often enough
        // to plan a plot around a sword.
        Verb{
            "falsify",
            "Falsify",
            PlotModel::StepType::Acquire,
            {{Slot{"item", SlotKind::Thing, Rule::DocumentPool, Awareness::Unset},
              Slot{"subject", SlotKind::Person, Rule::AnyPerson, Awareness::Failure, {{true, false, false}}}}},
            2,
            false,
            true,
            "falsify {item} in {subject}'s name",
            "falsify {item} in {subject}'s name",
            false,
        },
        Verb{
            "dig_up",
            "Dig Up",
            PlotModel::StepType::Locate,
            {{Slot{"item", SlotKind::Thing, Rule::DocumentPool, Awareness::Unset},
              Slot{"mark", SlotKind::Person, Rule::KnownToMastermind, Awareness::Failure, {{true, false, false}}}}},
            2,
            false,
            true,
            "dig up {item} proving {mark}'s wrongdoing",
            "dig up {item} proving {mark}'s wrongdoing",
            false,
        },
        Verb{
            "blackmail",
            "Blackmail",
            PlotModel::StepType::Recruit,
            {{Slot{"mark", SlotKind::Person, Rule::KnownToMastermind, Awareness::Contact, {{true, true, false}}}}},
            1,
            false,
            false,
            "blackmail {mark} into serving you",
            "blackmail {mark} into serving them",
        },
        Verb{
            "destroy",
            "Destroy",
            PlotModel::StepType::Sabotage,
            {{Slot{"item", SlotKind::Thing, Rule::ItemPool, Awareness::Unset},
              Slot{"mark", SlotKind::Person, Rule::KnownToMastermind, Awareness::Failure, {{true, false, true}}}}},
            2,
            false,
            false,
            "destroy {item} belonging to {mark}",
            "destroy {item} belonging to {mark}",
        },
        Verb{
            "petition",
            "Petition",
            PlotModel::StepType::Recruit,
            {{Slot{"authority", SlotKind::Person, Rule::AuthorityOverMark, Awareness::Contact, {{true, true, false}}}}},
            1,
            false,
            false,
            "petition {authority} on your behalf",
            "petition {authority} on their behalf",
        },
        Verb{
            "expose",
            "Expose",
            PlotModel::StepType::Discredit,
            {{Slot{"item", SlotKind::Thing, Rule::BoundItem, Awareness::Unset},
              Slot{"mark", SlotKind::Person, Rule::AnyPerson, Awareness::Success, {{false, true, false}}},
              Slot{"authority", SlotKind::Person, Rule::AuthorityOverMark, Awareness::Contact, {{true, true, false}}}}},
            3,
            true,
            false,
            "show {item} to {authority} as proof against {mark}",
            "show {item} to {authority} as proof against {mark}",
        },
        Verb{
            "smith",
            "Smith",
            PlotModel::StepType::Craft,
            {{Slot{"item", SlotKind::Thing, Rule::SmithRecipe, Awareness::Unset}}},
            1,
            false,
            true,
            "smith {item}",
            "smith {item} for them",
            false,
        },
        Verb{
            "concoct",
            "Concoct",
            PlotModel::StepType::Craft,
            {{Slot{"item", SlotKind::Thing, Rule::ConcoctRecipe, Awareness::Unset}}},
            1,
            false,
            true,
            "concoct {item}",
            "concoct {item} for them",
            false,
        },
        Verb{
            "enchant",
            "Enchant",
            PlotModel::StepType::Craft,
            {{Slot{"item", SlotKind::Thing, Rule::EnchantRecipe, Awareness::Unset}}},
            1,
            false,
            true,
            "make {item} at an arcane enchanter",
            "make {item} for them at an arcane enchanter",
            false,
        },
        Verb{
            "set",
            "Set",
            PlotModel::StepType::Craft,
            {{Slot{"item", SlotKind::Thing, Rule::SetRecipe, Awareness::Unset}}},
            1,
            false,
            true,
            "make {item}",
            "make {item} for them",
            false,
        },
        Verb{
            "fletch",
            "Fletch",
            PlotModel::StepType::Craft,
            {{Slot{"item", SlotKind::Thing, Rule::FletchRecipe, Awareness::Unset}}},
            1,
            false,
            true,
            "fletch {item}",
            "fletch {item} for them",
            false,
        },
    }};

    // One verb that may be done just before another, to make it work: the
    // watch before a theft, the planted evidence before a denunciation.
    //
    // `slot` of the preparation is bound to `from` of the step it prepares,
    // so the watch watches the very person about to be robbed and nobody has
    // to choose them. Both empty means nothing is bound: the bribe before a
    // theft pays whoever planning finds, never one of the theft's own people.
    //
    // Getting an item is not here. A verb with a bound item always has a
    // steal or buy of it planned in front of it; that is not a preparation
    // the model may leave out.
    struct Preparation
    {
        std::string_view prepares;
        std::string_view verb;
        std::string_view slot;
        std::string_view from;
    };

    inline constexpr std::array<Preparation, 40> kPreparations{{
        {"watch", "ask_around", "mark", "mark"},

        {"steal", "watch", "mark", "mark"},
        {"steal", "ask_around", "mark", "mark"},
        {"steal", "bribe", {}, {}},

        {"buy", "ask_around", "mark", "seller"},

        {"plant", "watch", "mark", "mark"},
        {"plant", "bribe", {}, {}},

        {"bribe", "watch", "mark", "mark"},
        {"bribe", "ask_around", "mark", "mark"},

        {"win_over", "ask_around", "mark", "mark"},

        {"threaten", "watch", "mark", "mark"},
        {"threaten", "ask_around", "mark", "mark"},

        {"spread_rumors", "watch", "mark", "mark"},
        {"spread_rumors", "ask_around", "mark", "mark"},
        {"spread_rumors", "plant", "mark", "mark"},

        {"denounce", "plant", "mark", "mark"},
        {"denounce", "steal", "mark", "mark"},
        {"denounce", "watch", "mark", "mark"},
        {"denounce", "spread_rumors", "mark", "mark"},
        {"denounce", "win_over", "mark", "authority"},
        {"denounce", "bribe", "mark", "authority"},
        {"denounce", "dig_up", "mark", "mark"},

        {"sabotage", "watch", "mark", "mark"},
        {"sabotage", "ask_around", "mark", "mark"},
        {"sabotage", "bribe", {}, {}},

        {"silence", "ask_around", "mark", "witness"},

        {"falsify", "ask_around", "mark", "subject"},

        {"dig_up", "watch", "mark", "mark"},
        {"dig_up", "ask_around", "mark", "mark"},

        {"blackmail", "dig_up", "mark", "mark"},
        {"blackmail", "steal", "mark", "mark"},
        {"blackmail", "watch", "mark", "mark"},

        {"destroy", "watch", "mark", "mark"},
        {"destroy", "bribe", {}, {}},

        {"petition", "win_over", "mark", "authority"},
        {"petition", "bribe", "mark", "authority"},

        {"expose", "win_over", "mark", "authority"},
        {"expose", "bribe", "mark", "authority"},
        {"expose", "watch", "mark", "mark"},
        {"expose", "ask_around", "mark", "mark"},
    }};

    // Every preparation of a verb, in table order.
    [[nodiscard]] std::vector<const Preparation*> PreparationsOf(std::string_view verb);

    // The verb with this id, or nullptr.
    [[nodiscard]] const Verb* Find(std::string_view id) noexcept;

    // The craft a verb makes things by, or none: the craft of its recipe
    // slot. A crafting verb's agent must be able to make the thing.
    [[nodiscard]] std::optional<PlotRecipes::Craft> CraftOf(const Verb& verb) noexcept;

    // The snake_case name of a template, for the log and the suite.
    [[nodiscard]] std::string_view TemplateId(Template t) noexcept;

    // Every `{name}` a template names, in order of appearance, repeats kept.
    [[nodiscard]] std::vector<std::string_view> PlaceholdersIn(std::string_view text);

    // A scheme as a verb phrase, so "as part of my plan to {scheme}" reads:
    // first letter lowercased, trailing punctuation and whitespace dropped.
    // "Spread a rumor that Hulda drinks." becomes "spread a rumor that Hulda
    // drinks".
    [[nodiscard]] std::string SchemePhrase(std::string_view scheme);

    // Fill a template. `lookup` answers each placeholder name; one it has no
    // answer for is rendered by `missing`, which is how an agent not yet cast
    // reads as "someone" on the dashboard rather than as a raw `{agent}`.
    using Lookup = std::function<std::optional<std::string>(std::string_view name)>;
    [[nodiscard]] std::string Render(std::string_view text,
                                     const Lookup& lookup,
                                     const std::function<std::string(std::string_view name)>& missing);

    // A verb's id as plain words: its caption, lowercased. `spread_rumors`
    // is "spread rumors" and `win_over` is "win over".
    [[nodiscard]] std::string Words(const Verb& verb);

    // `text` with every verb id that stands in it as a whole word written as
    // that verb's Words. A model shown the verbs by id sometimes writes one
    // into prose -- "spread_rumors about the court historian" -- and the
    // prose is otherwise right, so it is mended rather than refused.
    [[nodiscard]] std::string Plain(std::string_view text);

    // What an unfilled slot of each kind reads as.
    [[nodiscard]] std::string_view Unfilled(SlotKind kind) noexcept;

    // The slot of this name in a verb, or nullptr. `agent`, `mastermind` and
    // `scheme` are not slots: every verb has them implicitly.
    [[nodiscard]] const Slot* FindSlot(const Verb& verb, std::string_view name) noexcept;

    // One of a step's task phrases -- MastermindTask or AgentTask -- filled
    // from what the step holds: `{agent}` from its actor and every other name
    // from its slots, anything not yet filled reading as Unfilled of its
    // kind. Not capitalised, to go mid-sentence: "steal a ruby from Hulda".
    // Empty for a step with no verb, and for any other template.
    [[nodiscard]] std::string RenderPhrase(const PlotModel::Step& step, Template t);

    // The caption under a step's node: its verb's, the model's own for a
    // prose step that wrote one, or the resolution profile's verb.
    [[nodiscard]] std::string Caption(const PlotModel::Step& step);

} // namespace NarrativeEngine::PlotVerbs
