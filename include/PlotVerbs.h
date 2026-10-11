#pragma once

#include <PlotModel.h>

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
// sentence the dashboard shows, the people, places and things it needs and
// the rule each is admitted by, who would know it happened, which of the
// eight resolution profiles sizes and rolls it, and every memory it writes.
// Nothing about a step is written by a model: the model only ever chooses a
// verb from this list and a filler for each slot from a list of candidates.
//
// See docs/implementation/PHASE_17_MILESTONE_2_RIGID_STEPS.md, whose
// template blocks this table is generated from and must stay in step with.
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
        Contact  // whenever the attempt reached them, on success or failure
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
        SeniorInCourt,              // leads, or is senior in, a court
        AwareOfEarlierStep,         // somebody an earlier step reached, else known to the mastermind
        MarkCommunity,              // a settlement where the mark, or someone close to them, lives
        MarkOrMastermindSettlement, // the mark's settlement or the mastermind's
        ItemPool,                   // the curated Acquire pool
        BoundItem                   // whatever an earlier step of this plot obtained
    };

    struct Slot
    {
        std::string_view name;
        SlotKind kind = SlotKind::Person;
        Rule rule = Rule::AnyPerson;
        Awareness awareness = Awareness::Unset;
    };

    // The nine sentences every verb carries, in this order. The first is
    // the dashboard's present-tense line; the rest are first-person, past
    // tense memories.
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
        // Which of the eight resolution rows sizes, rolls and gates it.
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
        std::array<std::string_view, kTemplateCount> templates{};

        [[nodiscard]] constexpr std::string_view Text(Template t) const noexcept
        {
            return templates[static_cast<std::size_t>(t)];
        }
    };

    // Written to the mastermind when a plot ends unfinished with them alive.
    // One sentence for every verb: it follows a failure memory that already
    // says what went wrong.
    inline constexpr std::string_view kConcededTemplate = "I gave up on my plan to {scheme}.";

    inline constexpr std::array<Verb, 13> kVerbs{{
        Verb{
            "watch",
            "Watch",
            PlotModel::StepType::Surveil,
            {{Slot{"mark", SlotKind::Person, Rule::KnownToMastermind, Awareness::Failure}}},
            1,
            false,
            false,
            {{
                "{agent} watches {mark}, learning their habits.",
                "{mastermind} asked me to keep watch on {mark} and learn their habits, so I set off to do it.",
                "I sent {agent} to keep watch on {mark} and learn their habits, as part of my plan to {scheme}.",
                "I decided to keep watch on {mark} myself and learn their habits, as part of my plan to {scheme}.",
                "I kept watch on {mark} until I knew their habits.",
                "{mark} spotted me watching them, and I had to break it off.",
                "I kept watch on {mark}, but ran out of time before I learned their habits.",
                "{agent} reported back that {mark} had spotted them watching.",
                "{agent} reported back that they had run out of time before learning {mark}'s habits.",
            }},
        },
        Verb{
            "ask_around",
            "Ask Around",
            PlotModel::StepType::Locate,
            {{Slot{"mark", SlotKind::Person, Rule::KnownToMastermind, Awareness::Failure},
              Slot{"place", SlotKind::Place, Rule::MarkCommunity, Awareness::Unset}}},
            2,
            false,
            false,
            {{
                "{agent} asks around {place} about {mark}.",
                "{mastermind} asked me to go around {place} asking questions about {mark}, so I set off to do it.",
                "I sent {agent} to ask around {place} about {mark}, as part of my plan to {scheme}.",
                "I decided to ask around {place} about {mark} myself, as part of my plan to {scheme}.",
                "I asked around {place} and learned what I needed to know about {mark}.",
                "Word got back to {mark} that I was asking questions about them around {place}, and I had to stop.",
                "I asked around {place} about {mark}, but ran out of time before anyone told me anything useful.",
                "{agent} reported back that {mark} had heard they were asking questions around {place}.",
                "{agent} reported back that they had run out of time before learning anything useful about {mark}.",
            }},
        },
        Verb{
            "steal",
            "Steal",
            PlotModel::StepType::Acquire,
            {{Slot{"item", SlotKind::Thing, Rule::ItemPool, Awareness::Unset},
              Slot{"mark", SlotKind::Person, Rule::KnownToMastermind, Awareness::Failure}}},
            2,
            false,
            true,
            {{
                "{agent} steals {item} from {mark}.",
                "{mastermind} asked me to steal {item} from {mark}, so I set off to do it.",
                "I sent {agent} to steal {item} from {mark}, as part of my plan to {scheme}.",
                "I decided to steal {item} from {mark} myself, as part of my plan to {scheme}.",
                "I stole {item} from {mark}.",
                "{mark} caught me trying to steal {item} from them.",
                "I tried to steal {item} from {mark}, but ran out of time before I could get my hands on it.",
                "{agent} reported back that {mark} had caught them trying to steal {item}.",
                "{agent} reported back that they had run out of time before they could steal {item} from {mark}.",
            }},
        },
        Verb{
            "buy",
            "Buy",
            PlotModel::StepType::Acquire,
            {{Slot{"item", SlotKind::Thing, Rule::ItemPool, Awareness::Unset},
              Slot{"seller", SlotKind::Person, Rule::TradesForLiving, Awareness::Contact}}},
            2,
            false,
            true,
            {{
                "{agent} buys {item} from {seller}.",
                "{mastermind} asked me to buy {item} from {seller}, so I set off to do it.",
                "I sent {agent} to buy {item} from {seller}, as part of my plan to {scheme}.",
                "I decided to buy {item} from {seller} myself, as part of my plan to {scheme}.",
                "I bought {item} from {seller}.",
                "I tried to buy {item} from {seller}, but the deal went sour.",
                "I tried to buy {item} from {seller}, but ran out of time before we could strike a deal.",
                "{agent} reported back that the deal with {seller} for {item} had gone sour.",
                "{agent} reported back that they had run out of time before striking a deal with {seller} for {item}.",
            }},
        },
        Verb{
            "deliver",
            "Deliver",
            PlotModel::StepType::Deliver,
            {{Slot{"item", SlotKind::Thing, Rule::BoundItem, Awareness::Unset},
              Slot{"recipient", SlotKind::Person, Rule::AnyPerson, Awareness::Contact}}},
            2,
            true,
            false,
            {{
                "{agent} brings {item} to {recipient}.",
                "{mastermind} asked me to bring {item} to {recipient}, so I set off to do it.",
                "I sent {agent} to bring {item} to {recipient}, as part of my plan to {scheme}.",
                "I decided to bring {item} to {recipient} myself, as part of my plan to {scheme}.",
                "I brought {item} to {recipient}.",
                "I was stopped on my way to bring {item} to {recipient}.",
                "I ran out of time before I could bring {item} to {recipient}.",
                "{agent} reported back that they had been stopped on the way to bring {item} to {recipient}.",
                "{agent} reported back that they had run out of time before bringing {item} to {recipient}.",
            }},
        },
        Verb{
            "plant",
            "Plant",
            PlotModel::StepType::Conceal,
            {{Slot{"item", SlotKind::Thing, Rule::BoundItem, Awareness::Unset},
              Slot{"mark", SlotKind::Person, Rule::AnyPerson, Awareness::Failure}}},
            2,
            true,
            false,
            {{
                "{agent} hides {item} among {mark}'s belongings.",
                "{mastermind} asked me to hide {item} among {mark}'s belongings, so I set off to do it.",
                "I sent {agent} to hide {item} among {mark}'s belongings, as part of my plan to {scheme}.",
                "I decided to hide {item} among {mark}'s belongings myself, as part of my plan to {scheme}.",
                "I hid {item} among {mark}'s belongings.",
                "{mark} caught me trying to hide {item} among their belongings.",
                "I ran out of time before I could hide {item} among {mark}'s belongings.",
                "{agent} reported back that {mark} had caught them trying to hide {item} among their belongings.",
                "{agent} reported back that they had run out of time before hiding {item} among {mark}'s belongings.",
            }},
        },
        Verb{
            "bribe",
            "Bribe",
            PlotModel::StepType::Recruit,
            {{Slot{"mark", SlotKind::Person, Rule::RosteredStanding, Awareness::Contact}}},
            1,
            false,
            false,
            {{
                "{agent} pays {mark} to look the other way.",
                "{mastermind} asked me to pay {mark} to look the other way, so I set off to do it.",
                "I sent {agent} to pay {mark} to look the other way, as part of my plan to {scheme}.",
                "I decided to pay {mark} to look the other way myself, as part of my plan to {scheme}.",
                "I paid {mark}, and they agreed to look the other way.",
                "I offered {mark} coin to look the other way, but they would not take it.",
                "I ran out of time before I could get {mark} alone to offer them coin.",
                "{agent} reported back that {mark} had refused the bribe.",
                "{agent} reported back that they had run out of time before they could offer {mark} the bribe.",
            }},
        },
        Verb{
            "win_over",
            "Win Over",
            PlotModel::StepType::Recruit,
            {{Slot{"mark", SlotKind::Person, Rule::NotHostileKnown, Awareness::Contact}}},
            1,
            false,
            false,
            {{
                "{agent} wins {mark} over to {mastermind}'s side.",
                "{mastermind} asked me to win {mark} over to their side, so I set off to do it.",
                "I sent {agent} to win {mark} over to my side, as part of my plan to {scheme}.",
                "I decided to win {mark} over to my side myself, as part of my plan to {scheme}.",
                "I won {mark} over.",
                "I tried to win {mark} over, but they would not be swayed.",
                "I ran out of time before I could win {mark} over.",
                "{agent} reported back that {mark} would not be won over.",
                "{agent} reported back that they had run out of time before winning {mark} over.",
            }},
        },
        Verb{
            "threaten",
            "Threaten",
            PlotModel::StepType::Recruit,
            {{Slot{"mark", SlotKind::Person, Rule::KnownToMastermind, Awareness::Contact}}},
            1,
            false,
            false,
            {{
                "{agent} warns {mark} to stay out of {mastermind}'s affairs.",
                "{mastermind} asked me to warn {mark} to stay out of their affairs, so I set off to do it.",
                "I sent {agent} to warn {mark} to stay out of my affairs, as part of my plan to {scheme}.",
                "I decided to warn {mark} to stay out of my affairs myself, as part of my plan to {scheme}.",
                "I warned {mark} off, and they took the warning to heart.",
                "I tried to warn {mark} off, but they would not be frightened.",
                "I ran out of time before I could warn {mark} off.",
                "{agent} reported back that {mark} would not be warned off.",
                "{agent} reported back that they had run out of time before warning {mark} off.",
            }},
        },
        Verb{
            "spread_rumors",
            "Spread Rumors",
            PlotModel::StepType::Discredit,
            {{Slot{"mark", SlotKind::Person, Rule::AnyPerson, Awareness::Failure},
              Slot{"place", SlotKind::Place, Rule::MarkOrMastermindSettlement, Awareness::Unset}}},
            2,
            false,
            false,
            {{
                "{agent} spreads ugly talk about {mark} around {place}.",
                "{mastermind} asked me to spread ugly talk about {mark} around {place}, so I set off to do it.",
                "I sent {agent} to spread ugly talk about {mark} around {place}, as part of my plan to {scheme}.",
                "I decided to spread ugly talk about {mark} around {place} myself, as part of my plan to {scheme}.",
                "I spread ugly talk about {mark} around {place}, and people started repeating it.",
                "I spread ugly talk about {mark} around {place}, but it was traced back to me.",
                "I ran out of time before the talk about {mark} took hold around {place}.",
                "{agent} reported back that the talk about {mark} around {place} had been traced back to them.",
                "{agent} reported back that they had run out of time before the talk about {mark} took hold around {place}.",
            }},
        },
        Verb{
            "denounce",
            "Denounce",
            PlotModel::StepType::Discredit,
            {{Slot{"mark", SlotKind::Person, Rule::AnyPerson, Awareness::Never},
              Slot{"authority", SlotKind::Person, Rule::SeniorInCourt, Awareness::Contact}}},
            2,
            false,
            false,
            {{
                "{agent} denounces {mark} to {authority}.",
                "{mastermind} asked me to denounce {mark} to {authority}, so I set off to do it.",
                "I sent {agent} to denounce {mark} to {authority}, as part of my plan to {scheme}.",
                "I decided to denounce {mark} to {authority} myself, as part of my plan to {scheme}.",
                "I denounced {mark} to {authority}, and {authority} listened.",
                "I tried to denounce {mark} to {authority}, but {authority} would not hear it.",
                "I ran out of time before I could get an audience with {authority} to denounce {mark}.",
                "{agent} reported back that {authority} would not hear a word against {mark}.",
                "{agent} reported back that they had run out of time before getting an audience with {authority}.",
            }},
        },
        Verb{
            "sabotage",
            "Sabotage",
            PlotModel::StepType::Sabotage,
            {{Slot{"mark", SlotKind::Person, Rule::TradesForLiving, Awareness::Failure}}},
            1,
            false,
            false,
            {{
                "{agent} sabotages {mark}'s livelihood.",
                "{mastermind} asked me to sabotage {mark}'s livelihood, so I set off to do it.",
                "I sent {agent} to sabotage {mark}'s livelihood, as part of my plan to {scheme}.",
                "I decided to sabotage {mark}'s livelihood myself, as part of my plan to {scheme}.",
                "I sabotaged {mark}'s livelihood.",
                "{mark} caught me trying to sabotage their livelihood.",
                "I ran out of time before I could do {mark}'s livelihood any real harm.",
                "{agent} reported back that {mark} had caught them trying to sabotage their livelihood.",
                "{agent} reported back that they had run out of time before doing {mark}'s livelihood any real harm.",
            }},
        },
        Verb{
            "silence",
            "Silence",
            PlotModel::StepType::Conceal,
            {{Slot{"witness", SlotKind::Person, Rule::AwareOfEarlierStep, Awareness::Contact}}},
            1,
            false,
            false,
            {{
                "{agent} pays {witness} to keep quiet.",
                "{mastermind} asked me to pay {witness} to keep quiet, so I set off to do it.",
                "I sent {agent} to pay {witness} to keep quiet, as part of my plan to {scheme}.",
                "I decided to pay {witness} to keep quiet myself, as part of my plan to {scheme}.",
                "I paid {witness}, and they agreed to keep quiet.",
                "I offered {witness} coin to keep quiet, but they would not take it.",
                "I ran out of time before I could pay {witness} to keep quiet.",
                "{agent} reported back that {witness} would not be paid to keep quiet.",
                "{agent} reported back that they had run out of time before paying {witness} to keep quiet.",
            }},
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

    inline constexpr std::array<Preparation, 24> kPreparations{{
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

        {"denounce", "plant", "mark", "mark"},
        {"denounce", "steal", "mark", "mark"},
        {"denounce", "watch", "mark", "mark"},
        {"denounce", "spread_rumors", "mark", "mark"},
        {"denounce", "win_over", "mark", "authority"},
        {"denounce", "bribe", "mark", "authority"},

        {"sabotage", "watch", "mark", "mark"},
        {"sabotage", "ask_around", "mark", "mark"},
        {"sabotage", "bribe", {}, {}},

        {"silence", "ask_around", "mark", "witness"},
    }};

    // Every preparation of a verb, in table order.
    [[nodiscard]] std::vector<const Preparation*> PreparationsOf(std::string_view verb);

    // The verb with this id, or nullptr.
    [[nodiscard]] const Verb* Find(std::string_view id) noexcept;

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

    // A verb's dashboard sentence with every slot unfilled: "Someone steals
    // something from someone." Says what the verb does without saying to
    // whom, which is how a verb is offered as a kind of step.
    [[nodiscard]] std::string Generic(const Verb& verb);

    // What an unfilled slot of each kind reads as.
    [[nodiscard]] std::string_view Unfilled(SlotKind kind) noexcept;

    // The slot of this name in a verb, or nullptr. `agent`, `mastermind` and
    // `scheme` are not slots: every verb has them implicitly.
    [[nodiscard]] const Slot* FindSlot(const Verb& verb, std::string_view name) noexcept;

    // One of a step's templates, filled from what the step holds: `{agent}`
    // from its actor, `{scheme}` from `scheme` as a verb phrase, and every
    // other name from its slots. Anything not yet filled reads as Unfilled
    // of its kind. Empty for a step with no verb.
    [[nodiscard]] std::string RenderStep(const PlotModel::Step& step, Template t, std::string_view scheme = {});

    // The caption under a step's node: its verb's, the model's own for a
    // prose step that wrote one, or the resolution profile's verb.
    [[nodiscard]] std::string Caption(const PlotModel::Step& step);

    // The concession memory for a plot whose scheme this is.
    [[nodiscard]] std::string RenderConceded(std::string_view scheme);
} // namespace NarrativeEngine::PlotVerbs
