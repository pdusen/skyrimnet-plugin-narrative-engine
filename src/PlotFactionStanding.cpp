#include <PlotFactionRoster.h>

#include <algorithm>

// The pure half of the roster: method dispatch, override layering, and
// the normalisation. No file, no engine, no logging — so a probe can
// drive every method and every cross-cutting property directly.
//
// The engine-bound half (parsing PlotFactions.ini, resolving EditorIDs,
// gathering MemberFacts off the TESNPC) lives in PlotFactionRoster.cpp.
namespace NarrativeEngine::PlotFactionRoster
{
    namespace
    {
        // Normalise a rung against the top of its own ladder.
        //
        // This is the property that makes factions comparable at all:
        // being the head of a two-rung guild and the head of a
        // seven-rung college both come out at 1.0. Summing or comparing
        // raw rungs instead would make a deep hierarchy's middle
        // management outrank a shallow one's leader.
        double Normalise(int rung, int top)
        {
            if (top <= 0 || rung <= 0) {
                return 0.0;
            }
            return std::clamp(static_cast<double>(rung) / static_cast<double>(top), 0.0, 1.0);
        }
    } // namespace

    std::string_view MethodId(RankMethod m) noexcept
    {
        switch (m) {
        case RankMethod::Rank:
            return "Rank";
        case RankMethod::Marker:
            return "Marker";
        case RankMethod::Explicit:
            return "Explicit";
        }
        return "unknown";
    }

    std::string_view MembershipId(Membership m) noexcept
    {
        switch (m) {
        case Membership::Faction:
            return "Faction";
        case Membership::Ranked:
            return "Ranked";
        }
        return "unknown";
    }

    double StandingFrom(const Entry& entry, const MemberFacts& facts)
    {
        // An override wins under every method. It is layered rather than
        // alternative: `Explicit` is simply the degenerate case where
        // nothing is derived and only overrides remain, so there is one
        // code path here rather than three.
        //
        // The override's ceiling is the same ladder the method uses, so
        // a hand-written rung and a derived one mean the same thing.
        int top = 0;
        int derived = 0;

        switch (entry.method) {
        case RankMethod::Rank:
            top = std::max(1, entry.maxRank);
            derived = facts.authoredRank;
            break;

        case RankMethod::Marker: {
            // Markers are listed most-senior-first, so the rung is the
            // list read backwards: the first marker is the top.
            top = static_cast<int>(entry.markers.size());
            if (!facts.markerIndices.empty()) {
                const auto best = *std::min_element(facts.markerIndices.begin(), facts.markerIndices.end());
                if (best < entry.markers.size()) {
                    derived = top - static_cast<int>(best);
                }
            }
            break;
        }

        case RankMethod::Explicit:
            // No derivation at all; the overrides are the ladder.
            top = 0;
            derived = 0;
            break;
        }

        // The override ladder participates in the ceiling regardless of
        // method. Without this, an Explicit faction has no top to
        // normalise against, and a Rank faction whose override exceeds
        // MaxRank would clamp to 1.0 and quietly flatten the people
        // below it.
        int overridden = 0;
        bool hasOverride = false;
        for (const auto& o : entry.overrides) {
            top = std::max(top, o.rank);
            if (o.npc == facts.npc && o.npc != 0) {
                overridden = std::max(overridden, o.rank);
                hasOverride = true;
            }
        }

        return Normalise(hasOverride ? overridden : derived, top);
    }
} // namespace NarrativeEngine::PlotFactionRoster
