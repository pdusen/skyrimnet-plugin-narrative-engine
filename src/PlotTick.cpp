#include <PlotTick.h>

#include <DashboardUIManager.h>
#include <EngineUtils.h>
#include <logger.h>
#include <PlotBirth.h>
#include <PlotCasting.h>
#include <PlotDispatch.h>
#include <PlotLog.h>
#include <PlotMenus.h>
#include <PlotPopulation.h>
#include <PlotResolution.h>
#include <PlotSchedule.h>
#include <PlotState.h>
#include <Settings.h>

#include <algorithm>
#include <array>

namespace NarrativeEngine::PlotTick
{
    namespace
    {
        // Plugin-thread only. Poll() is called from PollOnPluginThread,
        // which is itself serialised through AsyncDispatch, so this
        // needs no synchronisation of its own.
        double g_lastFiredGameHours = 0.0;
        bool g_needsRebase = true;

        // In-world hours that forced ticks have injected on top of the
        // real calendar, and which every later stamp carries.
        //
        // A forced tick has to hand the simulation a game time it has
        // not seen before: budgets are spent per tick, and terminal
        // plots are reaped by age, so two ticks stamped with the same
        // hour are not "one tick twice", they are a clock that stopped.
        // The obvious implementation - stamp from the live calendar and
        // push the schedule anchor past the stamps - is what shipped,
        // and it was wrong twice over. The calendar does not move while
        // the dashboard holds the game paused, so every burst restarted
        // from the same hour; and an anchor parked in the future looks
        // exactly like a save loaded from the past, so the next poll
        // "re-based" it straight back down. Between them the sim clock
        // sawtoothed instead of advancing.
        //
        // Kept separate from the anchor precisely so those two concerns
        // stop fighting: the anchor still tracks the real calendar and
        // its backwards-detection still means what it says, while this
        // only ever grows. Reset per session, since it describes an
        // offset into a world the next save has not lived through.
        double g_forcedAheadGameHours = 0.0;

        // --- Stub content -------------------------------------------
        //
        // Hardcoded ladders, replaced wholesale by the birth prompt in
        // step 11. They exist so the MACHINERY can be exercised and
        // argued with at zero LLM cost: the casting distribution, the
        // progress-race arithmetic and the terminal-state bookkeeping
        // are all observable long before any model writes a word.
        //
        // Chosen to cover every step type and both conspicuousness
        // values, because a stub set that only exercised the easy types
        // would validate the easy half of the model.
        struct StubLadder
        {
            std::array<PlotModel::StepType, 4> steps;
            std::size_t length;
        };

        constexpr std::array<StubLadder, 5> kStubLadders{{
            {{PlotModel::StepType::Locate,
              PlotModel::StepType::Surveil,
              PlotModel::StepType::Acquire,
              PlotModel::StepType::Conceal},
             4},
            {{PlotModel::StepType::Locate,
              PlotModel::StepType::Acquire,
              PlotModel::StepType::Deliver,
              PlotModel::StepType::Locate},
             3},
            {{PlotModel::StepType::Surveil,
              PlotModel::StepType::Discredit,
              PlotModel::StepType::Locate,
              PlotModel::StepType::Locate},
             2},
            {{PlotModel::StepType::Locate,
              PlotModel::StepType::Suborn,
              PlotModel::StepType::Sabotage,
              PlotModel::StepType::Conceal},
             4},
            {{PlotModel::StepType::Surveil,
              PlotModel::StepType::Suborn,
              PlotModel::StepType::Discredit,
              PlotModel::StepType::Locate},
             3},
        }};

        // Travel distance along the road network.
        //
        // Step 12 measured the hold proxy this replaced and found it had
        // almost no range: same-hold / different-hold over ten holds put
        // 91% of steps at the same value, so distance was a flat surcharge
        // rather than a variable -- and since Step 13 made distance the
        // only circumstance that moves a step's odds, a distance that
        // barely varies means odds that barely vary.
        //
        // The hold comparison survives as the fallback, for members whose
        // settlement has no map marker and for a session with the road
        // graph switched off.
        double TravelDistanceNorm(const PlotCasting::Member* actor, const PlotCasting::Member* target)
        {
            if (actor == nullptr || target == nullptr) {
                return 0.5;
            }
            const double road = PlotPopulation::RoadDistanceNorm(actor->roadNode, target->roadNode);
            if (road >= 0.0) {
                return road;
            }
            if (actor->hold == 0 || target->hold == 0) {
                return 0.5;
            }
            return actor->hold == target->hold ? 0.2 : 0.8;
        }

