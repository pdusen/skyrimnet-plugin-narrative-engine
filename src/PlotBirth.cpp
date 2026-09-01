#include <PlotBirth.h>

#include <EventLogUtil.h>
#include <GossipGraph.h>
#include <LLMTextSanitizer.h>
#include <logger.h>
#include <PlotLog.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>

#include <nlohmann/json.hpp>

// The engine-bound half of plot birth: gathering what the model needs to
// know and making the call. The response handling is in
// PlotBirthParse.cpp and is engine-free, because that is where every
// rule worth a fixture lives.
namespace NarrativeEngine::PlotBirth
{
    namespace
    {
        constexpr const char* kPromptName = "narrative_engine_plot_birth";
        constexpr const char* kVariant = "narrative_engine_composer";

        // The mastermind's own recent memories, newest first.
        //
        // Takes an ACTOR REFERENCE id, not a base form -- see
        // BuildContext for what passing the wrong one cost.
        //
        // Through the FILTERED query rather than GetMemoriesForActor,
        // for the reason GossipHarvest documents at length: the old
        // endpoint ranks the whole store before truncating, so a plugin
        // that writes memories back sees its own output crowd out
        // everything else. Plots will write memories from Step 19, and
        // by then this would be reading its own homework.
        std::vector<std::string> MemoriesOf(RE::FormID actorRef)
        {
            std::vector<std::string> out;
            if (!SkyrimNetAPI::IsMemorySystemReady()) {
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

        nlohmann::json BuildContext(const PlotCasting::Member& mastermind, const PlotMenus::Menus& menus)
        {
            nlohmann::json ctx;

            // EVERY SkyrimNet endpoint speaks ACTOR REFERENCE ids --
            // profiles, memories, related-actor arrays, all of it. The
            // casting menus are keyed on the TESNPC base form, so the
            // boundary gets crossed exactly once, here. GossipGraph.h
            // says so in as many words; GossipContent already does it.
            //
            // This is what made every plot identical. The base form went
            // out, FormIDToUUID handed back 0, SkyrimNet logged "No bio
            // template found for UUID: 0", and the memory query matched
            // nothing -- so the model met four different masterminds as
            // four bare names with a rank phrase, and wrote the same
            // generic caper four times. It was reading no profile
            // because there was no profile to read.
            const auto actorRef = GossipGraph::ActorRefFor(mastermind.npc);
            if (actorRef == 0) {
                logger::warn("PlotBirth: no placed reference for {} (base 0x{:X}); the prompt will have no "
                             "character profile and no memories to work from.",
                             mastermind.name,
                             mastermind.npc);
            }

            // SkyrimNet's character-profile submodules key every
            // bio decorator off `npc.UUID`. Without it the prompt can
            // say the mastermind's name and nothing about who they are,
            // and a scheme generated from a name is a scheme any NPC
            // could have had.
            const auto uuid = actorRef != 0 ? SkyrimNetAPI::FormIDToUUID(actorRef) : 0;
            if (uuid == 0 && actorRef != 0) {
                logger::warn("PlotBirth: FormIDToUUID(0x{:X}) returned 0 for {}; the prompt will have no "
                             "character profile to work from.",
                             actorRef,
                             mastermind.name);
            }
            nlohmann::json npc = nlohmann::json::object();
            npc["UUID"] = uuid;
            ctx["npc"] = std::move(npc);

            nlohmann::json boss = nlohmann::json::object();
            boss["name"] = mastermind.name;
            boss["hold"] = HoldNameOf(mastermind.hold);
            double best = 0.0;
            for (const auto& faction : mastermind.factions) {
                best = std::max(best, faction.standing);
            }
            boss["standing"] = StandingPhrase(best);
            ctx["mastermind"] = std::move(boss);

            ctx["memories"] = MemoriesOf(actorRef);

            nlohmann::json actors = nlohmann::json::array();
            for (std::size_t i = 0; i < menus.actors.size(); ++i) {
                const auto& actor = menus.actors[i];
                nlohmann::json entry = nlohmann::json::object();
                entry["index"] = i;
                entry["name"] = actor.name;
                entry["relation"] = PlotMenus::RelationPhrase(actor.relation);
                entry["standing"] = StandingPhrase(actor.standing);
                actors.push_back(std::move(entry));
            }
            ctx["actors"] = std::move(actors);

            nlohmann::json items = nlohmann::json::array();
            for (std::size_t i = 0; i < menus.items.size(); ++i) {
                const auto& item = menus.items[i];
                nlohmann::json entry = nlohmann::json::object();
                entry["index"] = i;
                entry["name"] = item.displayName;
                entry["category"] = PlotItemPool::CategoryId(item.category);
                items.push_back(std::move(entry));
            }
            ctx["items"] = std::move(items);

            nlohmann::json factions = nlohmann::json::array();
            for (std::size_t i = 0; i < menus.factions.size(); ++i) {
                nlohmann::json entry = nlohmann::json::object();
                entry["index"] = i;
                entry["name"] = menus.factions[i].name;
                factions.push_back(std::move(entry));
            }
            ctx["factions"] = std::move(factions);

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

            // No min_steps / max_steps. Offering the model a range is
            // how the first three plots all came back at exactly four
            // steps: the midpoint of any stated range is where it
            // lands. The bound is enforced in Parse, where a violation
            // costs the response rather than shaping it.
            return ctx;
        }
    } // namespace

    bool Compose(const PlotThread::Token& pt,
                 const PlotCasting::Member& mastermind,
                 const PlotMenus::Menus& menus,
                 PlotModel::Plot& plot)
    {
        // A plot with nobody to involve is not worth an LLM call. This
        // is the one rejection that happens BEFORE the call rather than
        // after it, because it costs nothing to see coming.
        if (menus.actors.empty()) {
            PlotLog::BirthRejected(mastermind.name, "nobody on the menu to involve");
            return false;
        }

        const auto context = BuildContext(mastermind, menus);
        const auto result = SkyrimNetAPI::SendCustomPromptToLLM(pt, kPromptName, kVariant, context.dump());
        if (!result.ok) {
            PlotLog::BirthRejected(mastermind.name, "the call failed");
            return false;
        }

        // Reading the response is fallible in ways the checks inside
        // Parse cannot enumerate -- a throw from deep inside nlohmann on
        // a shape nobody predicted. Contained here so it costs this one
        // plot rather than unwinding the tick that asked for it.
        Outcome outcome;
        try {
            outcome = Parse(result.response, menus, Limits{});
        } catch (const std::exception& e) {
            PlotLog::BirthRejected(mastermind.name, std::string("parsing threw: ") + e.what());
            return false;
        }

        if (!outcome.ok) {
            PlotLog::BirthRejected(mastermind.name, outcome.rejection);
            logger::warn("PlotBirth: rejected a plot for {}: {}", mastermind.name, outcome.rejection);
            return false;
        }

        plot.ambition = std::move(outcome.ambition);
        plot.plan = std::move(outcome.plan);

        // The objective is recorded on the PLOT and never pushed into
        // the plan. It names the scheme and anchors adaptation; the last
        // step of the plan is what actually achieves it.
        plot.objectiveType = outcome.objective.type;
        plot.objectiveTarget = outcome.objective.target;
        plot.objectiveTargetName = outcome.objective.targetName;
        return true;
    }
} // namespace NarrativeEngine::PlotBirth
