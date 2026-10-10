#pragma once

#include <format>
#include <string_view>
#include <utility>

#include <RE/Skyrim.h>

// VisitorTravelLog — the whole of one visitor's journey, and nothing else.
//
// Writes to Data/../SKSE/NarrativeEngine_VisitorTravel.log, session-scoped
// and rotated five deep, following the GossipLog pattern. It owns its own
// stream rather than borrowing spdlog's, so nothing it writes reaches
// NarrativeEngine.log and nothing from elsewhere reaches it.
//
// Gated on `bVisitorTravelLogEnabled` ALONE and on by default. Not on
// bDebugMode, and not on bTraceMode: the point is that a plain play
// session already leaves behind enough to diagnose an arrival without
// re-running the game, and arrival bugs have repeatedly cost a full
// in-engine reproduction each because the main log recorded the verdict
// and not the reasoning.
//
// WHAT IT IS FOR, which decides what belongs in it. Every arrival defect
// found so far was a decision that looked correct in summary and wrong in
// detail: a route around the right side of a mountain whose arrival came
// off an off-route neighbour; a cover claim resting on a hill three
// thousand units from the visitor; a fine network discarded for being on
// the wrong side of the nearest node. In each case the summary line named
// the winner and not the loser, so the diagnosis took either a manual
// simulation or another play session. This file records the losers.
//
// FORMAT. One tagged line per step, in the order the decision happens, so
// a visit reads top to bottom as the search's own reasoning:
//
//   BEGIN    the sender, by name and form
//   ENDS     where each end resolved to, and which rung answered
//   GRID     the attached cell block the search may validate within
//   TIER     which tier answered, and why the others did not
//   GRAPH    every node set, attachment, bridge and laid line
//   SEARCH   the route, its cost, and the chain it produced
//   CHAIN    each point, its class, and whether it is in the grid
//   CAND     each candidate, with its distance, class and hop count
//   GATE     each rejection, naming the gate and the number that failed
//   COVER    the cover probe, ray by ray when it fails
//   GRADE    what each surviving candidate was graded
//   PICK     the winner, and what it beat
//   WARP     the marker, the anchor, and where the visitor was put
//   ESCORT   every stuck check, hop, fallback and close-in
//   END      the verdict
//
// Threading: callable from the plugin thread and the main thread. Takes no
// engine locks and reads no live RE:: pointer — callers pass positions and
// form IDs they already hold. Internally mutex-guarded, so lines from the
// chain build (plugin thread) and the warp (main thread) interleave
// coherently.
namespace NarrativeEngine::VisitorTravelLog
{
    // Registers the module and reads settings. Does NOT touch the
    // filesystem — the file lifecycle is scoped to save-game sessions.
    void Initialize();

    // kNewGame / kPostLoadGame. Rotates the previous five files and opens
    // a fresh one. No-op when logging is disabled.
    void OnSessionStart();

    // kPreLoadGame. Flushes and closes.
    void OnSessionEnd();

    // True when a file is open and lines will actually land. Call sites
    // that would do real work to build a line check this first.
    bool IsActive();

    // One line, tagged. Both are written verbatim; `tag` is padded so the
    // file stays column-readable.
    void Line(std::string_view tag, std::string_view text);

    // Formatting wrapper, which is what nearly every call site uses. The
    // check comes before the format so a disabled log costs an atomic
    // read rather than a string build.
    template <class... Args> void Write(std::string_view tag, std::format_string<Args...> fmt, Args&&... args)
    {
        if (!IsActive()) {
            return;
        }
        Line(tag, std::format(fmt, std::forward<Args>(args)...));
    }

    // --- framing ------------------------------------------------------

    // Opens a visit's transcript. Every line until `End` belongs to this
    // visitor, which is what makes the file readable without a filter:
    // only one visit beat runs at a time.
    void Begin(RE::FormID senderId, std::string_view senderName);

    // Closes it, with the verdict. `tier` and `pointClass` are the names
    // the main log uses, so the two can be cross-read.
    void End(RE::FormID senderId, std::string_view tier, std::string_view pointClass);

    // A visit that never reached a tier — no sender, no origin, a decline
    // before the search. Written instead of `End`, with the reason.
    void Abandoned(RE::FormID senderId, std::string_view reason);
} // namespace NarrativeEngine::VisitorTravelLog
