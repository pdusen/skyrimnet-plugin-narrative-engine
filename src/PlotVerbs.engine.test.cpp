#include <PlotVerbs.h>

#include <PlotSerialize.h>
#include <PlotState.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <string_view>

// Tests for the verb manifest: the closed set of things a plot step can be.
//
// The table is data, and these are the rules that make it usable data. Most
// of them guard a mistake that would otherwise only show up as a sentence in
// some NPC's memory reading wrong, days of in-world time after the step that
// wrote it: a template naming a slot its verb does not fill, an agent's memory
// naming the mastermind when the two are the same person, a bound item no verb
// ever obtains.

namespace
{
    namespace PlotVerbs = NarrativeEngine::PlotVerbs;
    namespace PlotModel = NarrativeEngine::PlotModel;
    namespace PlotSerialize = NarrativeEngine::PlotSerialize;
    using PlotVerbs::Template;

    constexpr Template kAll[] = {Template::Dashboard,
                                 Template::AgentDispatched,
                                 Template::MastermindDispatched,
                                 Template::SelfDispatched,
                                 Template::AgentSucceeded,
                                 Template::AgentFailed,
                                 Template::AgentTimedOut,
                                 Template::MastermindToldFailed,
                                 Template::MastermindToldTimedOut,
                                 Template::MastermindToldSucceeded};

    constexpr Template kAgentOutcomes[] = {Template::AgentSucceeded, Template::AgentFailed, Template::AgentTimedOut};

    constexpr Template kDispatch[] = {Template::AgentDispatched,
                                      Template::MastermindDispatched,
                                      Template::SelfDispatched};

    bool Names(std::string_view text, std::string_view name)
    {
        for (const auto found : PlotVerbs::PlaceholdersIn(text)) {
            if (found == name) {
                return true;
            }
        }
        return false;
    }

    bool IsDispatch(Template t)
    {
        for (const auto d : kDispatch) {
            if (d == t) {
                return true;
            }
        }
        return false;
    }

    // A step of `verb` with every slot filled, as planning and dispatch will
    // leave it.
    PlotModel::Step Filled(std::string_view verbId)
    {
        const auto* verb = PlotVerbs::Find(verbId);
        REQUIRE(verb != nullptr);
        PlotModel::Step step;
        step.verb = std::string{verbId};
        step.type = verb->profile;
        step.actor = 0x0001A001;
        step.actorName = "Aeta";
        step.slots.push_back({"mastermind", 0x0001A000, "Fanari Strong-Voice"});
        for (std::size_t i = 0; i < verb->slotCount; ++i) {
            const auto& slot = verb->slots[i];
            const std::string text = slot.kind == PlotVerbs::SlotKind::Person  ? "Hulda"
                                     : slot.kind == PlotVerbs::SlotKind::Place ? "Whiterun"
                                                                               : "a ruby";
            step.slots.push_back({std::string{slot.name}, static_cast<RE::FormID>(0x0002B000 + i), text});
        }
        return step;
    }
} // namespace

