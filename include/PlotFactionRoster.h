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

        // Rank: the top of the ladder, so standing normalises against it.
        int maxRank = 1;

        // Marker: descending seniority, most senior first.
        std::vector<RE::FormID> markers;

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
        int maxRank = 1;
        std::vector<std::string> markerEditorIds;
        std::vector<RawOverride> overrides;
    };

    struct ParseReport
    {
        std::vector<RawEntry> entries;
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
