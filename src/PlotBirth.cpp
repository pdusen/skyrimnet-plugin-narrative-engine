#include <PlotBirth.h>

#include <CharacterBios.h>
#include <GossipGraph.h>
#include <LLMTextSanitizer.h>
#include <logger.h>
#include <PlotCandidates.h>
#include <PlotLog.h>
#include <PlotMenus.h>
#include <PlotPlanning.h>
#include <PlotPopulation.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <mutex>

// The engine-bound half of plot birth: gathering what each of the three
// calls needs to know, and making them in order. The response handling
// is in PlotBirthParse.cpp and is engine-free, because that is where
// every rule worth a fixture lives.
namespace NarrativeEngine::PlotBirth
{
    namespace
    {
        constexpr const char* kCastPrompt = "narrative_engine_plot_cast";
        constexpr const char* kAmbitionPrompt = "narrative_engine_plot_ambition";

        // Set the first time SkyrimNet says no decision can be answered this
        // session -- no decision provider, decisions switched off, or a
        // SkyrimNet too old to have the API. Planning is made of decisions,
        // so every birth after that would spend two chat calls and then
        // fail; this stops them before the first.
        std::atomic<bool> g_cannotDecide{false};

        // Written by the plugin thread every poll, read by birth.
        std::mutex g_playerNameMutex;
        std::string g_playerName;

        // Both chat calls go to the DIRECTOR variant.
        //
        // Composition used to run on the composer, on the argument that
        // inventing a scheme is creative writing. It is not, quite: each
        // of these three calls is a bounded decision with a structured
        // answer -- pick one of five, name two sentences, list some
        // steps -- which is the director's shape. The prose that a
        // reader eventually sees is one sentence per field, not a
        // passage.
        constexpr const char* kVariant = "narrative_engine_director";

        // SkyrimNet speaks ACTOR REFERENCE ids for everything: profiles,
        // memories, related-actor arrays. The population is keyed on the
        // TESNPC base form, so the boundary gets crossed exactly once,
        // here. GossipGraph.h says so in as many words.
        RE::FormID RefOf(RE::FormID npc)
        {
            return GossipGraph::ActorRefFor(npc);
        }

        // The mastermind's own recent memories, newest first.
        //
        // Takes an ACTOR REFERENCE id, not a base form.
        //
        // Through the FILTERED query rather than GetMemoriesForActor,
        // for the reason GossipHarvest documents at length: the old
        // endpoint ranks the whole store before truncating, so a plugin
        // that writes memories back sees its own output crowd out
        // everything else. Plots will write memories later, and by then
        // this would be reading its own homework.
        std::vector<std::string> MemoriesOf(RE::FormID actorRef)
        {
            std::vector<std::string> out;
            if (actorRef == 0 || !SkyrimNetAPI::IsMemorySystemReady()) {
                return out;
            }

            const auto& cfg = Settings::Get();
            MemoryQuery query;
            query.maxCount = std::max(1, cfg.plotBirthMemoryCount);
            query.orderBy = MemoryOrder::GameTimeDesc;
            query.minImportance = cfg.plotBirthMinMemoryImportance;

            const auto raw = SkyrimNetAPI::QueryMemoriesForActor(actorRef, query);
            auto json = nlohmann::json::parse(raw, nullptr, false);
            if (json.is_discarded() || !json.is_array()) {
                return out;
            }
            for (const auto& row : json) {
                if (!row.is_object()) {
                    continue;
                }
                const auto text = row.find("content");
                if (text == row.end() || !text->is_string()) {
                    continue;
                }
                // Sanitized on the way IN as well as out. These strings
                // came from an LLM originally and go straight back into
                // a prompt; a stray zero-width character here would ride
                // through the whole round trip.
                auto clean = LLMTextSanitizer::Sanitize(text->get<std::string>());
                if (!clean.empty()) {
                    out.push_back(std::move(clean));
                }
            }
            return out;
        }

