#include <PlotFactionRoster.h>

#include <GossipGraph.h>
#include <logger.h>
#include <Settings.h>

#include <algorithm>
#include <fstream>
#include <sstream>

// The engine-bound half of the roster: reading the file off disk,
// turning EditorIDs into forms, and gathering the facts the standing
// calculation needs off a TESNPC.
//
// The parse itself is in PlotFactionParse.cpp and the standing
// arithmetic in PlotFactionStanding.cpp, both engine-free, because
// every rule worth verifying lives in those two and neither could be
// exercised if reaching it required a running game.
namespace NarrativeEngine::PlotFactionRoster
{
    namespace
    {
        constexpr const char* kIniPath = "Data/SKSE/Plugins/NarrativeEngine/PlotFactions.ini";

        std::vector<Entry> g_entries;
        bool g_loaded = false;

        // Resolve an EditorID to a form of the expected type. `found`
        // separates "no such EditorID" from "wrong record type", which
        // are different mistakes and deserve different log lines.
        template <typename T> T* LookupTyped(const std::string& editorId, bool& found, RE::FormType& actual)
        {
            auto* form = RE::TESForm::LookupByEditorID(editorId.c_str());
            found = form != nullptr;
            actual = form ? form->GetFormType() : RE::FormType::None;
            return form ? form->As<T>() : nullptr;
        }

        bool ResolveFaction(const std::string& editorId,
                            const std::string& sectionId,
                            const char* what,
                            RE::FormID& out)
        {
            bool found = false;
            RE::FormType actual = RE::FormType::None;
            auto* faction = LookupTyped<RE::TESFaction>(editorId, found, actual);
            if (!found) {
                logger::warn(
                    "PlotFactions: skipping '{}': {} '{}' did not resolve to any form.", sectionId, what, editorId);
                return false;
            }
            if (!faction) {
                logger::warn("PlotFactions: skipping '{}': {} '{}' resolved to form type {}, not a faction (FACT).",
                             sectionId,
                             what,
                             editorId,
                             static_cast<int>(actual));
                return false;
            }
            out = faction->GetFormID();
            return true;
        }

        // Resolve one parsed section. False skips it, having logged why.
        bool Resolve(const RawEntry& raw, Entry& out)
        {
            out.id = raw.id;
            out.displayName = raw.displayName;
            out.method = raw.method;
            out.membership = raw.membership;
            out.maxRank = raw.maxRank;

            if (!ResolveFaction(raw.factionEditorId, raw.id, "Faction", out.faction)) {
                return false;
            }
            for (const auto& marker : raw.markerEditorIds) {
                RE::FormID resolved = 0;
                if (!ResolveFaction(marker, raw.id, "MarkerFaction", resolved)) {
                    return false;
                }
                out.markers.push_back(resolved);
            }

            if (!raw.officeFactionEditorId.empty()
                && !ResolveFaction(raw.officeFactionEditorId, raw.id, "OfficeFaction", out.officeFaction)) {
                return false;
            }
            for (const auto& candidate : raw.officeCandidateEditorIds) {
                RE::FormID resolved = 0;
                if (!ResolveFaction(candidate, raw.id, "OfficeCandidate", resolved)) {
                    return false;
                }
                out.officeCandidates.push_back(resolved);
            }

            for (const auto& o : raw.overrides) {
                bool found = false;
                RE::FormType actual = RE::FormType::None;
                auto* npc = LookupTyped<RE::TESNPC>(o.npcEditorId, found, actual);
                if (!npc) {
                    // Costs this LINE, not the section. A mod that
                    // replaces General Tullius must not cost you the
                    // Imperial Legion.
                    logger::warn("PlotFactions: '{}': Member '{}' {}; ignoring this line, the rest of the "
                                 "faction is unaffected.",
                                 raw.id,
                                 o.npcEditorId,
                                 found ? "is not an NPC record" : "did not resolve");
                    continue;
                }
                out.overrides.push_back({npc->GetFormID(), o.rank});
            }

            if (out.method == RankMethod::Explicit && out.overrides.empty()) {
                logger::warn("PlotFactions: skipping '{}': every Member line was dropped, so an Explicit "
                             "faction is left describing no hierarchy at all.",
                             raw.id);
                return false;
            }
            return true;
        }
    } // namespace

