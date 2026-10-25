#pragma once

#include <PlotThread.h>

#include <cstdint>
#include <string_view>

// Two premade plots at the College of Winterhold, seeded from the dashboard,
// so what plots put into NPCs' prompts and behaviour can be checked against a
// known plot, with people the player can stand next to. A debug tool: the
// Plots tab shows its buttons only with bPlotDebugButtons on.
//
//   "brelyna"  Brelyna Maryon masterminds a plot against J'zargo, its steps
//              carried out by other members of the College.
//   "tolfdir"  Tolfdir masterminds a plot against Ancano, and Brelyna is the
//              agent of the step under way.
//
// Names and forms are literals from Skyrim.esm, so nothing here touches the
// engine. Seeding either first removes any test plot already there, so the
// two switch between each other. Either refuses, with a warning, when the
// budget is full or when anybody it would engage is already in a real plot:
// one NPC, one engagement.
namespace NarrativeEngine::PlotTestPlots
{
    // The new plot's id, or 0 if it was refused or `which` names neither.
    std::uint32_t Seed(const PlotThread::Token&, std::string_view which);
} // namespace NarrativeEngine::PlotTestPlots