        std::string HoldNameOf(RE::FormID hold)
        {
            if (hold == 0) {
                return {};
            }
            auto* location = RE::TESForm::LookupByID<RE::BGSLocation>(hold);
            const char* name = location != nullptr ? location->GetFullName() : nullptr;
            return name != nullptr ? name : std::string{};
        }

        // How to describe someone's rank in one phrase. Coarse on
        // purpose: the prompt wants "who is this", and a number would
        // invite the model to reason about a scale it cannot see.
        std::string StandingPhrase(double standing)
        {
            if (standing >= 0.99) {
                return "who leads it";
            }
            if (standing >= 0.6) {
                return "who is senior in it";
            }
            if (standing > 0.0) {
                return "who holds a place in it";
            }
            return {};
        }

        double BestStanding(const PlotCasting::Member& member)
        {
            double best = 0.0;
            for (const auto& faction : member.factions) {
                best = std::max(best, faction.standing);
            }
            return best;
        }

        // The character prose the prompts render, straight from the bio
        // file rather than through SkyrimNet.
        //
        // This used to be `npc.UUID`, which keyed
        // render_character_profile(...) inside the template. It worked,
        // for the one NPC in nine SkyrimNet had already met: the UUID
        // is minted when an actor is ENCOUNTERED, so a fresh save
        // resolved 93 of 881 and every other candidate reached the model
        // as a name and a hold. That is what IsCastable existed to
        // filter out, and why the shortlist was drawn five-from-25.
        //
        // The bios are files. CharacterBios reads them for 840 of the
        // same 881 with no reference to SkyrimNet at all, so the profile
        // is simply passed in as text and the template does no lookup.
        //
        // Sections are kept apart rather than concatenated because the
        // prompts head them separately; a single blob was tried and read
        // as a wall of text.
        nlohmann::json BioSections(RE::FormID npc, const std::string& name, const char* stage)
        {
            const auto& bio = CharacterBios::For(npc);
            if (bio.Empty()) {
                logger::warn("PlotBirth[{}]: no biography for {} (base 0x{:X}); the prompt will have no character "
                             "profile to work from.",
                             stage,
                             name,
                             npc);
            }
            nlohmann::json out = nlohmann::json::object();
            out["summary"] = bio.summary;
            out["background"] = bio.background;
            out["personality"] = bio.personality;
            out["aspirations"] = bio.aspirations;
            out["relationships"] = bio.relationships;
            out["occupation"] = bio.occupation;
            out["skills"] = bio.skills;
            return out;
        }

        // --- Call 1 -------------------------------------------------------

        // A form id the way every other prompt in this project renders
        // one: a hex string with an 0x prefix. See
        // narrative_engine_action_select's letter and visit senders.
        std::string FormIdString(RE::FormID id)
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%X", id);
            return buf;
        }

        nlohmann::json CastContext(const PlotCasting::Population& population, const std::vector<RE::FormID>& shortlist)
        {
            nlohmann::json ctx;

            nlohmann::json people = nlohmann::json::array();
            for (const auto npc : shortlist) {
                const auto* member = population.Find(npc);
                if (member == nullptr) {
                    continue;
                }
                nlohmann::json entry = nlohmann::json::object();
                // No index. The model answers with the form id, and
                // offering it a position as well would just be offering
                // it a second, worse way to answer.
                entry["form_id"] = FormIdString(member->npc);
                entry["name"] = member->name;
                entry["hold"] = HoldNameOf(member->hold);
                entry["standing"] = StandingPhrase(BestStanding(*member));
                // Each candidate carries their own prose so the prompt
                // can render a profile per row. That is the whole point
                // of the call: a choice between five names is a coin
                // toss. IsCastable has already guaranteed this is here.
                entry["bio"] = BioSections(member->npc, member->name, "cast");
                people.push_back(std::move(entry));
            }
            ctx["candidates"] = std::move(people);
            return ctx;
        }

        // --- Call 2 -------------------------------------------------------

