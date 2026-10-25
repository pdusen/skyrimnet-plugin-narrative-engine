#include <PlotTestPlots.h>

#include <logger.h>
#include <PlotState.h>
#include <PlotVerbs.h>
#include <Settings.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

// See PlotTestPlots.h.
namespace NarrativeEngine::PlotTestPlots
{
    namespace
    {
        using PlotModel::StepOutcome;
        using PlotModel::StepState;

        struct Npc
        {
            RE::FormID form;
            const char* name;
        };

        // Skyrim.esm base forms, from the Spriggit export.
        constexpr Npc kBrelyna{0x0001C196, "Brelyna Maryon"};
        constexpr Npc kTolfdir{0x0001C19E, "Tolfdir"};
        constexpr Npc kOnmund{0x0001C194, "Onmund"};
        constexpr Npc kJzargo{0x0001C195, "J'zargo"};
        constexpr Npc kNirya{0x0001C19B, "Nirya"};
        constexpr Npc kColette{0x0001C19A, "Colette Marence"};
        constexpr Npc kMirabelle{0x0001C1A0, "Mirabelle Ervine"};
        constexpr Npc kAncano{0x0001E7D7, "Ancano"};
        constexpr Npc kSavos{0x0001C19F, "Savos Aren"};

        // WinterholdCollegeLocation, and dunHarmugstahlJournal01 as the item
        // pool names it.
        const PlotModel::SlotValue kCollege{"place", 0x00076F3A, "the College of Winterhold"};
        const PlotModel::SlotValue kJournal{"item", 0x0008E5DF, "a private journal"};

        PlotModel::SlotValue Person(const char* slot, const Npc& npc)
        {
            return {slot, npc.form, npc.name};
        }

        // A verb step against `mark`, its slots filled as planning leaves
        // them, done by `actor` (or not yet cast, for a null one).
        PlotModel::Step Step(std::string_view verbId,
                             const Npc& boss,
                             const Npc& mark,
                             const Npc* actor,
                             std::initializer_list<PlotModel::SlotValue> slots)
        {
            const auto* verb = PlotVerbs::Find(verbId);
            PlotModel::Step step;
            step.verb = std::string{verbId};
            step.type = verb != nullptr ? verb->profile : PlotModel::StepType::Locate;
            step.target = mark.form;
            step.targetName = mark.name;
            step.slots.push_back(Person("mastermind", boss));
            step.slots.insert(step.slots.end(), slots);
            if (actor != nullptr) {
                step.actor = actor->form;
                step.actorName = actor->name;
            }
            return step;
        }

        PlotModel::Step Ended(PlotModel::Step step, StepOutcome how)
        {
            step.state = how == StepOutcome::Succeeded ? StepState::Succeeded : StepState::Failed;
            step.outcome = how;
            step.budget = 4;
            step.elapsed = 3;
            step.threshold = 1.0f;
            step.progress = how == StepOutcome::Succeeded ? 1.0f : 0.4f;
            return step;
        }

        // Under way, with most of a generous budget left, so a tick or two
        // does not end it before it has been talked about.
        PlotModel::Step Live(PlotModel::Step step)
        {
            step.state = StepState::InProgress;
            step.budget = 20;
            step.elapsed = 2;
            step.threshold = 1.0f;
            step.progress = 0.3f;
            return step;
        }

        // Brelyna wants J'zargo out of Tolfdir's favour. Onmund asked around
        // and was found out, Nirya watched him, Colette is spreading talk
        // now, and the denunciation to Mirabelle is still to come.
        PlotModel::Plot Brelyna()
        {
            const auto& b = kBrelyna;
            const auto& j = kJzargo;
            PlotModel::Plot plot;
            plot.mastermind = b.form;
            plot.mastermindName = b.name;
            plot.ambition = "Be known as the most promising apprentice the College has.";
            plot.scheme = "Have J'zargo lose his place as Tolfdir's favoured apprentice.";
            plot.finish = "Denounce J'zargo to Mirabelle Ervine.";
            plot.history.push_back(
                Ended(Step("ask_around", b, j, &kOnmund, {Person("mark", j), kCollege}), StepOutcome::FailedCaught));
            plot.history.push_back(Ended(Step("watch", b, j, &kNirya, {Person("mark", j)}), StepOutcome::Succeeded));
            plot.plan.push_back(Live(Step("spread_rumors", b, j, &kColette, {Person("mark", j), kCollege})));
            plot.plan.push_back(Step("denounce", b, j, nullptr, {Person("mark", j), Person("authority", kMirabelle)}));
            return plot;
        }