        // Target importance: how far up an organisation the target
        // sits. A jarl is a harder mark than a farmhand.
        //
        // This reads the same normalised standing the roster gives
        // casting, which means a faction with a declared hierarchy makes
        // its leaders harder targets automatically. Someone in no
        // faction, or on the bottom rung of one, is a soft mark.
        double TargetImportance(const PlotCasting::Member* target)
        {
            if (target == nullptr || target->factions.empty()) {
                return 0.25;
            }
            double best = 0.0;
            for (const auto& f : target->factions) {
                best = std::max(best, f.standing);
            }
            return std::clamp(0.3 + 0.7 * best, 0.0, 1.0);
        }

        PlotCasting::Cooldowns CooldownsFromSettings()
        {
            const auto& cfg = Settings::Get();
            return {static_cast<double>(cfg.plotMastermindCooldownDays),
                    static_cast<double>(cfg.plotActorCooldownDays)};
        }

        // Free the mastermind's slot and stamp the plot terminal.
        void EndPlot(PlotState& state,
                     PlotModel::Plot& plot,
                     PlotModel::PlotStatus status,
                     PlotModel::PlotOutcome outcome,
                     double gameDay)
        {
            plot.status = status;
            plot.outcome = outcome;
            plot.endedOnGameDay = gameDay;

            PlotCasting::Release(state.occupancy, plot.mastermind, gameDay, CooldownsFromSettings());

            if (status == PlotModel::PlotStatus::Succeeded) {
                ++state.counters.plotsSucceeded;
            } else {
                ++state.counters.plotsFailed;
            }

            PlotLog::End(plot);
        }

        // Push a resolved step into history, capped, and release its
        // actor.
        void RetireStep(PlotState& state, PlotModel::Plot& plot, PlotModel::Step step, double gameDay)
        {
            if (step.actor != 0 && step.actor != plot.mastermind) {
                PlotCasting::Release(state.occupancy, step.actor, gameDay, CooldownsFromSettings());
            }

            plot.history.push_back(std::move(step));
            const auto cap = static_cast<std::size_t>(std::max(1, Settings::Get().plotStepHistoryCap));
            while (plot.history.size() > cap) {
                plot.history.erase(plot.history.begin());
            }
        }

        // Cast an actor and size the step. Returns false when nobody can
        // be found at all, which ends the plot rather than leaving it
        // stuck.
        bool DispatchStep(PlotState& state, PlotModel::Plot& plot, double gameDay)
        {
            const auto& population = PlotPopulation::Get();
            PlotModel::Step& step = plot.plan[plot.cursor];

            const auto cast = PlotCasting::SelectActor(population,
                                                       plot.mastermind,
                                                       state.occupancy,
                                                       gameDay,
                                                       PlotPopulation::AlivePredicate(),
                                                       PlotPopulation::SeatedPredicate(),
                                                       state.rngState);
            if (cast.chosen == 0) {
                return false;
            }

            const PlotCasting::Member* actor = population.Find(cast.chosen);
            const PlotCasting::Member* target = population.Find(step.target);

            step.actor = cast.chosen;
            step.actorName = actor != nullptr ? actor->name : std::string{};
            step.state = PlotModel::StepState::InProgress;

            const auto& cfg = Settings::Get();
            const double travel = TravelDistanceNorm(actor, target);
            const double importance = TargetImportance(target);

            // Threshold first: the floor under the budget is a function of
            // it, so the deadline cannot be settled until the work is.
            step.threshold = PlotResolution::SizeThreshold(step.type, importance, travel);
            step.budget = std::max(
                PlotResolution::SizeBudget(step.type, importance),
                PlotResolution::MinimumViableBudget(
                    step.threshold, cfg.plotProgressRateMin, cfg.plotProgressRateMax, cfg.plotProgressMaxFraction));
            step.sizingTravel = static_cast<float>(travel);
            step.sizingImportance = static_cast<float>(importance);
            step.sizingCompetence = actor != nullptr ? static_cast<float>(actor->skills.competence) : 0.5f;
            step.sizingSuitability =
                actor != nullptr ? static_cast<float>(PlotResolution::Suitability(
                                       step.type, actor->skills.speech, actor->skills.sneak, actor->skills.pickpocket))
                                 : 0.5f;

            // The mastermind acting for themselves is already engaged in
            // this plot; engaging them again would overwrite the role on
            // their occupancy row and make releasing it start the wrong
            // cooldown.
            if (cast.chosen != plot.mastermind) {
                PlotCasting::Engage(state.occupancy, cast.chosen, PlotModel::Role::Actor, plot.id);
            }

            ++state.counters.stepsDispatched;
            const auto rejected = static_cast<std::size_t>(
                std::count_if(cast.considered.begin(), cast.considered.end(), [](const PlotCasting::Candidate& c) {
                    return c.reject != PlotCasting::Reject::None;
                }));
            PlotLog::Dispatch(plot, step, cast.chosenRung, cast.considered.size(), rejected);
            return true;
        }

