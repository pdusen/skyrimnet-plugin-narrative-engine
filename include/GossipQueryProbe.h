#pragma once

#include <PluginThread.h>

// GossipQueryProbe — THROWAWAY DIAGNOSTIC. DELETE WHEN THE ANSWER IS IN.
//
// One question, asked once per session: can `contextQuery` on
// PublicGetMemoriesForActor be made to return an actor's real memories
// while leaving our own gossip writebacks behind?
//
// PublicAPI.h documents the argument as:
//
//   "If non-empty, performs semantic (vector) search ranked by
//    relevance. If empty, returns most recent first."
//
// We pass empty today, which is why the harvest starves: gossip
// writebacks are always the most recent rows, so a ten-row window fills
// with our own output within three or four of an actor's turns.
//
// Whether a non-empty query fixes that — and which phrasing works — is
// not answerable from the documentation. The recall docs describe a
// six-signal weighted score (semantic 0.35, temporal 0.20, actor 0.20,
// emotional 0.10, keyword 0.10, location 0.05), but do not say whether
// this endpoint runs that scorer or something simpler, and SkyrimNet's
// source is not available. So: measure it.
//
// The probe fires once, a few seconds after a save loads, runs every
// query variant below against one actor, and writes a table to the
// plugin log. Grep `QUERYPROBE`.
//
// To remove: delete this file and its .cpp, then the two call sites in
// Plugin.cpp (OnSessionStart) and Tick.cpp (Poll). Nothing else refers
// to it.
namespace NarrativeEngine::GossipQueryProbe
{
    // Re-arms the probe. It fires once per loaded save.
    void OnSessionStart();

    // Plugin thread. Counts down, then hands the actual work to
    // GossipDispatch — every variant is a blocking round trip into
    // SkyrimNet's vector index, which has no business on this thread.
    void Poll(const PluginThread::Token&, double unpausedElapsedSeconds);
} // namespace NarrativeEngine::GossipQueryProbe