TEST_CASE("PlotVerbs manifest", "[PlotVerbs][engine]")
{
    SECTION("when every verb is read")
    {
        SECTION("should carry a distinct id and a caption")
        {
            std::set<std::string_view> ids;
            for (const auto& verb : PlotVerbs::kVerbs) {
                INFO(verb.id);
                REQUIRE_FALSE(verb.id.empty());
                REQUIRE_FALSE(verb.caption.empty());
                REQUIRE(ids.insert(verb.id).second);
                REQUIRE(PlotVerbs::Find(verb.id) == &verb);
            }
        }

        SECTION("should carry all ten templates")
        {
            for (const auto& verb : PlotVerbs::kVerbs) {
                for (const auto t : kAll) {
                    INFO(verb.id << " " << PlotVerbs::TemplateId(t));
                    REQUIRE_FALSE(verb.Text(t).empty());
                }
            }
        }

        SECTION("should name only slots it declares, plus agent, mastermind and scheme")
        {
            for (const auto& verb : PlotVerbs::kVerbs) {
                for (const auto t : kAll) {
                    for (const auto name : PlotVerbs::PlaceholdersIn(verb.Text(t))) {
                        INFO(verb.id << " " << PlotVerbs::TemplateId(t) << " {" << name << "}");
                        const bool implicit = name == "agent" || name == "mastermind" || name == "scheme";
                        REQUIRE((implicit || PlotVerbs::FindSlot(verb, name) != nullptr));
                    }
                }
            }
        }

        SECTION("should never name the mastermind in the agent's own outcome")
        {
            // These are the memories a self-agent keeps, and naming the
            // mastermind there puts the same person on both sides of a
            // sentence.
            for (const auto& verb : PlotVerbs::kVerbs) {
                for (const auto t : kAgentOutcomes) {
                    INFO(verb.id << " " << PlotVerbs::TemplateId(t));
                    REQUIRE_FALSE(Names(verb.Text(t), "mastermind"));
                }
            }
        }

        SECTION("should name the scheme in its dispatch templates and nowhere else")
        {
            for (const auto& verb : PlotVerbs::kVerbs) {
                for (const auto t : kAll) {
                    INFO(verb.id << " " << PlotVerbs::TemplateId(t));
                    if (t == Template::AgentDispatched) {
                        // The agent is told what to do, not why; the scheme
                        // is the mastermind's to know.
                        REQUIRE_FALSE(Names(verb.Text(t), "scheme"));
                    } else {
                        REQUIRE(Names(verb.Text(t), "scheme") == IsDispatch(t));
                    }
                }
            }
        }

        SECTION("should give each voice the people that voice would name")
        {
            for (const auto& verb : PlotVerbs::kVerbs) {
                INFO(verb.id);
                // The agent remembers who sent them; the mastermind who they
                // sent; somebody acting for themselves names neither.
                REQUIRE(Names(verb.Text(Template::AgentDispatched), "mastermind"));
                REQUIRE(Names(verb.Text(Template::MastermindDispatched), "agent"));
                REQUIRE_FALSE(Names(verb.Text(Template::SelfDispatched), "agent"));
                REQUIRE_FALSE(Names(verb.Text(Template::SelfDispatched), "mastermind"));
                REQUIRE(Names(verb.Text(Template::MastermindToldFailed), "agent"));
                REQUIRE(Names(verb.Text(Template::MastermindToldTimedOut), "agent"));
                REQUIRE(Names(verb.Text(Template::MastermindToldSucceeded), "agent"));
                REQUIRE(Names(verb.Text(Template::Dashboard), "agent"));
            }
        }

        SECTION("should let the accused of a denunciation or an exposure know of it only when it works")
        {
            for (const auto* id : {"denounce", "expose"}) {
                INFO(id);
                const auto* mark = PlotVerbs::FindSlot(*PlotVerbs::Find(id), "mark");
                REQUIRE(mark->awareness == PlotVerbs::Awareness::Success);
            }
        }

        SECTION("should give each slot exactly the memories its awareness can fire, in the voice of who is reached")
        {
            using PlotVerbs::Awareness;
            using PlotVerbs::TargetTemplate;
            for (const auto& verb : PlotVerbs::kVerbs) {
                const auto dashboard = verb.Text(Template::Dashboard);
                for (std::size_t i = 0; i < verb.slotCount; ++i) {
                    const auto& slot = verb.slots[i];
                    INFO(verb.id << " {" << slot.name << "}");
                    const bool onFailure = slot.awareness == Awareness::Contact || slot.awareness == Awareness::Failure;
                    const bool onSuccess = slot.awareness == Awareness::Contact || slot.awareness == Awareness::Success;
                    REQUIRE(slot.Text(TargetTemplate::Failed).empty() == !onFailure);
                    REQUIRE(slot.Text(TargetTemplate::Succeeded).empty() == !onSuccess);
                    for (const auto t : {TargetTemplate::Failed, TargetTemplate::Succeeded}) {
                        const auto text = slot.Text(t);
                        if (text.empty()) {
                            continue;
                        }
                        // They saw the agent; they say "me", not their own name;
                        // they never know the plan; and they know the mastermind
                        // only where the act itself says whose it is.
                        REQUIRE(Names(text, "agent"));
                        REQUIRE_FALSE(Names(text, slot.name));
                        REQUIRE_FALSE(Names(text, "scheme"));
                        REQUIRE((!Names(text, "mastermind") || Names(dashboard, "mastermind")));
                        for (const auto name : PlotVerbs::PlaceholdersIn(text)) {
                            INFO("{" << name << "}");
                            REQUIRE((name == "agent" || name == "mastermind" || PlotVerbs::FindSlot(verb, name)));
                        }
                    }
                }
            }
        }

        SECTION("should give every person slot an awareness, and no other slot one")
        {
            for (const auto& verb : PlotVerbs::kVerbs) {
                for (std::size_t i = 0; i < verb.slotCount; ++i) {
                    const auto& slot = verb.slots[i];
                    INFO(verb.id << " {" << slot.name << "}");
                    if (slot.kind == PlotVerbs::SlotKind::Person) {
                        REQUIRE(slot.awareness != PlotVerbs::Awareness::Unset);
                    } else {
                        REQUIRE(slot.awareness == PlotVerbs::Awareness::Unset);
                    }
                }
            }
        }

        SECTION("should bind only to what some verb can obtain")
        {
            bool someVerbObtains = false;
            for (const auto& verb : PlotVerbs::kVerbs) {
                someVerbObtains = someVerbObtains || verb.obtainsItem;
            }
            for (const auto& verb : PlotVerbs::kVerbs) {
                bool bindsItem = false;
                for (std::size_t i = 0; i < verb.slotCount; ++i) {
                    if (verb.slots[i].rule == PlotVerbs::Rule::BoundItem) {
                        INFO(verb.id << " {" << verb.slots[i].name << "}");
                        REQUIRE(verb.slots[i].kind == PlotVerbs::SlotKind::Thing);
                        REQUIRE(someVerbObtains);
                        bindsItem = true;
                    }
                }
                INFO(verb.id);
                // An agent bound to the item's holder needs an item to hold.
                REQUIRE((!verb.agentBoundToItem || bindsItem));
                // A verb that obtains an item does so from the pool, not by
                // binding to another; otherwise nothing ever starts a chain.
                REQUIRE((!verb.obtainsItem || !bindsItem));
            }
        }

        SECTION("should say what every slot is for, for the scheme call to name its role")
        {
            for (const auto& verb : PlotVerbs::kVerbs) {
                for (std::size_t i = 0; i < verb.slotCount; ++i) {
                    INFO(verb.id << " {" << verb.slots[i].name << "}");
                    REQUIRE_FALSE(verb.slots[i].says.empty());
                }
            }
        }

        SECTION("should finish a scheme unless it only makes, finds or hides something")
        {
            // What these get or hide is for a later step: a forgery shown to
            // nobody, or a thing planted that nobody is sent to find, ends
            // nothing.
            const std::set<std::string_view> preparatory{
                "falsify", "dig_up", "plant", "smith", "concoct", "enchant", "set", "fletch"};
            for (const auto& verb : PlotVerbs::kVerbs) {
                INFO(verb.id);
                REQUIRE(verb.finishes == !preparatory.contains(verb.id));
            }
        }

        SECTION("should keep a place only where the place is what the step acts on")
        {
            // The rule the milestone doc states for {place}. The two verbs
            // listed are the ones that act on a community; a third earning a
            // place should be added here on purpose.
            for (const auto& verb : PlotVerbs::kVerbs) {
                bool hasPlace = false;
                for (std::size_t i = 0; i < verb.slotCount; ++i) {
                    hasPlace = hasPlace || verb.slots[i].kind == PlotVerbs::SlotKind::Place;
                }
                INFO(verb.id);
                REQUIRE(hasPlace == (verb.id == "ask_around" || verb.id == "spread_rumors"));
            }
        }
    }

    SECTION("when the concession template is read")
    {
        SECTION("should name the scheme and nothing else")
        {
            const auto names = PlotVerbs::PlaceholdersIn(PlotVerbs::kConcededTemplate);
            REQUIRE(names.size() == 1);
            REQUIRE(names.front() == "scheme");
        }
    }
}

