#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <PlotModel.h>

// PlotLog — a dedicated trace of the plot simulation, and nothing else.
//
// Writes to Data/../SKSE/NarrativeEngine_Plots.log, session-scoped and
// rotated five deep, on GossipLog's precedent. It owns its own
// std::ofstream rather than borrowing the default logger, so nothing it
// writes reaches NarrativeEngine.log and nothing from elsewhere reaches
// it.
//
// Gated on bPlotLogEnabled ALONE — deliberately not on bDebugMode. The
// whole point is to run a long validation session with a quiet main log
// and a complete plot trace.
//
// ---------------------------------------------------------------------
// The format is meant to be ANALYSABLE, not merely verbose
//
// Every line begins with an uppercase tag so one kind of event can be
// grepped out of a long run, and a whole session's step outcomes can be
// counted from the RESOLVE lines alone. That property is asserted rather
// than hoped for: the step's probe parses a trace back and checks the
// counts against what the simulation itself recorded.
//
// DISPATCH carries the four numbers that SIZED the step — travel,
// importance, competence, suitability — because without them a later
// failure says what happened and not what it was ever up against.
// ROLL carries progress against threshold every tick, so "it failed at
// 0.62 of its threshold, having been held for four of its nine ticks" is
// recoverable from the file rather than only from the dashboard.
//
// ---------------------------------------------------------------------
// Threading
//
// Every emitter is callable from the plot worker and takes no engine
// locks: names come from the cached strings on the Plot and Step, never
// from a live RE:: pointer. Internally mutex-guarded, so ordering stays
// consistent even though one worker drives the simulation today.
//
// Every line is FLUSHED AS IT IS WRITTEN. From Step 16 a plot tick
// blocks on LLM round trips, and anything less than per-line flushing
// leaves the trace silent for seconds at a time and then arriving in a
// burst — with the file's tail sitting on a half-written line while it
// waits. Gossip shipped that mistake once; see the note in
// GossipLog.cpp.
namespace NarrativeEngine::PlotLog
{
    // Registers the module and reads settings. Does NOT touch the
    // filesystem — the file lifecycle is scoped to save-game sessions.
    void Initialize();

    // kNewGame / kPostLoadGame. Rotates the previous five files and
    // opens a fresh one. No-op when logging is disabled.
    void OnSessionStart();

    // kPreLoadGame. Writes the closing census, flushes, closes.
    void OnSessionEnd();

    [[nodiscard]] bool IsOpen();

    // --- Emitters ------------------------------------------------------

    void Tick(std::uint32_t tickNumber, double gameDay, std::size_t activePlots, std::size_t budget);

    void Born(const PlotModel::Plot& plot, double weight, std::size_t candidatesConsidered);

    // `rung` is which rung of the agent ladder the actor came from; the
    // rejects are how "the same six NPCs do everything" becomes visible
    // rather than merely suspected.
    void Dispatch(const PlotModel::Plot& plot,
                  const PlotModel::Step& step,
                  int rung,
                  std::size_t considered,
                  std::size_t rejected);

    void Roll(const PlotModel::Plot& plot, const PlotModel::Step& step, const PlotModel::TickRecord& record);

    void Resolve(const PlotModel::Plot& plot, const PlotModel::Step& step);

    void Adapt(const PlotModel::Plot& plot, int adaptations, int cap, std::size_t newTailLength);

    void End(const PlotModel::Plot& plot);

    void Reap(std::size_t plots, std::size_t occupancyRows, double gameDay);

    // A birth that got as far as asking and came back unusable.
    //
    // Logged because the alternative is silence: a rejected plot and a
    // tick where nobody was eligible look identical in the trace, and
    // one of them means the prompt needs work.
    void BirthRejected(const std::string& mastermind, const std::string& reason);

    // A tick that had a free slot and produced no plot WITHOUT ever
    // asking the model.
    //
    // This line exists because its absence was mistaken for a bug. A
    // tick declined to birth, made no LLM call, wrote no REJECT, and
    // looked from the log exactly like a tick that had never tried --
    // there was no way to tell "nobody was eligible" from "the birth
    // rule said no" from "something is broken", because all three were
    // the same silence.
    //
    // `drawn` and `castable` are the two numbers that separate the
    // cases: a drawn of 0 is the weighted draw finding nobody eligible,
    // and a castable of 0 out of a healthy drawn is SkyrimNet resolving
    // none of them to a profile.
    void BirthSkipped(const std::string& reason, std::size_t drawn, std::size_t castable);

    // A plot the mastermind gave up on, and why.
    void Concede(const PlotModel::Plot& plot, const std::string& reason);

    void Note(std::string_view text);

    // --- Line formatting, pure ----------------------------------------
    //
    // Separated from the file so the format can be probed. Every emitter
    // above is "format, then write one line", and the formatting is the
    // half worth asserting on.

    [[nodiscard]] std::string FormatTick(std::uint32_t tickNumber,
                                         double gameDay,
                                         std::size_t activePlots,
                                         std::size_t budget);
    [[nodiscard]] std::string FormatBorn(const PlotModel::Plot& plot, double weight, std::size_t considered);
    [[nodiscard]] std::string FormatDispatch(const PlotModel::Plot& plot,
                                             const PlotModel::Step& step,
                                             int rung,
                                             std::size_t considered,
                                             std::size_t rejected);
    [[nodiscard]] std::string FormatRoll(const PlotModel::Plot& plot,
                                         const PlotModel::Step& step,
                                         const PlotModel::TickRecord& record);
    [[nodiscard]] std::string FormatResolve(const PlotModel::Plot& plot, const PlotModel::Step& step);
    [[nodiscard]] std::string FormatAdapt(const PlotModel::Plot& plot, int adaptations, int cap, std::size_t tail);
    [[nodiscard]] std::string FormatEnd(const PlotModel::Plot& plot);
} // namespace NarrativeEngine::PlotLog
