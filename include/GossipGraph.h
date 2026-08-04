#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <RE/Skyrim.h>

// GossipGraph — the static social graph the gossip simulation runs over.
//
// Reconstructs, at runtime, the same structure the offline validation
// script built from the Spriggit export (see
// docs/implementation/PHASE_13_VALIDATION_LOG.md). Three layers:
//
//   1. A TIER TREE over BGSLocation::parentLoc, classified by LocType*
//      keywords into household / settlement / hold. A single location
//      may satisfy more than one tier (an Orc stronghold is both a
//      settlement and a household); the tiers then collapse onto it.
//
//   2. RESIDENCE per unique NPC, read from BGSLocation::uniqueNPCs (the
//      LCUN subrecord), falling back to the placed reference's editor
//      location. Note that no additive merge is needed here: the engine
//      has already unioned each plugin's ACUN override into
//      `uniqueNPCs` by the time this runs. The offline script had to
//      redo that merge by hand and got badly wrong numbers until it
//      did — the runtime does not.
//
//   3. PERSONAL EDGES, the distance-blind channel: BGSRelationship
//      records plus co-membership of a size- and name-filtered faction.
//      Measured on vanilla, ~92% of relationship edges are redundant
//      with proximity (same household or settlement) and only 25 cross
//      a hold boundary, so the faction half of this channel is what
//      actually carries a rumor between holds.
//
// ---------------------------------------------------------------------
// Threading
//
// `Initialize` runs on the main thread at kDataLoaded, after
// HoldGrid::Initialize. `RefreshRelationships` runs on the main thread
// at session start, before Tick begins polling. Between those points
// the graph is IMMUTABLE, and every accessor below is therefore safe to
// call from the plugin thread without a lock — the same contract
// HoldGrid and TravelGraph use.
//
// Nothing here hands back an RE:: pointer. Names are cached as
// std::string at build time specifically so the simulation and its log
// never need to touch the engine to render a line.
namespace NarrativeEngine::GossipGraph
{
    // Which route a transmission took. Recorded on every telling because
    // the channel attribution is the single most diagnostic field in the
    // gossip log — it is what established that organisations, not
    // families, move rumors between holds.
    enum class Channel : std::uint8_t
    {
        Household,
        Settlement,
        Hold,
        Province,
        Faction,
        Relationship,
    };

    const char* ChannelName(Channel c);

    // One gossip-eligible NPC. Any of the three location fields may be 0
    // — an NPC with no household simply shares no household-tier contact
    // with anyone. A participant always has at least `hold`.
    struct Participant
    {
        RE::FormID npc = 0;
        RE::FormID household = 0;
        RE::FormID settlement = 0;
        RE::FormID hold = 0;
        std::string name;
    };

    // A distance-blind tie to one other participant. `multiplier` folds
    // in the BGSRelationship rank multiplier (1.0 for a faction-only
    // edge). `faction` is the shared faction's FormID when `via` is
    // Faction, 0 otherwise; it is carried purely so the log can name it.
    struct PersonalEdge
    {
        RE::FormID other = 0;
        float multiplier = 1.0f;
        Channel via = Channel::Faction;
        RE::FormID faction = 0;
    };

    // One-shot build. Idempotent — a second call returns without work.
    // Gated on Settings::gossipEnabled; a no-op when disabled, so the
    // shipped default costs nothing.
    //
    // Degrades to an empty graph (and logs why) if TESDataHandler or the
    // tier keywords are unavailable. An empty graph makes every
    // accessor return empty and the simulation idle; it never crashes.
    void Initialize();

    // True once Initialize has built a non-empty participant set.
    bool IsReady();

    // Re-read BGSRelationship at session start so runtime relationship
    // changes (marriage sets a Lover rank that no plugin file contains)
    // are reflected. Main thread only, and must run before Tick starts.
    void RefreshRelationships();

    std::size_t ParticipantCount();

    // Every participant's NPC FormID, in a stable order.
    const std::vector<RE::FormID>& Participants();

    // Returns nullptr when `npc` is not a participant.
    const Participant* Find(RE::FormID npc);

    // Tier co-members, INCLUDING the queried NPC. Empty when `loc` is 0
    // or unknown. The returned reference is valid for the session.
    const std::vector<RE::FormID>& HouseholdMembers(RE::FormID loc);
    const std::vector<RE::FormID>& SettlementMembers(RE::FormID loc);
    const std::vector<RE::FormID>& HoldMembers(RE::FormID loc);

    // Personal edges for one participant. Empty for the ~23% who have
    // none.
    const std::vector<PersonalEdge>& PersonalEdges(RE::FormID npc);

    // Cached display names. Empty string when unknown. Safe from any
    // thread; no engine access.
    const std::string& NpcName(RE::FormID npc);
    const std::string& LocationName(RE::FormID loc);
    // EditorID of an admitted social faction. Only admitted factions are
    // cached — a faction the filter rejected can never appear on a
    // transmission, so there is nothing to name.
    const std::string& FactionName(RE::FormID faction);

    // Every settlement that has at least one participant, and every
    // hold. Used by the temporary seeder to stratify its picks and by
    // the startup census.
    const std::vector<RE::FormID>& Settlements();
    const std::vector<RE::FormID>& Holds();

    // Startup census, written to the main log at Initialize and
    // available for the dashboard later. The offline run's figures for
    // a vanilla + DLC load order are in the phase doc; a large
    // shortfall against them means the LCUN read is wrong, not that the
    // load order differs.
    struct Census
    {
        std::size_t participants = 0;
        std::size_t withHousehold = 0;
        std::size_t withSettlement = 0;
        std::size_t households = 0;
        std::size_t settlements = 0;
        std::size_t holds = 0;
        std::size_t relationshipEdges = 0;
        std::size_t factionsAdmitted = 0;
        std::size_t factionPairs = 0;
        std::size_t participantsWithPersonalEdge = 0;
        std::size_t uniqueNpcsScanned = 0;
        std::size_t rejectedNotPerson = 0;
        std::size_t rejectedNoLocation = 0;
    };
    const Census& GetCensus();
} // namespace NarrativeEngine::GossipGraph
