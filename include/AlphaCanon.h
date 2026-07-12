#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Predicates that detect when "vanilla content is in charge" — combat,
// scripted scenes, dialogue, configured do-not-disturb cells. The Director
// consults the combined bitmask each tick before deciding whether to act.
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

    // Aggregator — runs every predicate above and returns the combined
    // bitmask. Cast to std::uint32_t when storing into DecisionRecord.
    Signal EvaluateAll();

    // Names of every set bit in `mask`, in declaration order. Used for
    // prompt-context rendering (the Snapshot's `alphaCanonSignals` field)
    // and for human-readable log output.
    std::vector<std::string> Names(Signal mask);
} // namespace NarrativeEngine::AlphaCanon
