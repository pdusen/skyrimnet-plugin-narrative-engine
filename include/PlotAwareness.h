#pragma once

#include <PlotState.h>

#include <string>

#include <RE/Skyrim.h>

// PlotAwareness — what somebody in a plot knows of it, for their own prompt.
//
// The plot memories say what happened, one event at a time, and only when the
// memory system thinks them relevant. This says where things stand, every
// time the person speaks or thinks: the mastermind is told their scheme and
// each step of it, done, under way or still to do; an agent is told the one
// errand they were sent on, and nothing about what it is for.
//
// Second person, because it is rendered into the person's own instructions.
// Every phrase comes from the verb table (PlotVerbs' two task templates), so
// nothing here is written by a model and none of it needs sanitizing.
//
// Read by the `ne_plot_awareness` decorator from the published snapshot.
namespace NarrativeEngine::PlotAwareness
{
    // Pure. What `npc`, a base form like the rest of the plot code, knows of
    // the plot they are engaged in: as its mastermind, or as the agent of its
    // live step. Empty for somebody in no plot, an agent between errands, and
    // a plot with no verb step to describe.
    [[nodiscard]] std::string Describe(const PlotState& state, RE::FormID npc);
} // namespace NarrativeEngine::PlotAwareness
