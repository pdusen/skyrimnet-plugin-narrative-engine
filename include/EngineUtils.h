#pragma once

// Small, single-purpose wrappers around commonly-accessed engine
// singletons and derived values. Each helper's purpose:
//   * Give call sites a stable name and signature independent of
//     CommonLibSSE-NG's changing accessor shapes.
//   * Silence null-singleton edge cases at a single choke point rather
//     than re-checking `RE::Calendar::GetSingleton() != nullptr` in
//     every caller.
namespace NarrativeEngine::EngineUtils
{
    // Current game-time in hours since the calendar epoch. Returns
    // 0.0 when the Calendar singleton isn't yet available (very
    // early in plugin lifecycle). Safe to call from any thread —
    // reads a stable singleton pointer plus a bit-aligned float.
    double GetCurrentGameHours();

    // Coarse world-state gate reads used by BeatSystem's master poll
    // (and available to any other subsystem that wants the same
    // consistent notion of "is the game paused / is the player in
    // combat / is the vanilla dialogue menu open"). Each reads a
    // stable singleton pointer and a bit-aligned bool. Not officially
    // thread-safe, but CommonLibSSE-NG's off-thread usage of these
    // reads is routine and safe in practice; the BeatSystem poll
    // relies on that same guarantee.
    bool IsGamePaused();
    bool IsPlayerInCombat();
    bool IsPlayerInDialogue();
} // namespace NarrativeEngine::EngineUtils