TEST_CASE("PlotVerbs words for an id", "[PlotVerbs][engine]")
{
    SECTION("when every verb's words are read")
    {
        SECTION("should say its id with spaces for underscores")
        {
            for (const auto& verb : PlotVerbs::kVerbs) {
                INFO(verb.id);
                auto spaced = std::string{verb.id};
                std::replace(spaced.begin(), spaced.end(), '_', ' ');
                REQUIRE(PlotVerbs::Words(verb) == spaced);
            }
        }
    }

    SECTION("when prose carries an id")
    {
        SECTION("should write each one standing as a word in plain words, and touch nothing else")
        {
            REQUIRE(PlotVerbs::Plain("spread_rumors about the court historian around Solitude")
                    == "spread rumors about the court historian around Solitude");
            REQUIRE(PlotVerbs::Plain("win_over Brand-Shei, then ask_around Riften")
                    == "win over Brand-Shei, then ask around Riften");
            REQUIRE(PlotVerbs::Plain("watch the watchtower") == "watch the watchtower");
            REQUIRE(PlotVerbs::Plain("my_spread_rumors_plan") == "my_spread_rumors_plan");
        }
    }
}

TEST_CASE("PlotVerbs preparations", "[PlotVerbs][engine]")
{
    SECTION("when every preparation is read")
    {
        SECTION("should join two real verbs, never a verb to itself")
        {
            for (const auto& p : PlotVerbs::kPreparations) {
                INFO(p.prepares << " <- " << p.verb);
                REQUIRE(PlotVerbs::Find(p.prepares) != nullptr);
                REQUIRE(PlotVerbs::Find(p.verb) != nullptr);
                REQUIRE(p.prepares != p.verb);
            }
        }

        SECTION("should bind a person slot of the preparation to a person slot of the step, or bind nothing")
        {
            for (const auto& p : PlotVerbs::kPreparations) {
                INFO(p.prepares << " <- " << p.verb << " (" << p.slot << " = " << p.from << ")");
                REQUIRE(p.slot.empty() == p.from.empty());
                if (p.slot.empty()) {
                    continue;
                }
                const auto* own = PlotVerbs::FindSlot(*PlotVerbs::Find(p.verb), p.slot);
                const auto* theirs = PlotVerbs::FindSlot(*PlotVerbs::Find(p.prepares), p.from);
                REQUIRE(own != nullptr);
                REQUIRE(theirs != nullptr);
                REQUIRE(own->kind == PlotVerbs::SlotKind::Person);
                REQUIRE(theirs->kind == PlotVerbs::SlotKind::Person);
            }
        }

        SECTION("should list each preparation of a verb once")
        {
            std::set<std::string> seen;
            for (const auto& p : PlotVerbs::kPreparations) {
                const auto key = std::string{p.prepares} + "<-" + std::string{p.verb} + ":" + std::string{p.from};
                INFO(key);
                REQUIRE(seen.insert(key).second);
            }
        }
    }

    SECTION("when a verb's preparations are asked for")
    {
        SECTION("should give them in table order, and none for a verb nothing prepares")
        {
            const auto steal = PlotVerbs::PreparationsOf("steal");
            REQUIRE(steal.size() == 3);
            REQUIRE(PlotVerbs::PreparationsOf("falsify").front()->from == "subject");
            REQUIRE(steal[0]->verb == "watch");
            REQUIRE(steal[2]->verb == "bribe");
            REQUIRE(PlotVerbs::PreparationsOf("ask_around").empty());
            REQUIRE(PlotVerbs::PreparationsOf("deliver").empty());
        }
    }
}