        void AssignTargets(PlotState& state, PlotModel::Plot& plot, const PlotCasting::Population& population);

        // Stub adaptation: keep the objective, rebuild the tail from a
        // different ladder. Step 17 replaces this with the LLM call; what
        // must survive that replacement is the shape — the objective is
        // never rewritten, and the cap always terminates.
        bool AdaptPlot(PlotState& state, PlotModel::Plot& plot, double gameDay)
        {
            const auto cap = std::max(0, Settings::Get().plotMaxAdaptations);
            if (plot.adaptations >= cap) {
                EndPlot(state, plot, PlotModel::PlotStatus::Failed, PlotModel::PlotOutcome::AdaptationCapHit, gameDay);
                return false;
            }
            ++plot.adaptations;
            ++state.counters.adaptations;

            const auto& ladder = kStubLadders[Plots::NextRandom(state) % kStubLadders.size()];
            PlotLog::Adapt(plot, plot.adaptations, cap, ladder.length);

            // Rebuild from the cursor onward. Everything before it has
            // already moved to history, and the objective is NOT in the
            // rewritable set - adaptation rewrites the path, never the
            // destination.
            plot.plan.erase(plot.plan.begin() + static_cast<std::ptrdiff_t>(plot.cursor), plot.plan.end());
            for (std::size_t i = 0; i + 1 < ladder.length; ++i) {
                PlotModel::Step step;
                step.type = ladder.steps[i];
                plot.plan.push_back(step);
            }
            PlotModel::Step objective;
            objective.type = plot.objectiveType;
            objective.target = plot.objectiveTarget;
            objective.targetName = plot.objectiveTargetName;
            plot.plan.push_back(objective);

            // The rebuilt tail needs targets for the same reason the
            // original plan did. Without this the new steps go out
            // nameless - "Locate" against nobody - and, worse, size
            // themselves off the null-target fallbacks in
            // TravelDistanceNorm and TargetImportance, so every
            // post-adaptation step gets the same neutral travel and
            // importance regardless of who it is aimed at. The objective
            // already carries its own target and is skipped.
            AssignTargets(state, plot, PlotPopulation::Get());
            return true;
        }

        // Fill in targets for a freshly built plan from the population.
        void AssignTargets(PlotState& state, PlotModel::Plot& plot, const PlotCasting::Population& population)
        {
            if (population.members.empty()) {
                return;
            }

            // Drawn from the mastermind's OWN menu, not the province.
            //
            // Step 16 replaces this with the LLM picking indices off the
            // same menu; until then a uniform draw over the menu is the
            // stub. What changes here is not the randomness but the
            // POOL: the previous stub drew from all 881 unique NPCs, so
            // a Riften pickpocket's scheme could turn on a Solitude
            // priest neither of them had ever met, and every step sized
            // itself off a travel distance that reflected nothing.
            const auto menus = PlotMenus::Build(population, plot.mastermind, PlotMenus::CurrentWorld(), {});

            for (auto& step : plot.plan) {
                if (step.target != 0) {
                    continue;
                }
                // The menu can be empty -- an NPC in no faction, with no
                // ties, whose hold did not resolve. Falling back to the
                // population keeps such a plot runnable rather than
                // leaving it targetless, which is the failure Step 12
                // found the first time round.
                if (!menus.actors.empty()) {
                    const auto& pick = menus.actors[Plots::NextRandom(state) % menus.actors.size()];
                    step.target = pick.npc;
                    step.targetName = pick.name;
                } else {
                    const auto& pick = population.members[Plots::NextRandom(state) % population.members.size()];
                    step.target = pick.npc;
                    step.targetName = pick.name;
                }
            }
        }

