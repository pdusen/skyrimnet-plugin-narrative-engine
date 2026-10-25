#pragma once

#include <PlotState.h>

#include <nlohmann/json.hpp>

#include <RE/Skyrim.h>

// PlotAwareness — what somebody in a plot knows of it, for their own prompt.
//
// The plot memories say what happened, one event at a time, and only when the
// memory system thinks them relevant. This says where things stand, every
// time the person speaks or thinks: the mastermind is told their scheme and
// each step of it, done, under way or still to do; an agent is told the one
// errand they were sent on, and nothing about what it is for.
//
// What it says is a record, not prose. The `ne_plot_awareness` decorator hands
// it to SkyrimNet as JSON, which SkyrimNet parses into an object, and the
// submodule 0300_ne_plot_awareness.prompt words it in the second person,
// through components/narrative_engine/plot_awareness.prompt and the task
// phrases of plot_task.prompt. For a mastermind:
//
//   {role: "mastermind", scheme (as a phrase; may be empty), alone (bool),
//    steps: [{state, outcome, self (bool), agent, task}]}
//
// `state` is done, in_progress or todo; `outcome`, for a step that is done,
// succeeded, failed_caught or failed_timeout; `self` says the mastermind did
// it themselves, `agent` who they sent (empty when nobody is named), and
// `alone` that they have had nobody to send. For an agent:
//
//   {role: "agent", sender (may be empty), task}
//
// A `task` is {verb, fill}: the step's verb and each filled slot's text.
// Nothing in it is written by a model, so none of it needs sanitizing.
//
// Read by the `ne_plot_awareness` decorator from the published snapshot.
namespace NarrativeEngine::PlotAwareness
{
    // Pure. What `npc`, a base form like the rest of the plot code, knows of
    // the plot they are engaged in: as its mastermind, or as the agent of its
    // live step. Null for somebody in no plot, an agent between errands, and
    // a plot with no verb step to describe.
    [[nodiscard]] nlohmann::json Describe(const PlotState& state, RE::FormID npc);
} // namespace NarrativeEngine::PlotAwareness
