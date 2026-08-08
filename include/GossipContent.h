#pragma once

#include <cstdint>
#include <string>

#include <PluginThread.h>

#include <RE/Skyrim.h>

// GossipContent — what a rumor actually says.
//
// Two responsibilities, deliberately unequal in cost:
//
//   1. ONE LLM call per rumor, at seed time, producing every generation
//      band at once. Not one call per band, and emphatically not one per
//      transmission — cost scales with the number of rumors, never with
//      how far they spread. A rumor reaching eight people and one reaching
//      eighty cost exactly the same.
//
//   2. A string build per transmission: band text, plus a framing template
//      chosen by relationship and distance, plus names. No model involved.
//
// ---------------------------------------------------------------------
// Why the rumor waits
//
// SendCustomPromptToLLM is asynchronous and takes seconds. A rumor is
// therefore seeded `Pending` — claimed and recorded, with no carrier
// scheduled — and only becomes infectious when its text arrives. Because
// every band comes back together there is exactly one handshake and no
// state where some bands exist and others do not.
//
// If the call fails, or the model returns should_seed:false, the rumor is
// abandoned and its claim RELEASED. Otherwise a transient error or a single
// "not worth gossiping about" verdict would permanently burn a memory that
// never produced anything.
namespace NarrativeEngine::GossipContent
{
    void Initialize();

    // Fire the seed-time generation for a memory. Fire-and-forget: the
    // continuation runs on the plugin thread once SkyrimNet answers, and
    // calls GossipSim::SeedRumor itself with the sanitized bands.
    //
    // The caller has already claimed `sourceMemoryId`. Every failure path
    // from here — the call not queuing, an unparseable response, a
    // should_seed:false refusal, or a simulation that refuses the seed —
    // releases that claim before returning, so a memory is never burned by
    // a rumor that does not exist.
    void RequestBands(std::int64_t sourceMemoryId,
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