TEST_CASE("PlotVerbs rendering", "[PlotVerbs][engine]")
{
    SECTION("when a scheme is made a verb phrase")
    {
        SECTION("should lowercase the first letter and drop the final punctuation")
        {
            REQUIRE(PlotVerbs::SchemePhrase("Spread a rumor that Hulda drinks.") == "spread a rumor that Hulda drinks");
            REQUIRE(PlotVerbs::SchemePhrase("  Steal the ledger!  ") == "steal the ledger");
            REQUIRE(PlotVerbs::SchemePhrase("").empty());
        }
    }

    SECTION("when a step is rendered with every slot filled")
    {
        const auto step = Filled("watch");

        SECTION("should read as a sentence with no placeholder left in it")
        {
            REQUIRE(PlotModel::Label(step) == "Aeta watches Hulda, learning their habits.");
        }

        SECTION("should render the scheme as a verb phrase where it is named")
        {
            REQUIRE(PlotVerbs::RenderStep(step, Template::MastermindDispatched, "Ruin Hulda.")
                    == "I sent Aeta to keep watch on Hulda and learn their habits, as part of my plan to ruin Hulda.");
        }

        SECTION("should name the mastermind from the step's own record")
        {
            REQUIRE(PlotVerbs::RenderStep(step, Template::AgentDispatched).starts_with("Fanari Strong-Voice asked me"));
        }
    }

    SECTION("when the agent has not been cast yet")
    {
        auto step = Filled("steal");
        step.actor = 0;
        step.actorName.clear();

        SECTION("should read the agent as someone, capitalised where it opens the sentence")
        {
            REQUIRE(PlotModel::Label(step) == "Someone steals a ruby from Hulda.");
        }
    }

    SECTION("when a slot is unfilled")
    {
        auto step = Filled("ask_around");
        step.slots.erase(step.slots.begin() + 2); // the place

        SECTION("should read it as somewhere rather than as a raw placeholder")
        {
            REQUIRE(PlotModel::Label(step) == "Aeta asks around somewhere about Hulda.");
        }
    }

    SECTION("when a plot is given up")
    {
        SECTION("should say what was given up")
        {
            REQUIRE(PlotVerbs::RenderConceded("Steal Pantea's songbook.")
                    == "I gave up on my plan to steal Pantea's songbook.");
        }
    }

    SECTION("when a step has no verb")
    {
        PlotModel::Step prose;
        prose.description = "wait for the steward to leave";

        SECTION("should still read its prose")
        {
            REQUIRE(PlotModel::Label(prose) == "wait for the steward to leave");
            REQUIRE(PlotVerbs::RenderStep(prose, Template::Dashboard).empty());
        }
    }
}

