#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Predicates that detect when "vanilla content is in charge" — combat,
// scripted scenes, dialogue, configured do-not-disturb cells, configured
// blacklisted Locations. The Director consults the combined bitmask each
// tick before deciding whether to act.
//
// All predicates here read live engine state via RE::* APIs and must
// therefore be called on the main thread. The snapshot builder (Step 9) is
// the intended caller.
namespace NarrativeEngine::AlphaCanon
{
    enum class Signal : std::uint32_t
    {
        None = 0,
        InActiveCombat = 1u << 0,
        InScriptedScene = 1u << 1,
        InDialogue = 1u << 2,
        InDoNotDisturbCell = 1u << 3,
        InBlacklistedLocation = 1u << 4,
    };

    constexpr Signal operator|(Signal a, Signal b)
    {
        return Signal(std::uint32_t(a) | std::uint32_t(b));
    }
    constexpr Signal operator&(Signal a, Signal b)
    {
        return Signal(std::uint32_t(a) & std::uint32_t(b));
    }
    constexpr Signal& operator|=(Signal& a, Signal b)
    {
        a = a | b;
        return a;
    }
    constexpr bool HasAny(Signal mask)
    {
        return std::uint32_t(mask) != 0;
    }
    constexpr bool HasFlag(Signal mask, Signal flag)
    {
        return (std::uint32_t(mask) & std::uint32_t(flag)) != 0;
    }

    // Individual predicates. Cheap to call.
    bool IsInActiveCombat();
    bool IsInDialogue();
    bool IsInScriptedScene();
    bool IsInDoNotDisturbCell();

    // True when the player's current Location — or any ancestor reached
    // via BGSLocation::parentLoc — has an EditorID named in
    // Settings::Config::blacklistedLocationEDIDsCSV. Because the whole
    // ancestor chain is tested, blacklisting a parent (e.g.
    // SovngardeLocation) covers every child Location under it (e.g.
    // SovngardeHallofHeroesLocation).
    //
    // Location EditorIDs are only retained at runtime with an
    // EditorID-recovery mod installed (powerofthree's Tweaks or
    // equivalent) — the same dependency LocationKeywords already carries.
    // Without one this predicate is always false and the blacklist
    // degrades open rather than blocking everywhere.
    bool IsInBlacklistedLocation();

    // Aggregator — runs every predicate above and returns the combined
    // bitmask. Cast to std::uint32_t when storing into DecisionRecord.
    Signal EvaluateAll();

    // Names of every set bit in `mask`, in declaration order. Used for
    // prompt-context rendering (the Snapshot's `alphaCanonSignals` field)
    // and for human-readable log output.
    std::vector<std::string> Names(Signal mask);
} // namespace NarrativeEngine::AlphaCanon
