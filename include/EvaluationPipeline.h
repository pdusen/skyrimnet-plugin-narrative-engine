#pragma once

#include <DecisionLog.h>
#include <Snapshot.h>

#include <string>

// The Director's three-phase async evaluation pipeline.
//
//   Phase A (main thread, this::BuildSnapshot)        — capture game state.
//   Phase B (worker thread, this::BuildPromptContext) — assemble JSON context.
//   Phase C (worker thread, SkyrimNetAPI::SendCustomPromptToLLM)
//                                                     — fire the LLM call.
//   Phase D (main thread, this::ParseDecision + ApplyDecision)
//                                                     — parse, write log,
//                                                       advance phase.
//
// Step 9 implements Phase A in full plus a debug-mode snapshot dump.
// Phases B/C/D land in Steps 10/11/14; their declarations are present here
// so the pipeline's surface area is stable from the start.
namespace NarrativeEngine::EvaluationPipeline
{
    // Entry point — called by the tick driver on the main thread once per
    // tick interval. Atomically guards against overlapping evaluations,
    // builds the snapshot, and (Steps 10+) hands it off to the worker.
    void BeginEvaluation();

    // True between BeginEvaluation taking the inFlight flag and the eventual
    // Phase D completion releasing it.
    bool IsEvaluationInFlight();

    // Phase A — main thread. Captures every value-only field the snapshot
    // needs from RE::*, PhaseTracker, DecisionLog, SkyrimNetAPI, AlphaCanon.
    Snapshot BuildSnapshot();

    // Phase B — worker thread. Renders the snapshot into the JSON object
    // passed to SkyrimNet's prompt template. Stubbed for Step 9.
    std::string BuildPromptContext(const Snapshot& snapshot);

    // Phase D parser — worker thread. Reads the LLM's JSON response and
    // pre-fills snapshot-derived defaults so even a parse failure produces
    // a usable record. Stubbed for Step 9.
    DecisionLog::DecisionRecord ParseDecision(const std::string& jsonResponse,
                                              const Snapshot& snapshot);

    // Phase D applier — main thread. Appends to DecisionLog and applies any
    // phase advance. Stubbed for Step 9.
    void ApplyDecision(const DecisionLog::DecisionRecord& record);
}
