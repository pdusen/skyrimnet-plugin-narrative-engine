#pragma once

// Custom SkyrimNet Inja decorators that expose the Director's narrative
// state to every NPC's LLM context.
//
// Two decorators register at plugin startup:
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
namespace NarrativeEngine::Decorators
{
    // Register both decorators with SkyrimNet. Idempotent — SkyrimNet
    // rejects duplicate names, so calling twice is harmless. No-op (with
    // warning) if SkyrimNet is unavailable.
    //
    // Call once from kDataLoaded, after SkyrimNetAPI::Initialize().
    void Register();
}