        void BirthPlot(const PlotThread::Token& pt, PlotState& state, double gameDay)
        {
            const auto& population = PlotPopulation::Get();
            if (population.members.empty()) {
                return;
            }

            const auto cast = PlotCasting::SelectMastermind(population,
                                                            state.occupancy,
                                                            gameDay,
                                                            PlotPopulation::AlivePredicate(),
                                                            PlotPopulation::SeatedPredicate(),
                                                            state.rngState);
            if (cast.chosen == 0) {
                return;
            }
            const PlotCasting::Member* boss = population.Find(cast.chosen);
            if (boss == nullptr) {
                return;
            }

            PlotModel::Plot plot;
            plot.mastermind = cast.chosen;
            plot.mastermindName = boss->name;
            plot.bornOnGameDay = gameDay;

            // The menus the prompt is rendered from and the menus the
            // response is validated against are the SAME OBJECT. Built
            // once and passed to both, because an index means nothing
            // except against the list it was chosen from -- rebuilding
            // between the two would silently renumber the answer.
            const auto menus = PlotMenus::Build(population, plot.mastermind, PlotMenus::CurrentWorld(), {});

            // Blocks, on the plot worker, by design. Nothing else runs
            // there, so there is nobody to yield to.
            if (!PlotBirth::Compose(pt, *boss, menus, plot)) {
                // Rejected. The slot is never taken and the id is never
                // spent, so a run of bad responses costs call budget and
                // nothing else -- no half-built plot, no leaked
                // occupancy row, no gap in the numbering to explain.
                return;
            }
            plot.id = state.nextPlotId++;

            PlotCasting::Engage(state.occupancy, plot.mastermind, PlotModel::Role::Mastermind, plot.id);
            ++state.counters.plotsBorn;

            // The candidate's own weight, not the mastermind's competence.
            // The line has always been labelled `weight=`, and reporting
            // competence under that name made it read as a 0..1 quantity
            // when the real weight runs 1.0..4.5 — which is precisely the
            // range that says whether standing and faction membership are
            // actually steering selection or just decorating it.
            const auto pick = std::find_if(cast.considered.begin(),
                                           cast.considered.end(),
                                           [&](const PlotCasting::Candidate& c) { return c.npc == cast.chosen; });
            const double weight = pick != cast.considered.end() ? pick->weight : 0.0;

            PlotLog::Born(plot, weight, cast.considered.size());
            state.plots.push_back(std::move(plot));
        }

        void AdvancePlot(PlotState& state, PlotModel::Plot& plot, double gameDay)
        {
            const auto& cfg = Settings::Get();

            // A plot whose mastermind is gone has nobody to adapt it.
            // The player can cause this without ever knowing they did.
            if (!PlotPopulation::IsAlive(plot.mastermind)) {
                EndPlot(state, plot, PlotModel::PlotStatus::Failed, PlotModel::PlotOutcome::MastermindLost, gameDay);
                return;
            }

            if (plot.cursor >= plot.plan.size()) {
                EndPlot(
                    state, plot, PlotModel::PlotStatus::Succeeded, PlotModel::PlotOutcome::ObjectiveAchieved, gameDay);
                return;
            }

            PlotModel::Step& step = plot.plan[plot.cursor];
            if (step.state == PlotModel::StepState::Planned) {
                if (!DispatchStep(state, plot, gameDay)) {
                    EndPlot(state, plot, PlotModel::PlotStatus::Failed, PlotModel::PlotOutcome::Conceded, gameDay);
                }
                // A step dispatched this tick starts working next tick.
                // Sizing and rolling on the same tick would make the
                // first tick of every step worth double.
                return;
            }
            if (step.state != PlotModel::StepState::InProgress) {
                return;
            }

            PlotResolution::TickInputs inputs;
            inputs.blockedByPresence = PlotModel::IsConspicuous(step.type)
                                       && (PlotPopulation::IsNearPlayer(step.actor)
                                           || (step.target != 0 && PlotPopulation::IsNearPlayer(step.target)));
            inputs.roll.competence = step.sizingCompetence;
            inputs.roll.suitability = step.sizingSuitability;
            inputs.roll.threshold = step.threshold;
            inputs.roll.rateMin = cfg.plotProgressRateMin;
            inputs.roll.rateMax = cfg.plotProgressRateMax;
            inputs.roll.maxFractionPerTick = cfg.plotProgressMaxFraction;
            inputs.mishap.enabled = cfg.plotMishapEnabled;
            inputs.mishap.chanceBase = cfg.plotMishapChanceBase;
            inputs.mishap.conspicuous = PlotModel::IsConspicuous(step.type);
            inputs.mishap.competence = step.sizingCompetence;

            const auto record = PlotResolution::AdvanceStep(step, inputs, state.rngState);
            PlotLog::Roll(plot, step, record);

            if (!step.IsTerminal()) {
                return;
            }
            PlotLog::Resolve(plot, step);

            const bool succeeded = step.state == PlotModel::StepState::Succeeded;
            if (succeeded) {
                ++state.counters.stepsSucceeded;
            } else if (step.outcome == PlotModel::StepOutcome::FailedCaught) {
                ++state.counters.stepsCaught;
            } else {
                ++state.counters.stepsTimedOut;
            }

            PlotModel::Step resolved = step;
            RetireStep(state, plot, std::move(resolved), gameDay);

            if (succeeded) {
                ++plot.cursor;
                if (plot.cursor >= plot.plan.size()) {
                    EndPlot(state,
                            plot,
                            PlotModel::PlotStatus::Succeeded,
                            PlotModel::PlotOutcome::ObjectiveAchieved,
                            gameDay);
                }
                return;
            }
            AdaptPlot(state, plot, gameDay);
        }