        // The item pool as it is said: what the ambition call is shown, and
        // what a finish that takes an item has to name.
        std::vector<std::string> PoolItemNames()
        {
            std::vector<std::string> out;
            for (const auto& item : PlotMenus::CurrentWorld().items) {
                out.push_back(item.displayName);
            }
            return out;
        }

        // How many things of each craft the scheme call is shown. Enough to
        // say what the mastermind's people can make, from the cheap end to
        // the dear one, without a list of every sword in Skyrim.
        constexpr std::size_t kMadeExamplesPerCraft = 4;

        // What the mastermind's own craftsmen can make, as it is said.
        std::vector<std::string> MadeNames(const PlotCasting::Population& population, const PlotCasting::Member& boss)
        {
            const std::vector<PlotModel::SlotValue> none;
            const std::vector<const PlotModel::Step*> earlier;
            const PlotCandidates::Request request{
                population, boss.npc, PlotMenus::CurrentWorld(), none, earlier, PlotPopulation::AlivePredicate(), {}};
            return PlotCandidates::MadeExamples(request, kMadeExamplesPerCraft);
        }

        // The schemes `boss` has finished with: those brought off, or those
        // that failed.
        std::vector<std::string> PastSchemes(const std::vector<PlotModel::PastScheme>& past,
                                             const PlotCasting::Member& boss,
                                             bool succeeded)
        {
            std::vector<std::string> out;
            for (const auto& p : past) {
                if (p.mastermind == boss.npc && p.succeeded == succeeded) {
                    out.push_back(p.scheme);
                }
            }
            return out;
        }

        nlohmann::json AmbitionContext(const PlotCasting::Member& boss,
                                       const std::vector<std::string>& made,
                                       const std::vector<PlotModel::PastScheme>& past)
        {
            nlohmann::json ctx;

            ctx["bio"] = BioSections(boss.npc, boss.name, "ambition");

            nlohmann::json person = nlohmann::json::object();
            person["name"] = boss.name;
            person["hold"] = HoldNameOf(boss.hold);
            person["standing"] = StandingPhrase(BestStanding(boss));
            ctx["mastermind"] = std::move(person);

            ctx["memories"] = MemoriesOf(RefOf(boss.npc));
            ctx["player"] = PlayerName();

            // What a scheme can be made of. The scheme is written to end in
            // one of these verbs and to name objects only as the pool does,
            // because planning can build nothing else: a scheme to
            // "document" something was planned as whichever verb came nearest.
            nlohmann::json verbs = nlohmann::json::array();
            for (const auto& verb : PlotVerbs::kVerbs) {
                // Only what a scheme can end in; see PlotPlanning's Goal.
                if (!verb.finishes) {
                    continue;
                }
                nlohmann::json roles = nlohmann::json::array();
                for (std::size_t i = 0; i < verb.slotCount; ++i) {
                    roles.push_back({{"name", verb.slots[i].name}, {"says", verb.slots[i].says}});
                }
                verbs.push_back({{"id", verb.id}, {"line", PlotVerbs::Generic(verb)}, {"roles", std::move(roles)}});
            }
            ctx["verbs"] = std::move(verbs);
            ctx["items"] = PoolItemNames();
            ctx["made"] = made;
            ctx["accomplished"] = PastSchemes(past, boss, true);
            ctx["failed"] = PastSchemes(past, boss, false);
            return ctx;
        }

