#pragma once

#include <MainThread.h>
#include <TextTemplate.h>

namespace RE
{
    template <class T> class BSTEventSink;
    struct TESFastTravelEndEvent;
    class Actor;
    class TESNPC;
} // namespace RE

// Small, single-purpose wrappers around commonly-accessed engine
// singletons and derived values. Each helper's purpose:
//   * Give call sites a stable name and signature independent of
//     CommonLibSSE-NG's changing accessor shapes.
//   * Silence null-singleton edge cases at a single choke point rather
//     than re-checking `RE::Calendar::GetSingleton() != nullptr` in
//     every caller.
//
// Each function has two overloads: an untagged legacy overload for
// existing main-thread callers, and a `MainThread::Token`-taking
// overload for callers that are inside a `MainThread::Run` /
// `MainThread::FireAndForget` lambda. The token overload is the
// go-forward form; the untagged overload is retained so the phase-10
// substrate refactor doesn't touch every existing call site. Both
// overloads share the same body — the token's purpose is compile-time
// gating, not runtime state.
namespace NarrativeEngine::EngineUtils
{
    // Current game-time in hours since the calendar epoch. Returns
    // 0.0 when the Calendar singleton isn't yet available (very
    // early in plugin lifecycle). Safe to call from any thread —
    // reads a stable singleton pointer plus a bit-aligned float.
    double GetCurrentGameHours();
    double GetCurrentGameHours(const MainThread::Token&);

    // Coarse world-state gate reads used by BeatSystem's master poll
    // (and available to any other subsystem that wants the same
    // consistent notion of "is the game paused / is the player in
    // combat / is the vanilla dialogue menu open"). Each reads a
    // stable singleton pointer and a bit-aligned bool. Not officially
    // thread-safe, but CommonLibSSE-NG's off-thread usage of these
    // reads is routine and safe in practice; the BeatSystem poll
    // relies on that same guarantee.
    bool IsGamePaused();
    bool IsGamePaused(const MainThread::Token&);
    bool IsPlayerInCombat();
    bool IsPlayerInCombat(const MainThread::Token&);
    bool IsPlayerInDialogue();
    bool IsPlayerInDialogue(const MainThread::Token&);

    // Whether a base record is a ghost. Ghosts take part in no beat and
    // no simulation: several carry the display name of somebody still
    // alive, and none of them can do what a scheme, a rumor, a letter or
    // a visit asks of a person.
    //
    // True for the ACBS IsGhost flag, which vanilla sets on about seventy
    // records (Savos Aren's Ghost, Kodlak's, the barrow shades), and for
    // apparitions built without it whose display name ends in the
    // standalone word "Ghost" ("Dunmer Ghost", "Helgi's Ghost"). Matched
    // as the final word, so a Ghostblade's owner or a Ghost Sea
    // fisherman is untouched.
    //
    // Token-free, as is IsGhost below: the callers are availability
    // checks on the gossip, plot and beat workers that hold no token to
    // pass, and both reads are safe off the main thread.
    bool IsGhostRecord(const RE::TESNPC* npc);

    // A person's sex off their base record, for language-file templates that
    // agree with it. Record data, so safe from any thread; Other when there
    // is no record.
    [[nodiscard]] TextTemplate::Sex SexOf(const RE::TESNPC* npc);
    [[nodiscard]] TextTemplate::Sex SexOf(RE::FormID npc);

    // Whether a live actor is a ghost right now: its base record by the
    // rule above, or the reference's own ghost state, which a script can
    // set on a record that carries no flag at all. The engine's
    // Actor::IsGhost answers the second.
    bool IsGhost(const RE::Actor* actor);

    // Register / unregister a TESFastTravelEndEvent sink.
    //
    // These exist because `ScriptEventSourceHolder::AddEventSink<T>` is
    // NOT safe for this one event type. Skyrim VR's holder layout stops
    // one base class short of SE/AE's: it has no TESFastTravelEndEvent
    // source, so CommonLibSSE-NG's accessor returns nullptr on VR and
    // the templated AddEventSink then dereferences it. Callers must go
    // through these wrappers instead of touching the holder directly.
    // See docs/engine-findings/scripteventsourceholder-addeventsink-crashes-on-vr.md.
    //
    // Returns true when the sink was actually (un)registered — false on
    // VR, and false if the holder singleton isn't up yet. VR has no fast
    // travel at all, so the false case costs nothing but the caller
    // should not report the sink as live.
    bool AddFastTravelEndSink(RE::BSTEventSink<RE::TESFastTravelEndEvent>* sink);
    bool RemoveFastTravelEndSink(RE::BSTEventSink<RE::TESFastTravelEndEvent>* sink);
} // namespace NarrativeEngine::EngineUtils