        // Drop terminal plots once their retention window passes. Their
        // memories persist in SkyrimNet regardless; this is only the
        // record the dashboard reads.
        void ReapPlots(PlotState& state, double gameDay)
        {
            const double retention = std::max(0.0, static_cast<double>(Settings::Get().plotTerminalRetentionDays));
            const auto before = state.plots.size();
            std::erase_if(state.plots, [gameDay, retention](const PlotModel::Plot& p) {
                return p.IsTerminal() && gameDay - p.endedOnGameDay > retention;
            });

            // Spent occupancy rows go with them. They are not covered by
            // the retention window -- a row stops meaning anything the
            // moment its last cooldown is up, whereas a terminal plot is
            // deliberately kept around to be read on the dashboard.
            const auto rows = PlotCasting::PruneExpired(state.occupancy, gameDay);

            PlotLog::Reap(before - state.plots.size(), rows, gameDay);
        }

        void RunSimulation(const PlotThread::Token& pt, PlotState& state, double gameDay)
        {
            const auto budget = static_cast<std::size_t>(std::max(1, Settings::Get().plotMaxConcurrent));

            // Birth first, before any plot is advanced. A slot freed by a
            // plot ending on THIS tick is not refilled until the next
            // one, which keeps the budget check reading the world as the
            // tick found it.
            if (state.ActivePlotCount() < budget) {
                BirthPlot(pt, state, gameDay);
            }

            // Indexed rather than ranged: BirthPlot above may have
            // appended, and AdvancePlot never does, but the vector is
            // still live and an iterator would be one refactor away from
            // dangling.
            for (std::size_t i = 0; i < state.plots.size(); ++i) {
                if (!state.plots[i].IsTerminal()) {
                    AdvancePlot(state, state.plots[i], gameDay);
                }
            }

            ReapPlots(state, gameDay);
        }

        // Run one unit of plot work, as of `asOfGameHours`.
        //
        // Cancellation is checked at every operation boundary, not only
        // before the publish. A tick that keeps running past a load
        // writes memories into SkyrimNet's database — which lives
        // outside our co-save and is not rolled back by loading an
        // earlier game — and from step 17 will mutate inventories and
        // relationship ranks too. Discarding results at the end would
        // disown those writes without preventing them.
        void RunTick(const PlotThread::Token& pt, const PlotDispatch::CancellationHandle& cancel, double asOfGameHours)
        {
            if (cancel && cancel->IsCancelled()) {
                return;
            }

            // A load that landed while this job sat in the queue takes
            // effect here, before anything reads live state.
            Plots::TakePendingState(pt);

            if (cancel && cancel->IsCancelled()) {
                return;
            }

            PlotState& state = Plots::MutableState(pt);
            state.simGameDay = asOfGameHours / 24.0;
            ++state.counters.ticksRun;

            RunSimulation(pt, state, state.simGameDay);

            if (cancel && cancel->IsCancelled()) {
                return;
            }

            PlotLog::Tick(state.counters.ticksRun,
                          state.simGameDay,
                          state.ActivePlotCount(),
                          static_cast<std::size_t>(std::max(1, Settings::Get().plotMaxConcurrent)));

            // Published at the END of the unit of work, never during one.
            // A snapshot taken mid-tick would show a half-advanced
            // simulation: some plots stepped to the new game day and
            // some not.
            Plots::PublishSnapshot(pt);

            // Publishing only updates the snapshot the dashboard would
            // read on its next push; nothing pushes on its own, so a tab
            // left open sat on whatever the last Director evaluation
            // sent until it was closed and reopened. Every other thing
            // that changes plot state pushes, and so must this.
            //
            // Only the last tick of a burst pushes: the handle is
            // retired after this returns, so a count of one means this
            // job is the queue. Forcing ten ticks should redraw the tab
            // once, not compose the full state ten times.
            if (PlotDispatch::OutstandingCount() <= 1) {
                DashboardUIManager::PushFullState();
            }
        }