    void Load()
    {
        if (g_loaded) {
            return;
        }
        g_loaded = true;
        g_entries.clear();

        if (!Settings::Get().plotsEnabled) {
            return;
        }

        std::ifstream file(kIniPath, std::ios::binary);
        if (!file) {
            logger::warn("PlotFactions: could not read {}. Plots will run with no faction hierarchies — "
                         "everyone in a faction will be a peer.",
                         kIniPath);
            return;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();

        const auto report = ParseRoster(buffer.str());
        for (const auto& line : report.skipped) {
            logger::warn("PlotFactions: skipping {}", line);
        }
        for (const auto& line : report.warnings) {
            logger::warn("PlotFactions: {}", line);
        }
        if (!report.disabled.empty()) {
            std::string list;
            for (const auto& id : report.disabled) {
                list += (list.empty() ? "" : ", ") + id;
            }
            logger::info("PlotFactions: {} section(s) switched off in the file: {}", report.disabled.size(), list);
        }

        std::size_t unresolved = 0;
        for (const auto& raw : report.entries) {
            Entry entry;
            if (!Resolve(raw, entry)) {
                ++unresolved;
                continue;
            }
            g_entries.push_back(std::move(entry));
        }

        logger::info("PlotFactions: loaded {} faction(s); {} rejected by the parse, {} by resolution.",
                     g_entries.size(),
                     report.skipped.size(),
                     unresolved);
        for (const auto& e : g_entries) {
            logger::info("PlotFactions:   '{}' ({}) method={} membership={} markers={} overrides={} gate={}",
                         e.id,
                         e.displayName,
                         MethodId(e.method),
                         MembershipId(e.membership),
                         e.markers.size(),
                         e.overrides.size(),
                         e.officeFaction != 0 ? e.officeCandidates.size() : 0);
        }
    }

    bool IsSeated(RE::FormID npc, const Entry& entry)
    {
        if (entry.officeFaction == 0 || entry.officeCandidates.empty()) {
            return true;
        }

        // The actor rather than the base form: runtime faction changes
        // live in ExtraFactionChanges on the reference. A unique NPC's
        // actor is persistent and always resident, so this resolves even
        // with their cell unloaded -- the same property PlotPopulation's
        // liveness checks rely on.
        const auto* participant = GossipGraph::Find(npc);
        auto* actor = participant != nullptr && participant->actorRef != 0
                          ? RE::TESForm::LookupByID<RE::Actor>(participant->actorRef)
                          : nullptr;
        if (actor == nullptr) {
            // No reference to ask. Seated is the safe answer: it leaves
            // the ladder exactly as the authored data describes it,
            // rather than emptying a court because a lookup failed.
            return true;
        }

        auto* office = RE::TESForm::LookupByID<RE::TESFaction>(entry.officeFaction);
        if (office == nullptr) {
            return true;
        }

        bool isCandidate = false;
        for (const RE::FormID id : entry.officeCandidates) {
            auto* candidate = RE::TESForm::LookupByID<RE::TESFaction>(id);
            if (candidate != nullptr && actor->IsInFaction(candidate)) {
                isCandidate = true;
                break;
            }
        }
        if (!isCandidate) {
            return true;
        }
        return actor->IsInFaction(office);
    }

    bool IsLoaded()
    {
        return g_loaded;
    }

    const std::vector<Entry>& Entries()
    {
        return g_entries;
    }

    const Entry* Find(RE::FormID faction)
    {
        const auto it = std::find_if(
            g_entries.begin(), g_entries.end(), [faction](const Entry& e) { return e.faction == faction; });
        return it == g_entries.end() ? nullptr : &*it;
    }

    double StandingOf(RE::FormID npc, RE::FormID faction)
    {
        const Entry* entry = Find(faction);
        if (entry == nullptr) {
            return 0.0;
        }

        auto* npcForm = RE::TESForm::LookupByID<RE::TESNPC>(npc);
        if (npcForm == nullptr) {
            return 0.0;
        }

        MemberFacts facts;
        facts.npc = npc;
        for (const auto& membership : npcForm->factions) {
            if (membership.faction == nullptr) {
                continue;
            }
            const auto id = membership.faction->GetFormID();
            if (id == entry->faction) {
                facts.authoredRank = static_cast<int>(membership.rank);
            }
            for (std::size_t i = 0; i < entry->markers.size(); ++i) {
                if (entry->markers[i] == id) {
                    facts.markerIndices.push_back(i);
                }
            }
        }

        return StandingFrom(*entry, facts);
    }
} // namespace NarrativeEngine::PlotFactionRoster
