#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <RE/Skyrim.h>

// PlotFactionRoster — which factions the simulation knows the shape of, and
// how seniority inside each is worked out.
//
// NOT an allowlist for participation. Plots are cast from the whole
// unique-NPC population and an NPC in no faction at all can still
// mastermind one. What the roster adds is HIERARCHY: for a listed
// faction it can say that Savos Aren outranks an apprentice, which
// mastermind weighting and the agent ladder both need. A faction that
// is not listed still counts as an organisation — its members are
// simply peers.
//
// ---------------------------------------------------------------------
// Why this is declared rather than derived
//
// Because the data does not answer the question, and that was measured
// rather than assumed. Of the ~857 unique NPCs the simulation draws
// from, exactly one faction — the College of Winterhold — populates an
// authored rank ladder. Ulfric, Kodlak and Astrid all sit at rank 0 in
// the organisations they lead, because vanilla drives guild rank through
// quests at runtime. Two heuristics were tried and rejected on evidence:
// property ownership measures wealth rather than authority (innkeepers
// outranked jarls), and faction nesting fires for 65% of the population
// once household, location and per-vendor factions are counted.
//
// See docs/implementation/PHASE_14_FACTION_PLOTS.md step 7, and
// statics/SKSE/Plugins/NarrativeEngine/PlotFactions.ini for the file
// itself.
namespace NarrativeEngine::PlotFactionRoster
{
    enum class RankMethod : std::uint8_t
    {
        // Read the faction rank authored on each NPC. Needs MaxRank.
        Rank,
        // Seniority from membership of separate, more exclusive
        // factions, listed in descending order.
        Marker,
        // No derivation; the Member overrides are the whole ladder.
        Explicit
    };

    [[nodiscard]] std::string_view MethodId(RankMethod m) noexcept;

    // Who counts as a member of a rostered faction.
    //
    // The default is everyone in the primary faction, which is right for
    // an organisation that has a faction of its own -- the Companions
    // are exactly the members of CompanionsFaction.
    //
    // `Ranked` exists because a hold court has no faction of its own.
    // The only vanilla faction holding a jarl, their steward, their
    // housecarl, their court wizard AND their guards is the hold's crime
    // faction, which is every single person who lives in the hold. Under
    // `Faction` that would enrol every farmer in Whiterun Hold as a
    // member of the court -- false on its face, and it would hand the
    // mastermind weighting a membership bonus that everyone in Skyrim
    // earns, flattening the distinction it exists to draw. `Ranked`
    // narrows membership to the people the ladder actually places, and
    // uses the primary faction only to scope it: "the jarl OF THIS
    // HOLD", out of a province-wide JobJarlFaction.
    enum class Membership : std::uint8_t
    {
        Faction,
        Ranked
    };

    [[nodiscard]] std::string_view MembershipId(Membership m) noexcept;

    struct Override
    {
        RE::FormID npc = 0;
        int rank = 0;
    };

    struct Entry
    {
        std::string id;          // the section id, for logs
        std::string displayName; // for the dashboard and LLM context
        RE::FormID faction = 0;
        RankMethod method = RankMethod::Explicit;
        Membership membership = Membership::Faction;

        // Rank: the top of the ladder, so standing normalises against it.
        int maxRank = 1;

        // Marker: descending seniority, most senior first.
        std::vector<RE::FormID> markers;

        // The tenure gate. Zero when the section declares none.
        //
        // `officeFaction` is the faction whose CURRENT membership means
        // holding the post; `officeCandidates` are the people the gate
        // is about. A member of a candidate faction who is not in the
        // office faction has their standing suspended -- they are
        // treated as though the section did not list them -- and anyone
        // in no candidate faction is untouched.
        //
        // This is the only part of a roster entry that is not a
        // statement about authored data, and it is why the standing is
        // re-read per tick rather than baked at load. In Skyrim it maps
        // onto GovRuling / GovImperial / GovSons, which CWGovernmentScript
        // maintains at runtime with AddToFaction and RemoveFromFaction as
        // holds change hands.
        RE::FormID officeFaction = 0;
        std::vector<RE::FormID> officeCandidates;

        // Valid under EVERY method, layered over whatever the method
        // derived. This is what lets a Rank faction correct one NPC the
        // authored data gets wrong without hand-writing its ladder.
        std::vector<Override> overrides;
    };

    // Load at kDataLoaded, after the data handler is up. Idempotent.
    //
    // Every section is validated on its own: a broken one is skipped
    // with a named reason and the rest still load. An unresolvable
    // `Member` costs that LINE only, never the section — a mod that
    // replaces one NPC should not cost you the faction.
    void Load();

    [[nodiscard]] bool IsLoaded();

    [[nodiscard]] const std::vector<Entry>& Entries();

