#pragma once

#include <DecisionLog.h>
#include <PluginThread.h>
#include <Snapshot.h>

#include <string>

// The Director's per-tick evaluation pipeline. Entry point runs on
// the plugin thread; single MainThread::Run hop for the engine reads
// during snapshot; the sync LLM round-trip blocks the calling plugin
// thread; ApplyDecision hands the compose-side push off to
// AsyncDispatch.
namespace NarrativeEngine::EvaluationPipeline
{
    // Called by the tick driver once per tick interval. Guards against
    // overlapping evaluations via IsEvaluationInFlight.
    void BeginEvaluation(const PluginThread::Token&);

    // True from BeginEvaluation's in-flight take until ApplyDecision's
    // release.
    bool IsEvaluationInFlight();

    // Main-thread overload — for callers already on main
    // (BeatSystem::ForceDispatchBeat).
    Snapshot BuildSnapshot();

    // Plugin-thread overload — bundles the engine reads into a single
    // MainThread::Run hop.
    Snapshot BuildSnapshot(const PluginThread::Token&);

    std::string BuildPromptContext(const Snapshot& snapshot);

    // Pre-fills snapshot-derived defaults so even a parse failure
    // produces a usable record.
    DecisionLog::DecisionRecord ParseDecision(const std::string& jsonResponse, const Snapshot& snapshot);

    // Plugin thread. Appends to DecisionLog, applies phase advance,
    // pushes dashboard state.
    void ApplyDecision(const PluginThread::Token&, const DecisionLog::DecisionRecord& record);

    // Strip leading/trailing whitespace and a wrapping markdown code
    // fence (```json ... ``` or ``` ... ```) — best-effort tolerance
    // when LLMs wrap their JSON output despite instructions not to.
    std::string StripMarkdownFences(const std::string& input);
} // namespace NarrativeEngine::EvaluationPipeline