        void Enqueue(double asOfGameHours)
        {
            PlotDispatch::EnqueueCancellableWork(
                [asOfGameHours](const PlotThread::Token& pt, const PlotDispatch::CancellationHandle& cancel) {
                    RunTick(pt, cancel, asOfGameHours);
                });
        }
    } // namespace

    void Initialize()
    {
        g_lastFiredGameHours = 0.0;
        g_forcedAheadGameHours = 0.0;
        g_needsRebase = true;
    }

    void OnSessionStart()
    {
        // Re-based on the next poll rather than here: this runs on the
        // main thread at kNewGame / kPostLoadGame, and the clock reading
        // that matters is the one the plugin thread sees when it next
        // looks. Marking it is enough, and it keeps the game-clock read
        // on one thread.
        g_needsRebase = true;

        // Fabricated hours do not survive the world that was told about
        // them. Plots restored from a co-save carry day stamps from the
        // session that forced them, so a debug run leaves ages that read
        // as being in the future until the calendar catches up; that is
        // a property of forcing time forward at all, not something the
        // offset should try to paper over by persisting.
        g_forcedAheadGameHours = 0.0;
    }

    void Poll(const PluginThread::Token&)
    {
        const auto& cfg = Settings::Get();
        if (!cfg.plotsEnabled) {
            return;
        }

        const double nowGameHours = EngineUtils::GetCurrentGameHours();

        // Before the Calendar singleton exists the reading is 0.0, which
        // is not a real clock value — treating it as one would make the
        // first genuine reading look like an enormous backlog.
        if (nowGameHours <= 0.0) {
            return;
        }

        if (g_needsRebase) {
            g_lastFiredGameHours = nowGameHours;
            g_needsRebase = false;
            return;
        }

        const auto decision = PlotSchedule::Advance(g_lastFiredGameHours,
                                                    nowGameHours,
                                                    static_cast<double>(cfg.plotTickIntervalGameHours),
                                                    static_cast<std::size_t>(std::max(1, cfg.plotMaxOutstandingTicks)),
                                                    PlotDispatch::OutstandingCount());

        g_lastFiredGameHours = decision.newLastFiredGameHours;

        if (decision.rebased) {
            logger::info("PlotTick: game clock moved backwards; schedule re-based to {:.3f}h", nowGameHours);
            return;
        }
        if (decision.skipped > 0) {
            logger::warn("PlotTick: outstanding cap reached; advancing schedule past {} tick(s) without running them",
                         decision.skipped);
        }
        for (const double stamp : decision.stamps) {
            Enqueue(stamp + g_forcedAheadGameHours);
        }
    }

    void ForceTicks(std::size_t count)
    {
        if (count == 0) {
            return;
        }
        const double nowGameHours = EngineUtils::GetCurrentGameHours();
        const double interval = std::max(0.001, static_cast<double>(Settings::Get().plotTickIntervalGameHours));

        // Stamped as if the schedule had produced them, so a forced
        // tick is indistinguishable from a scheduled one once it is
        // running - but stamped ON TOP of the hours previous bursts
        // fabricated, not from the raw calendar, which does not move
        // while the dashboard has the game paused.
        //
        // The schedule anchor is deliberately left alone. Forcing a tick
        // injects in-world time; it does not consume the natural cadence,
        // and writing a future hour into the anchor is what made the next
        // poll mistake this for a loaded save.
        for (std::size_t i = 1; i <= count; ++i) {
            Enqueue(nowGameHours + g_forcedAheadGameHours + interval * static_cast<double>(i));
        }
        g_forcedAheadGameHours += interval * static_cast<double>(count);

        logger::info("PlotTick: forced {} tick(s); simulation is now {:.1f}h ahead of the calendar",
                     count,
                     g_forcedAheadGameHours);
    }
} // namespace NarrativeEngine::PlotTick