    // The roster entry for a faction, or nullptr when it is not listed.
    [[nodiscard]] const Entry* Find(RE::FormID faction);

    // Standing of `npc` inside `faction`, NORMALISED to 0..1.
    //
    // Normalised because the top of a two-rung faction and the top of a
    // seven-rung one should weigh the same: leading your organisation
    // means the same thing whether it kept a deep hierarchy or a
    // shallow one.
    //
    // Returns 0 for an unlisted faction, for a non-member, and for
    // anyone on the bottom rung — all three are "no standing to speak
    // of", and distinguishing them would imply a precision the data
    // does not have.
    [[nodiscard]] double StandingOf(RE::FormID npc, RE::FormID faction);

    // Does `npc` currently hold the post `entry`'s ladder claims for
    // them? Safe from any thread, and called straight from the plot
    // worker.
    //
    // Every step is a read of the same kind GossipSim already documented
    // as needing no main thread: the participant lookup is a map built
    // at load and const afterwards, `LookupByID` takes the engine's own
    // read-write lock, and `IsInFaction` is a short scan of two arrays
    // that mutates nothing. Nothing here is more volatile than the
    // 3D-loaded state PlotPopulation::IsNearPlayer reads off-thread on
    // every conspicuous step -- that one changes as the player walks
    // around, where faction membership changes a handful of times in a
    // playthrough.
    //
    // True for everyone when the entry declares no gate, and for anyone
    // the gate does not cover. Otherwise it is exactly the test
    // CWGovernmentScript itself makes: in a candidate faction, and in the
    // office faction right now.
    //
    // Read through the ACTOR, not the TESNPC, and that distinction is
    // the whole point. `AddToFaction` and `RemoveFromFaction` write to
    // ExtraFactionChanges on the reference; the base form's `factions`
    // array keeps the values the plugin was authored with and never
    // moves. Reading the base form would answer "who was in office when
    // Bethesda shipped the game", which is not the question.
    [[nodiscard]] bool IsSeated(RE::FormID npc, const Entry& entry);

    // --- The parse, before anything is resolved -------------------------
    //
    // Reading the file and resolving EditorIDs are separated for the
    // reason PlotSerialize learned the hard way: an interface is not the
    // same as testability. The parse is where every malformed-input rule
    // lives -- missing keys, an unknown method, a method without its
    // parameters, a Member line that is not "<name>, <rank>" -- and none
    // of it can be exercised if reaching it requires a running game.
    //
    // So ParseRoster takes INI TEXT and yields EditorID STRINGS plus a
    // list of what it rejected and why. Resolution happens afterwards,
    // in the engine-bound half.

    struct RawOverride
    {
        std::string npcEditorId;
        int rank = 0;
    };

    struct RawEntry
    {
        std::string id;
        std::string displayName;
        std::string factionEditorId;
        RankMethod method = RankMethod::Explicit;
        Membership membership = Membership::Faction;
        int maxRank = 1;
        std::vector<std::string> markerEditorIds;
        std::string officeFactionEditorId;
        std::vector<std::string> officeCandidateEditorIds;
        std::vector<RawOverride> overrides;
    };

    struct ParseReport
    {
        std::vector<RawEntry> entries;
        // Sections switched off with `Enabled = false`, by id. Kept apart
        // from `warnings` because a section the author deliberately turned
        // off is not a complaint about their file -- the shipped roster
        // ships several off by default, and logging those at warning level
        // would put four grumbles in every user's log describing the
        // intended configuration.
        std::vector<std::string> disabled;
        // One line per section that was skipped, and per Member line
        // that was dropped, each naming what and why. The caller logs
        // them; keeping them as data is what lets a probe assert on the
        // reason rather than only on the count.
        std::vector<std::string> skipped;
        std::vector<std::string> warnings;
    };

    // Pure. `iniText` is the file's contents, not a path.
    [[nodiscard]] ParseReport ParseRoster(const std::string& iniText);

    // --- Testable core -------------------------------------------------
    //
    // The parse and the standing arithmetic are separated from the file
    // and the engine so a probe can drive them. Everything above is the
    // engine-bound half.

    // One NPC's inputs for the standing calculation, gathered by the
    // caller so this stays engine-free.
    struct MemberFacts
    {
        RE::FormID npc = 0;
        // Authored rank in the primary faction. Only consulted by Rank.
        int authoredRank = 0;
        // Which of the entry's markers this NPC belongs to, by index.
        // Only consulted by Marker.
        std::vector<std::size_t> markerIndices;
    };

    // Pure. The whole of the method dispatch and the override layering.
    [[nodiscard]] double StandingFrom(const Entry& entry, const MemberFacts& facts);
} // namespace NarrativeEngine::PlotFactionRoster
