#pragma once

// EventHistoryWriter — Step 9 testing aid. A running, session-scoped
// history log at Data/../SKSE/NarrativeEngine_EventHistory.log that
// captures every emitted internal event (combat + weather + travel)
// plus SkyrimNet's own event stream, in chronological order, each line
// prefixed with an absolute in-game timestamp (not the "[N ago]"
// relative format the LLM sees).
//
// Session boundaries rotate the file: the current file becomes .1,
// .1 -> .2, ..., .4 -> .5, and the previous .5 is deleted. Five
// previous sessions are kept.
//
// Not a cosave subsystem. Nothing here is persisted through the SKSE
// serialization interface — the file itself is the persistence.
//
// Threading: Poll runs on the main thread from Tick. File I/O is
// synchronous inside the flush path; short bursts of events fit in
// the pending queues without back-pressure.
namespace NarrativeEngine::EventHistoryWriter
{
    // Called at kDataLoaded. Reads settings and registers the module.
    // Does NOT touch the filesystem — the file lifecycle is scoped to
    // save-game sessions, opened in OnSessionStart.
    void Initialize();

    // Called at kNewGame and kPostLoadGame — the two entry points to a
    // save-game session. Flushes and closes any file left open from a
    // prior session (defensive), rotates the previous five history
    // files, and opens a fresh current file in truncate mode.
    void OnSessionStart();

    // Called at kPreLoadGame. Drains all sources one last time,
    // flushes, and closes the file. The next OnSessionStart will
    // rotate this session's file into slot .1.
    void OnSessionEnd();

    // Main-thread poll driven by Tick's 500 ms loop when unpaused.
    // Uses the Tick-driven accumulator pattern
    // (feedback_tick_driven_accumulators) — every call adds the
    // caller-supplied unpausedElapsedSeconds to a threshold-oriented
    // accumulator. Once the accumulator crosses
    // iEventHistoryFlushIntervalSeconds, a flush runs: drain each
    // internal event log's pending history, fetch fresh SkyrimNet
    // events since the bookmark, sort the combined batch by
    // localTime, write to the file. No-op if no file is open (e.g.
    // player is on the main menu with no save loaded).
    void Poll(double unpausedElapsedSeconds);
} // namespace NarrativeEngine::EventHistoryWriter
