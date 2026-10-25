#pragma once

// Custom SkyrimNet Inja decorators that expose the Director's narrative
// state, and each NPC's part in a plot, to their LLM context.
//
// Three decorators register at plugin startup:
//
//   ne_narrative_tension - returns the most recent decision's tension
//                          score as a decimal string ("0".."100").
//                          Pre-first-evaluation it is "0" (calm default).
//   ne_narrative_phase   - returns the current Freytag-pyramid phase name
//                          ("Exposition" | "RisingAction" | "Climax" |
//                          "FallingAction" | "Resolution").
//
// Both decorators ignore the RE::Actor* SkyrimNet passes them — the
// Director's narrative state is a global property of the world, not a
// per-NPC fact. The decorator callbacks are thread-safe because the
// underlying PhaseTracker and DecisionLog getters take their own
// internal mutexes; SkyrimNet calls these from its prompt-rendering
// thread, which may not be the main thread.
//
// The decorators are consumed by the shipped `system_head` submodule
// template `0500_ne_narrative_state.prompt`, which maps the numeric
// tension and the phase name to a qualitative one-sentence "world mood"
// paragraph that gets injected at the head of every assembled bio.
//
//   ne_plot_awareness    - returns what this actor knows of the plot they
//                          are in, as a mastermind or an agent, in the
//                          second person; empty for anybody else. See
//                          PlotAwareness. Unlike the two above it reads the
//                          actor, by base form, and the published plot
//                          snapshot, which needs no lock.
//
// It is consumed by the `user_final_instructions` submodule
// `0300_ne_plot_awareness.prompt`.
namespace NarrativeEngine::Decorators
{
    // Register every decorator with SkyrimNet. Idempotent — SkyrimNet
    // rejects duplicate names, so calling twice is harmless. No-op (with
    // warning) if SkyrimNet is unavailable.
    //
    // Call once from kDataLoaded, after SkyrimNetAPI::Initialize().
    void Register();
} // namespace NarrativeEngine::Decorators
