#pragma once

// CharacterIndexSmokeTest — does prose retrieval actually find the right
// people, in a real save, over the real bio corpus?
//
// THROWAWAY. This exists to answer one question and should be deleted
// once it has: the offline harness that motivated CharacterIndex ran
// over the STAE bio files on disk with a Python BM25, and agreed with
// itself. What it could not check is whether the pieces line up inside
// a running game — whether SkyrimNet hands back a template name for the
// population plots actually draw on, whether those files are where the
// virtual filesystem says they are, and whether the rankings survive
// the trip.
//
// It runs once per loaded session, off the main thread, reads every
// bio it can resolve, and writes a few dozen fixed queries and their
// top matches to the plugin log. It changes no state and nothing reads
// its output but a human.
namespace NarrativeEngine::CharacterIndexSmokeTest
{
    // Call at kPostLoadGame / kNewGame, after PlotPopulation::Build and
    // PlotPopulation::OnSessionStart. Returns immediately; the work is
    // queued onto the plugin thread, because it is several hundred file
    // reads and has no business on the main thread.
    //
    // Runs at most once per game session, and unconditionally: there is
    // no setting to turn it on, because it is not meant to outlive the
    // question it answers. If it is still here when this stops being a
    // question, delete it rather than adding a switch.
    void OnSessionStart();
} // namespace NarrativeEngine::CharacterIndexSmokeTest
