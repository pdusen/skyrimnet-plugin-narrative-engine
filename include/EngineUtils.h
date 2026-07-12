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
}
