#include <PlotMenus.h>

#include <GossipGraph.h>
#include <logger.h>
#include <PlotFactionRoster.h>
#include <PlotPopulation.h>

#include <unordered_set>

// The engine-bound half of the menus: gathering the world the pure
// builder picks from. The relevance rules live in PlotMenuBuild.cpp,
// which is engine-free.
namespace NarrativeEngine::PlotMenus
{
    namespace
    {
        World g_world;
        bool g_built = false;
    } // namespace

    void BuildWorld()
    {
        if (g_built) {
            return;
        }
        g_built = true;
        g_world = {};

        // Settlements, from the population rather than from the whole
        // location tree: a place nobody lives is a place no scheme has a
        // reason to name.
        std::unordered_set<RE::FormID> seen;
        for (const auto& member : PlotPopulation::Get().members) {
            if (member.settlement == 0 || !seen.insert(member.settlement).second) {
                continue;
            }
            auto* location = RE::TESForm::LookupByID<RE::BGSLocation>(member.settlement);
            if (location == nullptr) {
                continue;
            }
            const char* name = location->GetFullName();
            if (name == nullptr || name[0] == '\0') {
                // Unnamed locations exist and are useless in a sentence.
                // Skipping them here rather than filtering later keeps
                // "everything on a menu can be said out loud" true of
                // the menus themselves.
                continue;
            }
            g_world.settlements.push_back({member.settlement, name});
        }

        for (const auto& entry : PlotFactionRoster::Entries()) {
            g_world.factions.push_back({entry.faction, entry.displayName});
        }

        g_world.items = PlotItemPool::Items();

        logger::info("PlotMenus: world built -- {} settlement(s), {} faction(s), {} item(s).",
                     g_world.settlements.size(),
                     g_world.factions.size(),
                     g_world.items.size());
    }

    const World& CurrentWorld()
    {
        return g_world;
    }
} // namespace NarrativeEngine::PlotMenus
