#pragma once

#include <DecisionLog.h>
#include <PluginThread.h>
#include <Snapshot.h>

#include <string>

// The Director's async evaluation pipeline.
//
//   Phases A + B + C (plugin thread, BeginEvaluation body)
//     — capture game state (with a single MainThread::Run hop for
//       the specific engine reads), build the JSON prompt context,
//       fire the LLM call.
//   Phase D (main thread, ParseDecision + ApplyDecision via BeatSystem's
//           ConsiderBeat finalize chain)
//     — parse, write log, advance phase. Still main-thread today
//       because BeatSystem's downstream chain lives there; the audit-
//       fix follow-on migration of BeatSystem will move it.
namespace NarrativeEngine::EvaluationPipeline
{
    // Entry point — called by the tick driver on the plugin thread
    // once per tick interval. Atomically guards against overlapping
    // evaluations, builds the snapshot (hopping to main for the
    // specific engine reads), assembles the prompt context, and fires
    // the LLM call.
    void BeginEvaluation(const PluginThread::Token&);

    // True between BeginEvaluation taking the inFlight flag and the eventual
    // Phase D completion releasing it.
    bool IsEvaluationInFlight();

    // Phase A — main-thread overload. Captures every value-only field
    // the snapshot needs from RE::*, PhaseTracker, DecisionLog,
    // SkyrimNetAPI, AlphaCanon. Called by BeatSystem's StartBeat /
    // ForceDispatchBeat, which run on the main thread today.
    Snapshot BuildSnapshot();

    // Phase A — plugin-thread overload. Same snapshot shape, but
    // orchestrated for a plugin-thread caller: the thread-safe reads
    // (PhaseTracker, DecisionLog, SkyrimNetAPI DLL fetch) run inline
    // on the plugin thread, and a single MainThread::Run hop covers
    // the engine reads (player + calendar + AlphaCanon).
    Snapshot BuildSnapshot(const PluginThread::Token&);

    // Phase B — worker thread. Renders the snapshot into the JSON object
    // passed to SkyrimNet's prompt template. Stubbed for Step 9.
    std::string BuildPromptContext(const Snapshot& snapshot);

    // Phase D parser — worker thread. Reads the LLM's JSON response and
    // pre-fills snapshot-derived defaults so even a parse failure produces
    // a usable record. Stubbed for Step 9.
    DecisionLog::DecisionRecord ParseDecision(const std::string& jsonResponse, const Snapshot& snapshot);

    // Phase D applier — main thread. Appends to DecisionLog and applies any
    // phase advance. Stubbed for Step 9.
    void ApplyDecision(const DecisionLog::DecisionRecord& record);

    // Strip leading/trailing whitespace and a wrapping markdown code fence
    // (```json ... ``` or ``` ... ```) if present. LLMs sometimes wrap their
    // JSON output despite a "no fences" instruction; this is a small
    // best-effort tolerance so a fenced response still parses. Exposed for
    // BeatSystem and any other LLM consumer.
    std::string StripMarkdownFences(const std::string& input);
} // namespace NarrativeEngine::EvaluationPipeline