TEST_CASE("PlotVerbs steps survive the co-save", "[PlotVerbs][engine]")
{
    NarrativeEngine::PlotState state;
    PlotModel::Plot plot;
    plot.id = 1;
    plot.mastermind = 0x0001A000;
    plot.mastermindName = "Fanari Strong-Voice";
    plot.scheme = "Win the Skaal's trust.";
    plot.finish = "win Storn Crag-Strider over";
    plot.abandoned.push_back({"steal", 0x0001B000, "Frea"});
    plot.onGuard.push_back(0x0001B001);
    plot.plan.push_back(Filled("deliver"));
    plot.history.push_back(Filled("steal"));
    plot.history.back().sizingEase = 0.64f;
    state.plots.push_back(plot);
    state.RememberScheme(0x0001A000, "forge a dossier on Ondolemar", true);
    state.RememberScheme(0x0001A000, "win the Skaal's trust", false);

    PlotSerialize::VectorSink sink;
    REQUIRE(PlotSerialize::WriteState(state, sink));

    SECTION("when read back")
    {
        NarrativeEngine::PlotState loaded;
        PlotSerialize::VectorSource source{sink.Bytes()};
        std::size_t dropped = 0;
        REQUIRE(PlotSerialize::ReadState(loaded, source, PlotSerialize::kRecordVersion, dropped));
        REQUIRE(dropped == 0);
        REQUIRE(loaded.plots.size() == 1);

        SECTION("should keep how the scheme finishes")
        {
            REQUIRE(loaded.plots[0].finish == "win Storn Crag-Strider over");
            REQUIRE(loaded.plots[0].scheme == plot.scheme);
        }

        SECTION("should keep every way the plot gave up on")
        {
            REQUIRE(loaded.plots[0].abandoned.size() == 1);
            REQUIRE(loaded.plots[0].abandoned[0].verb == "steal");
            REQUIRE(loaded.plots[0].abandoned[0].target == 0x0001B000);
            REQUIRE(loaded.plots[0].abandoned[0].targetName == "Frea");
        }

        SECTION("should keep each step's verb and every slot")
        {
            const auto& step = loaded.plots[0].plan[0];
            REQUIRE(step.verb == "deliver");
            REQUIRE(step.slots.size() == plot.plan[0].slots.size());
            for (std::size_t i = 0; i < step.slots.size(); ++i) {
                REQUIRE(step.slots[i].name == plot.plan[0].slots[i].name);
                REQUIRE(step.slots[i].form == plot.plan[0].slots[i].form);
                REQUIRE(step.slots[i].text == plot.plan[0].slots[i].text);
            }
        }

        SECTION("should keep the mastermind's past schemes and how each went")
        {
            REQUIRE(loaded.past.size() == 2);
            REQUIRE(loaded.past[0].mastermind == 0x0001A000);
            REQUIRE(loaded.past[0].scheme == "forge a dossier on Ondolemar");
            REQUIRE(loaded.past[0].succeeded);
            REQUIRE_FALSE(loaded.past[1].succeeded);
        }

        SECTION("should keep who is on guard against the plot")
        {
            REQUIRE(loaded.plots[0].onGuard == std::vector<RE::FormID>{0x0001B001});
        }

        SECTION("should keep what a step's preparations eased it by")
        {
            REQUIRE(loaded.plots[0].history[0].sizingEase == 0.64f);
            REQUIRE(loaded.plots[0].plan[0].sizingEase == 1.0f);
        }

        SECTION("should render the same sentence it rendered before")
        {
            REQUIRE(PlotModel::Label(loaded.plots[0].plan[0]) == PlotModel::Label(plot.plan[0]));
            REQUIRE(PlotModel::Label(loaded.plots[0].history[0]) == PlotModel::Label(plot.history[0]));
        }
    }

    SECTION("when a slot's form no longer resolves")
    {
        NarrativeEngine::PlotState loaded;
        PlotSerialize::VectorSource source{sink.Bytes(), [](RE::FormID saved, RE::FormID& out) {
                                               // Every slot's form is gone; the people in the plot are not.
                                               out = saved;
                                               return saved < 0x0002B000;
                                           }};
        std::size_t dropped = 0;
        REQUIRE(PlotSerialize::ReadState(loaded, source, PlotSerialize::kRecordVersion, dropped));

        SECTION("should drop the plot rather than run it against nobody")
        {
            REQUIRE(dropped == 1);
            REQUIRE(loaded.plots.empty());
        }
    }
}

TEST_CASE("PlotState keeps a mastermind's last few schemes", "[PlotVerbs][engine]")
{
    NarrativeEngine::PlotState state;
    for (std::size_t i = 0; i < PlotModel::kPastSchemesKept + 2; ++i) {
        state.RememberScheme(0x0001A000, "scheme " + std::to_string(i), true);
    }
    state.RememberScheme(0x0001A001, "somebody else's", false);

    SECTION("when a mastermind has finished more than are kept")
    {
        SECTION("should drop their oldest, and nobody else's")
        {
            REQUIRE(state.past.size() == PlotModel::kPastSchemesKept + 1);
            REQUIRE(state.past.front().scheme == "scheme 2");
            REQUIRE(state.past.back().mastermind == 0x0001A001);
        }
    }
}
