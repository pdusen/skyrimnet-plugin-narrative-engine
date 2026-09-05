#include <PlotBirth.h>

#include <CharacterBios.h>
#include <GossipGraph.h>
#include <LLMTextSanitizer.h>
#include <logger.h>
#include <PlotLog.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>

#include <nlohmann/json.hpp>

#include <cstdio>

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
        constexpr const char* kPlanPrompt = "narrative_engine_plot_steps";

        // All three go to the DIRECTOR variant.
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

        nlohmann::json AmbitionContext(const PlotCasting::Member& boss)
        {
            nlohmann::json ctx;

            ctx["bio"] = BioSections(boss.npc, boss.name, "ambition");

            nlohmann::json person = nlohmann::json::object();
            person["name"] = boss.name;
            person["hold"] = HoldNameOf(boss.hold);
            person["standing"] = StandingPhrase(BestStanding(boss));
            ctx["mastermind"] = std::move(person);

            ctx["memories"] = MemoriesOf(RefOf(boss.npc));
            return ctx;
        }

        // --- Call 3 -------------------------------------------------------

        nlohmann::json PlanContext(const PlotCasting::Member& boss,
                                   const std::string& ambition,
                                   const std::string& scheme)
        {
            nlohmann::json ctx;

            ctx["bio"] = BioSections(boss.npc, boss.name, "steps");

            nlohmann::json person = nlohmann::json::object();
            person["name"] = boss.name;
            person["hold"] = HoldNameOf(boss.hold);
            person["standing"] = StandingPhrase(BestStanding(boss));
            ctx["mastermind"] = std::move(person);

            ctx["ambition"] = ambition;
            ctx["scheme"] = scheme;

            // The manifest, VERBATIM from the model rather than restated
            // in the template. A prompt that lists step types by hand is
            // a second copy of the enum, and the day one gains a member
            // the two disagree silently.
            nlohmann::json stepTypes = nlohmann::json::array();
            for (const auto& traits : PlotModel::kStepTypes) {
                nlohmann::json entry = nlohmann::json::object();
                entry["id"] = traits.id;
                entry["description"] = traits.description;
                stepTypes.push_back(std::move(entry));
            }
            ctx["step_types"] = std::move(stepTypes);

            // No min_steps / max_steps. Offering the model a range is how
            // three plots in a row came back at exactly four steps: the
            // midpoint of any stated range is where it lands. The bound
            // is enforced in ParsePlan, where a violation costs the
            // response rather than shaping it.
            return ctx;
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

    bool IsCastable(RE::FormID npc)
    {
        return CharacterBios::Has(npc);
    }

    bool Compose(const PlotThread::Token& pt,
                 const PlotCasting::Population& population,
                 const std::vector<RE::FormID>& shortlist,
                 PlotModel::Plot& plot)
    {
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
        const auto ambition = AskTwice<AmbitionOutcome>(
            pt,
            kAmbitionPrompt,
            [&] { return AmbitionContext(*boss); },
            [](const std::string& response) { return ParseAmbition(response, TextLimits{}); },
            boss->name,
            "ambition");
        if (!ambition.ok) {
            Rejected(boss->name, "ambition", ambition.rejection);
            return false;
        }

        // --- 3. How --------------------------------------------------------
        auto steps = AskTwice<PlanOutcome>(
            pt,
            kPlanPrompt,
            [&] { return PlanContext(*boss, ambition.ambition, ambition.scheme); },
            [](const std::string& response) { return ParsePlan(response, PlanLimits{}); },
            boss->name,
            "steps");
        if (!steps.ok) {
            Rejected(boss->name, "steps", steps.rejection);
            return false;
        }

        // Nothing is written into `plot` until all three have answered,
        // so a birth that dies at call 3 leaves no half-built plot
        // behind for the caller to have to unpick.
        plot.mastermind = boss->npc;
        plot.mastermindName = boss->name;
        plot.ambition = ambition.ambition;
        plot.scheme = ambition.scheme;
        plot.plan = std::move(steps.plan);
        return true;
    }
} // namespace NarrativeEngine::PlotBirth
