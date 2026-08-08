#include <GossipGraph.h>

#include <logger.h>
#include <Settings.h>

#include <SimpleIni.h>

#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace NarrativeEngine::GossipGraph
{
    namespace
    {
        constexpr const char* kFactionFilePath = "Data/SKSE/Plugins/NarrativeEngine/GossipFactions.ini";

        // Cap on the parentLoc walk. Vanilla chains are 3-4 deep; the
        // guard is against malformed or cyclic mod data, which is the
        // only way this loop can fail to terminate.
        constexpr int kMaxParentDepth = 16;

        // Tier keyword sets, by EditorID. The household criterion is
        // "a group of people sleep here", which is why inns, shops,
        // guild halls, barracks, temples, farms, ships and camps all
        // qualify alongside actual houses. See the phase doc's
        // "Household indicators" for the full rationale, including why
        // LocTypeJail is deliberately in and LocTypeDungeon is out.
        constexpr std::array kHouseholdKeywords{
            "LocTypeDwelling",      "LocTypeHouse",          "LocTypeStewardsDwelling",
            "LocTypePlayerHouse",   "BYOH_LocTypeHomestead", "LocTypeInn",
            "LocTypeStore",         "LocTypeGuild",          "LocTypeTemple",
            "LocTypeCastle",        "LocTypeFarm",           "LocTypeLumberMill",
            "LocTypeMine",          "LocTypeBarracks",       "LocTypeMilitaryCamp",
            "LocTypeMilitaryFort",  "LocTypeJail",           "LocTypeBanditCamp",
            "LocTypeForswornCamp",  "LocTypeVampireLair",    "LocTypeWarlockLair",
            "LocTypeOrcStronghold", "LocTypeShip",
        };
        constexpr std::array kSettlementKeywords{
            "LocTypeHabitation",
            "LocTypeHabitationHasInn",
            "LocTypeCity",
            "LocTypeTown",
            "LocTypeSettlement",
            "LocTypeHoldCapital",
            "LocTypeOrcStronghold",
        };
        // LocTypeHoldMajor / LocTypeHoldMinor sit on HOLD locations, not
        // on settlements, despite reading like settlement descriptors.
        // Verified against the Spriggit export: Major on Reach/Rift/
        // Whiterun, Minor on Falkreath/Hjaalmarch/Pale/Winterhold.
        constexpr std::array kHoldKeywords{
            "LocTypeHold",
            "LocTypeHoldMajor",
            "LocTypeHoldMinor",
        };

        // Relationship rank -> contact multiplier. Vanilla ships zero
        // Lover and zero Archnemesis records; Lover only ever appears at
        // runtime, via marriage.
        float RankMultiplier(RE::BGSRelationship::RELATIONSHIP_LEVEL level)
        {
            using L = RE::BGSRelationship::RELATIONSHIP_LEVEL;
            switch (level) {
            case L::kLover:
                return 4.0f;
            case L::kConfidant:
                return 3.0f;
            case L::kAlly:
            case L::kFriend:
                return 2.0f;
            case L::kAcquaintance:
                return 1.2f;
            case L::kRival:
                return 0.4f;
            case L::kFoe:
                return 0.15f;
            case L::kEnemy:
                return 0.05f;
            case L::kArchnemesis:
                return 0.0f;
            default:
                return 1.0f;
            }
        }

        struct TierFlags
        {
            bool household = false;
            bool settlement = false;
            bool hold = false;
        };

        // ---- state, immutable after Initialize / RefreshRelationships

        bool g_initialized = false;
        bool g_ready = false;

        std::unordered_map<RE::FormID, RE::FormID> g_parent; // location -> parent location
        std::unordered_map<RE::FormID, TierFlags> g_tiers;   // location -> which tiers it satisfies
        std::unordered_map<RE::FormID, std::string> g_locNames;
        std::unordered_map<RE::FormID, std::string> g_factionNames;

        std::unordered_map<RE::FormID, Participant> g_participants;
        std::vector<RE::FormID> g_participantIds;
        // Placed-reference id -> base NPC id. The bridge into SkyrimNet's
        // id space; see the note on Find() in the header.
        std::unordered_map<RE::FormID, RE::FormID> g_byActorRef;

        std::unordered_map<RE::FormID, std::vector<RE::FormID>> g_household;
        std::unordered_map<RE::FormID, std::vector<RE::FormID>> g_settlement;
        std::unordered_map<RE::FormID, std::vector<RE::FormID>> g_hold;
        std::vector<RE::FormID> g_settlementIds;
        std::vector<RE::FormID> g_holdIds;

        std::unordered_map<RE::FormID, std::vector<PersonalEdge>> g_edges;
        // Faction edges are rebuilt only at Initialize; relationship
        // edges are refreshed per session. Kept separate so a refresh
        // does not have to redo the faction expansion.
        std::unordered_map<RE::FormID, std::vector<PersonalEdge>> g_factionEdges;

        Census g_census;

        const std::vector<RE::FormID> g_emptyIds;
        const std::vector<PersonalEdge> g_emptyEdges;
        const std::string g_emptyName;

        // Name-based faction filter. Size alone does not separate a
        // social group from an attribute bucket: JobInnkeeperFaction has
        // 29 members who are mutual strangers, while a 3-member manor
        // faction shares a roof.
        std::vector<std::string> g_denyFragments;
        std::unordered_set<std::string> g_allowExact;

        std::string ToLower(std::string_view s)
        {
            std::string out(s);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return out;
        }

        // Built-in filter, used when the INI is absent so the subsystem
        // still behaves sensibly out of the box. The shipped file
        // restates these with commentary; this is the safety net.
        void LoadBuiltinDenyList()
        {
            static constexpr std::array kDefaults{
                // Attribute buckets: everyone sharing a property, mutual
                // strangers to each other.
                "crime",
                "town",
                "job",
                "potential",
                "current",
                "favor",
                "gov",
                "guard",
                "trainer",
                "vendor",
                "friend",
                "nopickpocket",
                "marriage",
                "follower",
                "playerfaction",
                "excluded",
                "prisoner",
                "bard",
                "witness",
                "shared",
                "generic",
                // Quest and scripting scaffolding. These are the more
                // damaging class: they span holds for reasons unrelated
                // to who talks to whom, and left in they dominated the
                // cross-hold transmission count in offline testing.
                "alias",
                "partyguest",
                "housecarl",
                "carriage",
                "adoptable",
                "disallow",
                "castlehide",
                "additem",
                "exclusion",
                "neverfill",
                "council",
                "immune",
                "hide",
                "dialogue",
                "scene",
                "patrons",
                // Corpse-handling and creature groupings. These produced
                // 3 of the 8 hold crossings observed in the third
                // validation run: WINoBodyCleanupFaction's members are
                // dead NPCs scattered province-wide, and the draugr
                // factions are crypt undead. Note that "dead" is NOT a
                // usable fragment here — it also matches the Dead Man's
                // Drink, both Halls of the Dead, and (by accident)
                // SolitudeAddvarsHouseFaction.
                "nobodycleanup",
                "draugr",
            };
            g_denyFragments.assign(kDefaults.begin(), kDefaults.end());
        }

        void LoadFactionFilter()
        {
            LoadBuiltinDenyList();
            g_allowExact.clear();

            std::error_code ec;
            if (!std::filesystem::exists(kFactionFilePath, ec)) {
                logger::info("GossipGraph: '{}' not found; using the built-in faction filter ({} deny fragments)",
                             kFactionFilePath,
                             g_denyFragments.size());
                return;
            }

            CSimpleIniA ini;
            ini.SetUnicode();
            ini.SetMultiKey(true);
            if (const auto rc = ini.LoadFile(kFactionFilePath); rc < 0) {
                logger::error("GossipGraph: failed to parse '{}' (SimpleIni error {}); keeping the built-in filter",
                              kFactionFilePath,
                              static_cast<int>(rc));
                return;
            }

            CSimpleIniA::TNamesDepend deny;
            if (ini.GetAllValues("Deny", "Fragment", deny)) {
                g_denyFragments.clear();
                for (const auto& v : deny) {
                    if (v.pItem && *v.pItem) {
                        g_denyFragments.push_back(ToLower(v.pItem));
                    }
                }
            }

            CSimpleIniA::TNamesDepend allow;
            if (ini.GetAllValues("Allow", "EditorID", allow)) {
                for (const auto& v : allow) {
                    if (v.pItem && *v.pItem) {
                        g_allowExact.insert(ToLower(v.pItem));
                    }
                }
            }

            logger::info("GossipGraph: loaded faction filter from '{}' ({} deny fragments, {} explicit allows)",
                         kFactionFilePath,
                         g_denyFragments.size(),
                         g_allowExact.size());
        }

        bool IsBucketFaction(std::string_view editorId)
        {
            if (editorId.empty()) {
                // No EditorID means nothing can be judged about it.
                // Excluded rather than admitted: an unnameable faction
                // cannot be audited, and a wrong admit is worse than a
                // wrong reject here.
                return true;
            }
            const auto lowered = ToLower(editorId);
            if (g_allowExact.contains(lowered)) {
                return false;
            }
            return std::any_of(g_denyFragments.begin(), g_denyFragments.end(), [&](const std::string& frag) {
                return lowered.find(frag) != std::string::npos;
            });
        }

        RE::BGSKeyword* ResolveKeyword(const char* editorId)
        {
            auto* form = RE::TESForm::LookupByEditorID(editorId);
            return form ? form->As<RE::BGSKeyword>() : nullptr;
        }

        // Resolve a keyword-name array to the keywords that actually
        // exist in this load order. A missing keyword is not an error —
        // BYOH_LocTypeHomestead is absent without HearthFires.
        std::vector<RE::BGSKeyword*> ResolveKeywordSet(const auto& names, const char* label)
        {
            std::vector<RE::BGSKeyword*> out;
            out.reserve(names.size());
            for (const char* n : names) {
                if (auto* kw = ResolveKeyword(n)) {
                    out.push_back(kw);
                }
            }
            if (out.size() != names.size()) {
                logger::debug("GossipGraph: {} keywords resolved {}/{} (absent ones are DLC-gated)",
                              label,
                              out.size(),
                              names.size());
            }
            return out;
        }

        bool HasAnyKeyword(RE::BGSLocation* loc, const std::vector<RE::BGSKeyword*>& set)
        {
            return std::any_of(set.begin(), set.end(), [&](RE::BGSKeyword* kw) { return loc->HasKeyword(kw); });
        }

        // First ancestor (inclusive) satisfying each tier. A single node
        // may satisfy several, in which case all three resolve to it —
        // that is the tier-collapse rule, and it is how an Orc
        // stronghold or a lone farm correctly ends up being its own
        // household AND its own settlement.
        void WalkTiers(RE::FormID start, RE::FormID& household, RE::FormID& settlement, RE::FormID& hold)
        {
            household = settlement = hold = 0;
            RE::FormID cur = start;
            for (int depth = 0; cur && depth < kMaxParentDepth; ++depth) {
                const auto it = g_tiers.find(cur);
                if (it != g_tiers.end()) {
                    if (!household && it->second.household) {
                        household = cur;
                    }
                    if (!settlement && it->second.settlement) {
                        settlement = cur;
                    }
                    if (!hold && it->second.hold) {
                        hold = cur;
                    }
                }
                if (household && settlement && hold) {
                    return;
                }
                const auto p = g_parent.find(cur);
                cur = (p == g_parent.end()) ? 0 : p->second;
            }
        }

        // "Unique-flagged" is not the same as "a person who can gossip".
        // 251 of 1285 unique-flagged vanilla records are character
        // creation presets, test actors, or creatures — Alduin and the
        // Daedric princes are all unique-flagged.
        bool IsGossipEligiblePerson(RE::TESNPC* npc, RE::BGSKeyword* actorTypeNpc)
        {
            if (!npc || !npc->IsUnique()) {
                return false;
            }
            const char* name = npc->GetName();
            if (!name || !*name) {
                return false;
            }
            auto* race = npc->GetRace();
            if (!race || !actorTypeNpc || !race->HasKeyword(actorTypeNpc)) {
                return false;
            }
            if (const char* eid = npc->GetFormEditorID(); eid && *eid) {
                const auto lowered = ToLower(eid);
                if (lowered.find("preset") != std::string::npos || lowered.rfind("test", 0) == 0
                    || lowered.find("dummy") != std::string::npos) {
                    return false;
                }
            }
            return true;
        }

        void BuildTierTree()
        {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                return;
            }

            const auto householdKw = ResolveKeywordSet(kHouseholdKeywords, "household");
            const auto settlementKw = ResolveKeywordSet(kSettlementKeywords, "settlement");
            const auto holdKw = ResolveKeywordSet(kHoldKeywords, "hold");

            for (auto* form : dh->GetFormArray<RE::BGSLocation>()) {
                auto* loc = form ? form->As<RE::BGSLocation>() : nullptr;
                if (!loc) {
                    continue;
                }
                const auto id = loc->GetFormID();
                if (loc->parentLoc) {
                    g_parent[id] = loc->parentLoc->GetFormID();
                }
                TierFlags flags;
                flags.household = HasAnyKeyword(loc, householdKw);
                flags.settlement = HasAnyKeyword(loc, settlementKw);
                flags.hold = HasAnyKeyword(loc, holdKw);
                if (flags.household || flags.settlement || flags.hold) {
                    g_tiers[id] = flags;
                }
                if (const char* n = loc->GetFullName(); n && *n) {
                    g_locNames[id] = n;
                } else if (const char* eid = loc->GetFormEditorID(); eid && *eid) {
                    g_locNames[id] = eid;
                }
            }
        }

        // NPC -> its own editor location, from every location's LCUN.
        // The per-entry `editorLoc` is finer-grained than the location
        // carrying the entry: WhiterunLocation's array lists the
        // Gray-Manes with editorLoc = WhiterunHouseGrayManeLocation.
        struct Residence
        {
            RE::FormID location = 0;
            RE::FormID actorRef = 0;
        };

        std::unordered_map<RE::FormID, Residence> BuildResidenceIndex()
        {
            std::unordered_map<RE::FormID, Residence> out;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                return out;
            }

            std::size_t rows = 0;
            std::size_t viaRef = 0;
            for (auto* form : dh->GetFormArray<RE::BGSLocation>()) {
                auto* loc = form ? form->As<RE::BGSLocation>() : nullptr;
                if (!loc) {
                    continue;
                }
                for (const auto& entry : loc->uniqueNPCs) {
                    ++rows;
                    // The LCUN row's `actor` field is the base NPC form.
                    auto* base = entry.actor ? entry.actor->As<RE::TESNPC>() : nullptr;
                    if (!base) {
                        continue;
                    }
                    const auto npcId = base->GetFormID();
                    if (out.contains(npcId)) {
                        continue;
                    }
                    RE::FormID resolved = entry.editorLoc ? entry.editorLoc->GetFormID() : 0;
                    if (!resolved && entry.refID) {
                        // Fallback: ask the placed reference itself.
                        // LookupByID takes the engine's own read-write
                        // lock over the all-forms map, so this is safe.
                        if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(entry.refID)) {
                            if (auto* el = ref->GetEditorLocation()) {
                                resolved = el->GetFormID();
                                ++viaRef;
                            }
                        }
                    }
                    if (!resolved) {
                        resolved = loc->GetFormID();
                    }
                    out[npcId] = Residence{resolved, entry.refID};
                }
            }
            logger::info("GossipGraph: LCUN scan -> {} rows, {} distinct NPCs ({} resolved via placed ref)",
                         rows,
                         out.size(),
                         viaRef);
            return out;
        }

        void BuildParticipants()
        {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                return;
            }
            auto* actorTypeNpc = ResolveKeyword("ActorTypeNPC");
            if (!actorTypeNpc) {
                logger::warn("GossipGraph: ActorTypeNPC keyword missing; creature filtering disabled");
            }

            const auto residence = BuildResidenceIndex();

            for (auto* form : dh->GetFormArray<RE::TESNPC>()) {
                auto* npc = form ? form->As<RE::TESNPC>() : nullptr;
                if (!npc || !npc->IsUnique()) {
                    continue;
                }
                ++g_census.uniqueNpcsScanned;
                if (!IsGossipEligiblePerson(npc, actorTypeNpc)) {
                    ++g_census.rejectedNotPerson;
                    continue;
                }
                const auto id = npc->GetFormID();
                const auto it = residence.find(id);
                if (it == residence.end()) {
                    ++g_census.rejectedNoLocation;
                    continue;
                }

                Participant p;
                p.npc = id;
                p.actorRef = it->second.actorRef;
                p.name = npc->GetName() ? npc->GetName() : "";
                WalkTiers(it->second.location, p.household, p.settlement, p.hold);
                if (!p.hold) {
                    // No tier at all. Correct for wandering caravans,
                    // Daedric princes, and quest actors staged in
                    // holding cells.
                    ++g_census.rejectedNoLocation;
                    continue;
                }

                g_participants.emplace(id, std::move(p));
                g_participantIds.push_back(id);
            }

            for (const auto id : g_participantIds) {
                const auto& p = g_participants.at(id);
                if (p.actorRef) {
                    g_byActorRef.emplace(p.actorRef, id);
                    ++g_census.withActorRef;
                }
                if (p.household) {
                    g_household[p.household].push_back(id);
                }
                if (p.settlement) {
                    g_settlement[p.settlement].push_back(id);
                }
                g_hold[p.hold].push_back(id);
            }
            for (const auto& [loc, _] : g_settlement) {
                g_settlementIds.push_back(loc);
            }
            for (const auto& [loc, _] : g_hold) {
                g_holdIds.push_back(loc);
            }
            // Stable ordering so a fixed RNG seed reproduces a run.
            std::sort(g_settlementIds.begin(), g_settlementIds.end());
            std::sort(g_holdIds.begin(), g_holdIds.end());
        }

        void BuildFactionEdges()
        {
            LoadFactionFilter();

            const auto& cfg = Settings::Get();
            const int lo = std::max(2, cfg.gossipFactionSizeMin);
            const int hi = std::max(lo, cfg.gossipFactionSizeMax);

            // faction -> participant members
            std::unordered_map<RE::FormID, std::vector<RE::FormID>> members;
            for (const auto id : g_participantIds) {
                auto* npc = RE::TESForm::LookupByID<RE::TESNPC>(id);
                if (!npc) {
                    continue;
                }
                for (const auto& fr : npc->factions) {
                    if (fr.faction) {
                        members[fr.faction->GetFormID()].push_back(id);
                    }
                }
            }

            std::size_t pairs = 0;
            for (auto& [factionId, mem] : members) {
                const int size = static_cast<int>(mem.size());
                if (size < lo || size > hi) {
                    continue;
                }
                auto* faction = RE::TESForm::LookupByID<RE::TESFaction>(factionId);
                const char* eid = faction ? faction->GetFormEditorID() : nullptr;
                if (IsBucketFaction(eid ? eid : "")) {
                    continue;
                }
                ++g_census.factionsAdmitted;
                g_factionNames[factionId] = (eid && *eid) ? eid : std::format("0x{:08X}", factionId);
                for (std::size_t i = 0; i < mem.size(); ++i) {
                    for (std::size_t j = i + 1; j < mem.size(); ++j) {
                        g_factionEdges[mem[i]].push_back({mem[j], 1.0f, Channel::Faction, factionId});
                        g_factionEdges[mem[j]].push_back({mem[i], 1.0f, Channel::Faction, factionId});
                        ++pairs;
                    }
                }
            }
            g_census.factionPairs = pairs;
        }

        // Merge faction edges with a freshly-read relationship set.
        // Relationship rank wins where a pair is connected both ways —
        // it carries real signal, whereas the faction multiplier is a
        // flat 1.0.
        void RebuildEdges()
        {
            g_edges = g_factionEdges;
            g_census.relationshipEdges = 0;

            auto* dh = RE::TESDataHandler::GetSingleton();
            if (dh) {
                for (auto* form : dh->GetFormArray<RE::BGSRelationship>()) {
                    auto* rel = form ? form->As<RE::BGSRelationship>() : nullptr;
                    if (!rel || !rel->npc1 || !rel->npc2) {
                        continue;
                    }
                    const auto a = rel->npc1->GetFormID();
                    const auto b = rel->npc2->GetFormID();
                    if (a == b || !g_participants.contains(a) || !g_participants.contains(b)) {
                        continue;
                    }
                    const float mult = RankMultiplier(rel->level.get());
                    ++g_census.relationshipEdges;

                    // Replace any faction edge for the same pair rather
                    // than stacking a second one.
                    const auto upsert = [&](RE::FormID from, RE::FormID to) {
                        auto& list = g_edges[from];
                        const auto it = std::find_if(
                            list.begin(), list.end(), [&](const PersonalEdge& e) { return e.other == to; });
                        if (it != list.end()) {
                            it->multiplier = mult;
                            it->via = Channel::Relationship;
                            it->faction = 0;
                        } else {
                            list.push_back({to, mult, Channel::Relationship, 0});
                        }
                    };
                    upsert(a, b);
                    upsert(b, a);
                }
            }

            g_census.participantsWithPersonalEdge = 0;
            for (const auto& [npc, list] : g_edges) {
                if (!list.empty()) {
                    ++g_census.participantsWithPersonalEdge;
                }
            }
        }

        void LogCensus()
        {
            const auto& c = g_census;
            logger::info("GossipGraph: census -- participants={} (household={}, settlement={})",
                         c.participants,
                         c.withHousehold,
                         c.withSettlement);
            logger::info(
                "GossipGraph: census -- households={} settlements={} holds={}", c.households, c.settlements, c.holds);
            logger::info("GossipGraph: census -- relationshipEdges={} factionsAdmitted={} factionPairs={} "
                         "participantsWithPersonalEdge={}",
                         c.relationshipEdges,
                         c.factionsAdmitted,
                         c.factionPairs,
                         c.participantsWithPersonalEdge);
            logger::info("GossipGraph: census -- uniqueScanned={} rejectedNotPerson={} rejectedNoLocation={}",
                         c.uniqueNpcsScanned,
                         c.rejectedNotPerson,
                         c.rejectedNoLocation);
            // The bridge into SkyrimNet's id space. A participant with no
            // placed reference cannot be matched against an engagement row
            // or be written a memory without an engine lookup, so a large
            // shortfall here caps what gossip can ever do.
            logger::info("GossipGraph: census -- participantsWithActorRef={} ({} without)",
                         c.withActorRef,
                         c.participants > c.withActorRef ? c.participants - c.withActorRef : 0);

            // Offline reference figures for a vanilla + DLC load order
            // (docs/implementation/PHASE_13_GOSSIP_PROPAGATION.md). A large
            // shortfall means the LCUN read is broken, not that the load
            // order differs — that failure mode produced 326
            // participants and a 10-resident Riften during offline work.
            if (c.participants > 0 && c.participants < 400) {
                logger::warn("GossipGraph: only {} participants — offline measurement on vanilla+DLC was 857. "
                             "Suspect the LCUN residence read rather than load-order differences.",
                             c.participants);
            }

            if (!Settings::Get().debugMode) {
                return;
            }
            std::vector<std::pair<std::size_t, RE::FormID>> bySize;
            bySize.reserve(g_settlement.size());
            for (const auto& [loc, mem] : g_settlement) {
                bySize.emplace_back(mem.size(), loc);
            }
            std::sort(bySize.begin(), bySize.end(), std::greater<>());
            for (std::size_t i = 0; i < bySize.size() && i < 15; ++i) {
                logger::debug(
                    "GossipGraph: settlement {:>4} residents  {}", bySize[i].first, LocationName(bySize[i].second));
            }
        }
    } // namespace

    const char* ChannelName(Channel c)
    {
        switch (c) {
        case Channel::Household:
            return "household";
        case Channel::Settlement:
            return "settlement";
        case Channel::Hold:
            return "hold";
        case Channel::Province:
            return "province";
        case Channel::Faction:
            return "faction";
        case Channel::Relationship:
            return "relationship";
        default:
            return "unknown";
        }
    }

    void Initialize()
    {
        if (g_initialized) {
            return;
        }
        g_initialized = true;

        if (!Settings::Get().gossipEnabled) {
            logger::info("GossipGraph: disabled (bGossipEnabled=false); no graph built");
            return;
        }

        const auto start = std::chrono::steady_clock::now();

        BuildTierTree();
        if (g_tiers.empty()) {
            logger::error("GossipGraph: no tier-classified locations; graph is empty");
            return;
        }
        BuildParticipants();
        if (g_participantIds.empty()) {
            logger::error("GossipGraph: no participants resolved; graph is empty");
            return;
        }
        BuildFactionEdges();
        RebuildEdges();

        g_census.participants = g_participantIds.size();
        g_census.households = g_household.size();
        g_census.settlements = g_settlement.size();
        g_census.holds = g_hold.size();
        for (const auto id : g_participantIds) {
            const auto& p = g_participants.at(id);
            if (p.household) {
                ++g_census.withHousehold;
            }
            if (p.settlement) {
                ++g_census.withSettlement;
            }
        }

        g_ready = true;
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        logger::info("GossipGraph: built in {} ms", elapsedMs);
        LogCensus();
    }

    bool IsReady()
    {
        return g_ready;
    }

    void RefreshRelationships()
    {
        if (!g_ready) {
            return;
        }
        RebuildEdges();
        logger::info("GossipGraph: relationships refreshed ({} edges, {} participants with a personal edge)",
                     g_census.relationshipEdges,
                     g_census.participantsWithPersonalEdge);
    }

    std::size_t ParticipantCount()
    {
        return g_participantIds.size();
    }

    const std::vector<RE::FormID>& Participants()
    {
        return g_participantIds;
    }

    const Participant* FindByActorRef(RE::FormID actorRef)
    {
        if (actorRef == 0) {
            return nullptr;
        }
        const auto it = g_byActorRef.find(actorRef);
        return it == g_byActorRef.end() ? nullptr : Find(it->second);
    }

    RE::FormID ActorRefFor(RE::FormID npc)
    {
        const auto* p = Find(npc);
        return p ? p->actorRef : 0;
    }

    const Participant* Find(RE::FormID npc)
    {
        const auto it = g_participants.find(npc);
        return it == g_participants.end() ? nullptr : &it->second;
    }

    const std::vector<RE::FormID>& HouseholdMembers(RE::FormID loc)
    {
        const auto it = g_household.find(loc);
        return it == g_household.end() ? g_emptyIds : it->second;
    }

    const std::vector<RE::FormID>& SettlementMembers(RE::FormID loc)
    {
        const auto it = g_settlement.find(loc);
        return it == g_settlement.end() ? g_emptyIds : it->second;
    }

    const std::vector<RE::FormID>& HoldMembers(RE::FormID loc)
    {
        const auto it = g_hold.find(loc);
        return it == g_hold.end() ? g_emptyIds : it->second;
    }

    const std::vector<PersonalEdge>& PersonalEdges(RE::FormID npc)
    {
        const auto it = g_edges.find(npc);
        return it == g_edges.end() ? g_emptyEdges : it->second;
    }

    const std::string& NpcName(RE::FormID npc)
    {
        const auto it = g_participants.find(npc);
        return it == g_participants.end() ? g_emptyName : it->second.name;
    }

    const std::string& LocationName(RE::FormID loc)
    {
        const auto it = g_locNames.find(loc);
        return it == g_locNames.end() ? g_emptyName : it->second;
    }

    const std::string& FactionName(RE::FormID faction)
    {
        const auto it = g_factionNames.find(faction);
        return it == g_factionNames.end() ? g_emptyName : it->second;
    }

    const std::vector<RE::FormID>& Settlements()
    {
        return g_settlementIds;
    }

    const std::vector<RE::FormID>& Holds()
    {
        return g_holdIds;
    }

    const Census& GetCensus()
    {
        return g_census;
    }
} // namespace NarrativeEngine::GossipGraph
