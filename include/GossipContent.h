#pragma once

#include <cstdint>
#include <string>

#include <PluginThread.h>

#include <RE/Skyrim.h>

// GossipContent — what a rumor actually says.
//
// Two responsibilities, deliberately unequal in cost:
//
//   1. AT MOST TWO LLM calls per candidate memory, at seed time — an
//      evaluation, and then the band generation only if the evaluation
//      approves. Never one call per band, and emphatically not one per
//      transmission: cost scales with the number of candidates, never with
//      how far a rumor spreads. A rumor reaching eight people and one
//      reaching eighty cost exactly the same.
//
//   2. A string build per transmission: band text, plus a framing template
//      chosen by relationship and distance, plus names. No model involved.
//
// ---------------------------------------------------------------------
// Why two calls and not one
//
// The two halves are different jobs wanting different models. Deciding
// whether a memory should become gossip is classification — read a
// character profile, compare against the circulating rumors, return one
// word from a fixed set — and runs on the cheap director variant. Writing
// the generation bands is creative writing in a specific register, and
// runs on the composer variant the letter beat uses.
//
// Combining them meant the expensive model was answering "no" for the
// majority of candidates, since `private`, `not_worthy` and `duplicate`
// together outnumber `seed`. Splitting them means a refusal costs a
// director call and nothing else.
//
// The second call gets a deliberately narrower context: no character
// profile, no active-rumor list, no importance tier. Those exist to support
// judgements already made, and re-sending them would be paying the
// composer to reconsider a decision its prompt tells it not to revisit.
//
// ---------------------------------------------------------------------
// Why the rumor waits
//
// SendCustomPromptToLLM is asynchronous and takes seconds, and there are
// now two of them chained — the second is fired from the first's callback,
// on the plugin thread. A rumor is therefore seeded `Pending` — claimed and
// recorded, with no carrier scheduled — and only becomes infectious when
// its text arrives. Because every band comes back together there is exactly
// one handshake and no state where some bands exist and others do not.
//
// If either call fails, the rumor is abandoned and its claim RELEASED.
// Otherwise a transient error would permanently burn a memory that never
// produced anything. A refusal is different from a failure and is handled
// differently — see RequestRumor.
namespace NarrativeEngine::GossipContent
{
    void Initialize();

    // Evaluate a memory and, if it passes, generate the rumor from it.
    // Fire-and-forget: both continuations run on the plugin thread once
    // SkyrimNet answers, and the last of them calls GossipSim::SeedRumor
    // itself with the sanitized bands.
    //
    // The caller has already claimed `sourceMemoryId` and its related
    // events. What happens to those claims depends on WHY no rumor
    // resulted, and the distinction matters:
    //
    //   * Nothing was learned — a call failed to queue, failed outright,
    //     returned something unparseable, carried an unrecognised verdict,
    //     or the simulation refused the seed. Everything is released, so
    //     the memory can be tried again on a later sweep.
    //
    //   * `private` / `not_worthy` — a judgement about THIS owner. The
    //     memory stays claimed so they are not asked again; the events are
    //     released so another witness's account of the same happening can
    //     still seed.
    //
    //   * `duplicate` — a judgement about the happening. Memory and events
    //     both stay claimed, so no second rumor about it can start.
    void RequestRumor(std::int64_t sourceMemoryId,
                      RE::FormID owner,
                      const std::string& sourceText,
                      const std::string& locationName,
                      float importance);

    // Band index for a carrier at `generation`, clamped to the configured
    // band count. Edges are every three generations: 0-2, 3-5, 6+.
    std::size_t BandForGeneration(std::uint32_t generation);

    // The two memories a transmission writes, composed from band text plus
    // relationship-aware framing. No LLM.
    //
    // Kinship terms come from BGSAssociationType::associationLabels, which
    // supplies gendered labels ("sister", "cousin") straight from the
    // record rather than inventing them.
    struct ComposedPair
    {
        std::string tellerText;
        std::string listenerText;
    };
    ComposedPair Compose(const std::string& bandText, RE::FormID teller, RE::FormID listener);
} // namespace NarrativeEngine::GossipContent