        // Tolfdir means to prove Ancano has been prying. J'zargo watched him,
        // Brelyna is digging up the proof now, and showing it to Savos Aren
        // falls to whoever she finds it with.
        PlotModel::Plot Tolfdir()
        {
            const auto& t = kTolfdir;
            const auto& a = kAncano;
            PlotModel::Plot plot;
            plot.mastermind = t.form;
            plot.mastermindName = t.name;
            plot.ambition = "Keep the College safe from those who would use it for their own ends.";
            plot.scheme = "Prove to Savos Aren that Ancano has been prying into the College's secrets.";
            plot.finish = "Show the proof against Ancano to Savos Aren.";
            plot.history.push_back(Ended(Step("watch", t, a, &kJzargo, {Person("mark", a)}), StepOutcome::Succeeded));
            plot.plan.push_back(Live(Step("dig_up", t, a, &kBrelyna, {kJournal, Person("mark", a)})));
            plot.plan.push_back(
                Step("expose", t, a, nullptr, {kJournal, Person("mark", a), Person("authority", kSavos)}));
            return plot;
        }

        // Takes out any test plot already seeded, and frees whoever it had
        // engaged with no cooldown, so the two buttons switch between the
        // variations. A test plot is told by its mastermind and scheme
        // together, so a real plot of Brelyna's or Tolfdir's is left alone.
        void RemoveTestPlots(PlotState& state)
        {
            const PlotModel::Plot tests[] = {Brelyna(), Tolfdir()};
            const auto isTest = [&tests](const PlotModel::Plot& p) {
                return std::any_of(std::begin(tests), std::end(tests), [&p](const PlotModel::Plot& t) {
                    return p.mastermind == t.mastermind && p.scheme == t.scheme;
                });
            };
            for (const auto& p : state.plots) {
                if (!isTest(p)) {
                    continue;
                }
                for (auto& [npc, row] : state.occupancy) {
                    if (row.plotId == p.id) {
                        row.plotId = 0;
                    }
                }
                logger::info("PlotTestPlots: removed test plot {}", p.id);
            }
            std::erase_if(state.plots, isTest);
        }
    } // namespace

    std::uint32_t Seed(const PlotThread::Token& pt, std::string_view which)
    {
        PlotModel::Plot plot;
        if (which == "brelyna") {
            plot = Brelyna();
        } else if (which == "tolfdir") {
            plot = Tolfdir();
        } else {
            logger::warn("PlotTestPlots::Seed: no test plot named '{}'", which);
            return 0;
        }

        PlotState& state = Plots::MutableState(pt);
        RemoveTestPlots(state);
        if (state.ActivePlotCount() >= static_cast<std::size_t>(std::max(1, Settings::Get().plotMaxConcurrent))) {
            logger::warn("PlotTestPlots::Seed: budget full; not seeding '{}'", which);
            return 0;
        }

        const auto* live = plot.LiveStep();
        std::vector<std::pair<RE::FormID, PlotModel::Role>> engaged{{plot.mastermind, PlotModel::Role::Mastermind}};
        if (live != nullptr && live->actor != plot.mastermind) {
            engaged.emplace_back(live->actor, PlotModel::Role::Actor);
        }
        for (const auto& [npc, role] : engaged) {
            const auto row = state.occupancy.find(npc);
            if (row != state.occupancy.end() && row->second.IsEngaged()) {
                logger::warn("PlotTestPlots::Seed: {:08X} is already in plot {}; not seeding '{}'",
                             npc,
                             row->second.plotId,
                             which);
                return 0;
            }
        }

        plot.id = state.nextPlotId++;
        plot.bornOnGameDay = state.simGameDay;
        for (const auto& [npc, role] : engaged) {
            auto& row = state.occupancy[npc];
            row.plotId = plot.id;
            row.role = role;
        }

        const std::uint32_t id = plot.id;
        state.plots.push_back(std::move(plot));
        ++state.counters.plotsBorn;
        logger::info("PlotTestPlots::Seed: seeded '{}' as plot {}", which, id);
        return id;
    }
} // namespace NarrativeEngine::PlotTestPlots