        // --- Call 3 -------------------------------------------------------
        //
        // A candidate's occupation, as one short phrase for their line on a
        // decision's list: the first sentence of the biography's occupation
        // section, cut at a word if it runs long.
        std::string ShortOccupation(RE::FormID npc)
        {
            std::string text = CharacterBios::For(npc).occupation;
            if (const auto stop = text.find_first_of(".\n"); stop != std::string::npos) {
                text.resize(stop);
            }
            constexpr std::size_t kMax = 90;
            if (text.size() > kMax) {
                const auto space = text.rfind(' ', kMax);
                text.resize(space != std::string::npos ? space : kMax);
            }
            while (!text.empty() && (text.back() == ' ' || text.back() == ',')) {
                text.pop_back();
            }
            if (!text.empty()) {
                text[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[0])));
            }
            return text;
        }

        // Every stage reports the same way, so a rejected birth says
        // WHICH question went wrong. Three calls means three places a
        // plot can die, and "rejected" without a stage is not a
        // debuggable statement.
        void Rejected(const std::string& who, const char* stage, const std::string& why)
        {
            PlotLog::BirthRejected(who, std::string(stage) + ": " + why);
            logger::warn("PlotBirth: rejected a plot for {} at the {} stage: {}", who, stage, why);
        }

        // Ask one question, and ask it a second time if the first answer
        // was refused -- handing back the reason it was refused.
        //
        // Two of the first four births under this pipeline died at the
        // third call, one on a seventh step and one on a trailing
        // `conceal`. Both had already paid for calls one and two, and
        // both were the kind of mistake a model corrects immediately
        // when told. Throwing away the whole birth over them was the
        // most expensive possible response to the cheapest possible
        // error.
        //
        // ONE retry, not a loop. A model that gets it wrong twice is not
        // going to converge on the third go, and a plot is not worth
        // unbounded call budget; the tick simply produces no plot, which
        // it is already allowed to do.
        //
        // The context is rebuilt rather than mutated between attempts so
        // the retry note is the ONLY difference -- a stale field carried
        // over from the first attempt would make the second question a
        // different question.
        template <typename Outcome, typename Build, typename Parse>
        Outcome AskTwice(const PlotThread::Token& pt,
                         const char* promptName,
                         const Build& build,
                         const Parse& parse,
                         const std::string& who,
                         const char* stage)
        {
            Outcome outcome;
            std::string retryNote;

            for (int attempt = 0; attempt < 2; ++attempt) {
                auto ctx = build();
                ctx["retry"] = retryNote;

                const auto call = SkyrimNetAPI::SendCustomPromptToLLM(pt, promptName, kVariant, ctx.dump());
                if (!call.ok) {
                    // A failed CALL is not a refused answer. There is
                    // nothing to tell the model it did wrong, and the
                    // most likely cause is that the endpoint is down,
                    // so a second attempt would just wait again.
                    outcome = Outcome{};
                    outcome.rejection = "the call failed";
                    return outcome;
                }

                try {
                    outcome = parse(call.response);
                } catch (const std::exception& e) {
                    outcome = Outcome{};
                    outcome.rejection = std::string("parsing threw: ") + e.what();
                }
                if (outcome.ok) {
                    if (attempt > 0) {
                        logger::info("PlotBirth[{}]: {} answered correctly on the retry", stage, who);
                    }
                    return outcome;
                }
                retryNote = outcome.rejection;
                logger::info("PlotBirth[{}]: retrying for {} after: {}", stage, who, retryNote);
            }
            return outcome;
        }
    } // namespace

    namespace
    {
        // How a step's race runs, as Settings has it, for planning's
        // estimates.
        void TuneFromSettings(PlotPlanning::Input& input)
        {
            const auto& cfg = Settings::Get();
            input.tuning.rateMin = cfg.plotProgressRateMin;
            input.tuning.rateMax = cfg.plotProgressRateMax;
            input.tuning.maxFractionPerTick = cfg.plotProgressMaxFraction;
            input.tuning.mishapEnabled = cfg.plotMishapEnabled;
            input.tuning.mishapChance = cfg.plotMishapChanceBase;
            input.tuning.preparationEase = cfg.plotPreparationEase;
            input.tuning.alertHardening = cfg.plotAlertHardening;
            input.targetOdds = cfg.plotPlanOdds;
        }
    } // namespace

    PlotPlanning::Outcome Replan(const PlotThread::Token& pt,
                                 const PlotCasting::Population& population,
                                 const PlotModel::Plot& plot)
    {
        PlotPlanning::Outcome refused;
        const auto* boss = population.Find(plot.mastermind);
        if (boss == nullptr) {
            refused.rejection = "the mastermind is no longer in the population";
            return refused;
        }

        PlotPlanning::Input input{population, PlotMenus::CurrentWorld()};
        input.mastermind = boss->npc;
        input.mastermindName = boss->name;
        input.standing = StandingPhrase(BestStanding(*boss));
        input.ambition = plot.ambition;
        input.scheme = plot.scheme;
        input.finish = plot.finish;
        input.abandoned = plot.abandoned;
        input.onGuard = plot.onGuard;
        // The finish is the plan's last step, and still to come: a failed
        // finish ends the plot rather than re-planning it.
        if (!plot.plan.empty() && plot.cursor < plot.plan.size()) {
            input.goalStep = &plot.plan.back();
            input.goal = plot.plan.back().verb;
        }
        input.bio = BioSections(boss->npc, boss->name, "replan");
        TuneFromSettings(input);
        input.alive = PlotPopulation::AlivePredicate();
        input.occupation = ShortOccupation;
        for (const auto& past : plot.history) {
            input.before.push_back(&past);
        }
        // What is left of the cap, and always room for a goal and the step
        // that gets its item.
        const auto spent = std::min(plot.history.size(), PlotPlanning::kPlanCap - PlotPlanning::kPlanFloor);
        input.maxSteps = PlotPlanning::kPlanCap - spent;

        return PlotPlanning::Plan(input, [&pt](std::string_view name, const nlohmann::json& ctx) {
            return SkyrimNetAPI::SendCustomDecision(pt, std::string{name}, ctx.dump());
        });
    }

    std::string Occupation(RE::FormID npc)
    {
        return ShortOccupation(npc);
    }

    void SetPlayerName(std::string name)
    {
        const std::scoped_lock lock(g_playerNameMutex);
        g_playerName = std::move(name);
    }

    std::string PlayerName()
    {
        const std::scoped_lock lock(g_playerNameMutex);
        return g_playerName;
    }

    bool CanDecide() noexcept
    {
        return !g_cannotDecide;
    }

    void OnSessionStart() noexcept
    {
        // A user who set up a decision provider and reloaded deserves plots.
        g_cannotDecide = false;
    }

    bool IsCastable(RE::FormID npc)
    {
        return CharacterBios::Has(npc);
    }

    bool Compose(const PlotThread::Token& pt,
                 const PlotCasting::Population& population,
                 const std::vector<RE::FormID>& shortlist,
                 const std::vector<PlotModel::PastScheme>& past,
                 PlotModel::Plot& plot,
                 std::size_t& decisions)
    {
        decisions = 0;
        // Nothing to choose between is not an LLM's problem. This is the
        // one rejection that happens BEFORE any call rather than after
        // one, because it costs nothing to see coming.
        if (shortlist.empty()) {
            PlotLog::BirthRejected("nobody", "cast: the shortlist was empty");
            return false;
        }

        // --- 1. Who ------------------------------------------------------
        //
        // Named for the log before the choice is made, because a
        // rejection at this stage has no mastermind to name yet and
        // "rejected a plot for nobody" is not a useful line.
        const auto* first = population.Find(shortlist.front());
        const std::string shortlistName = first != nullptr ? first->name : std::string("an unknown candidate");

        // The prompt is rendered from `shortlist` and the answer is
        // validated against the same object. An answer means nothing
        // except against the list it was chosen from -- the menus made
        // the same argument.
        const auto cast = AskTwice<CastOutcome>(
            pt,
            kCastPrompt,
            [&] { return CastContext(population, shortlist); },
            [&](const std::string& response) { return ParseCast(response, shortlist); },
            shortlistName,
            "cast");
        if (!cast.ok) {
            Rejected(shortlistName, "cast", cast.rejection);
            return false;
        }

        const PlotCasting::Member* boss = population.Find(shortlist[cast.choice]);
        if (boss == nullptr) {
            Rejected(shortlistName, "cast", "the chosen candidate is no longer in the population");
            return false;
        }

        // --- 2. Why, and what ---------------------------------------------
        //
        // A gift's item may be a thing the mastermind's people can make, as
        // well as one from the pool, so the call is shown some of those too.
        const auto made = MadeNames(population, *boss);
        const auto accomplished = PastSchemes(past, *boss, true);
        const auto ambition = AskTwice<AmbitionOutcome>(
            pt,
            kAmbitionPrompt,
            [&] { return AmbitionContext(*boss, made, past); },
            [player = PlayerName(), &population, boss, &accomplished](const std::string& response) {
                const auto resolveRoles = [&population, boss](const PlotVerbs::Verb& goal,
                                                              const std::vector<PlotModel::SlotValue>& named,
                                                              std::vector<PlotModel::SlotValue>& resolved) {
                    const std::vector<PlotModel::SlotValue> none;
                    const std::vector<const PlotModel::Step*> earlier;
                    const PlotCandidates::Request request{population,
                                                          boss->npc,
                                                          PlotMenus::CurrentWorld(),
                                                          none,
                                                          earlier,
                                                          PlotPopulation::AlivePredicate(),
                                                          {}};
                    auto resolution = PlotCandidates::ResolveRoles(goal, named, request);
                    resolved = std::move(resolution.values);
                    return resolution.problem;
                };
                return ParseAmbition(response, TextLimits{}, player, resolveRoles, accomplished);
            },
            boss->name,
            "ambition");
        if (!ambition.ok) {
            Rejected(boss->name, "ambition", ambition.rejection);
            return false;
        }

        // --- 3. How --------------------------------------------------------
        //
        // A sequence of decisions rather than one prompt: the step that
        // accomplishes the scheme, then what must come before it. See
        // PlotPlanning.
        PlotPlanning::Input input{population, PlotMenus::CurrentWorld()};
        input.mastermind = boss->npc;
        input.mastermindName = boss->name;
        input.standing = StandingPhrase(BestStanding(*boss));
        input.ambition = ambition.ambition;
        input.scheme = ambition.scheme;
        input.goal = ambition.goal;
        input.goalRoles = ambition.roles;
        input.finish = ambition.finish;
        input.bio = BioSections(boss->npc, boss->name, "steps");
        TuneFromSettings(input);
        input.alive = PlotPopulation::AlivePredicate();
        input.occupation = ShortOccupation;

        const auto steps = PlotPlanning::Plan(input, [&pt](std::string_view name, const nlohmann::json& ctx) {
            return SkyrimNetAPI::SendCustomDecision(pt, std::string{name}, ctx.dump());
        });
        decisions = steps.decisions;
        if (!steps.ok) {
            if (PlotPlanning::SessionCannotDecide(steps.error)) {
                // Said once, here, and then never again this session: the
                // latch stops every later birth before it spends a call.
                g_cannotDecide = true;
                logger::warn("PlotBirth: SkyrimNet answered '{}' to a decision request. Plots are planned by "
                             "decision and need a decision provider set up in SkyrimNet (Beta 26 or later); "
                             "no plot will be born this session.",
                             steps.error);
            }
            Rejected(boss->name, "steps", !steps.error.empty() ? "decision failed: " + steps.error : steps.rejection);
            return false;
        }

        // Nothing is written into `plot` until all three have answered,
        // so a birth that dies at call 3 leaves no half-built plot
        // behind for the caller to have to unpick.
        plot.mastermind = boss->npc;
        plot.mastermindName = boss->name;
        plot.ambition = ambition.ambition;
        plot.scheme = ambition.scheme;
        plot.finish = ambition.finish;
        plot.plan = steps.plan;
        return true;
    }
} // namespace NarrativeEngine::PlotBirth
