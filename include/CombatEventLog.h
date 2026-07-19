#pragma once

#include <EventLogUtil.h>

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <vector>

namespace SKSE
{
    class SerializationInterface;
}

// NarrativeEngine-owned combat event source. Fills the gap SkyrimNet's event
// log leaves around moment-to-moment combat by hooking SKSE's combat,
// hit, and bleedout events, storing them in a small in-process log, pruning
// on phase boundaries, and exposing a rendered tail the prompt + dashboard
// merge into the existing `recent_events` stream.
//
// Five internal event kinds: combat_start / combat_end (player only),
// hit (any actor in range, including environmental damage sources),
// collapse / regain_footing (any nearby actor entering / leaving bleedout).
//
// Threading: SKSE event sinks fire on non-main threads; the bleedout
// recovery poll and the prune-on-phase-change call run on the main thread.
// A single internal mutex protects every piece of state.
namespace NarrativeEngine::CombatEventLog
{
    // SKSE co-save record type ID. Frozen — changing it orphans every
    // previously-saved CombatEventLog payload.
    inline constexpr std::uint32_t kRecordTypeId = 'NECE';

    // Register the combat-state, hit, and bleedout event sinks. Idempotent.
    // Call at SKSE's kDataLoaded.
    void Initialize();

    // Best-effort sink deregistration. Engine usually outlives us so this
    // is mostly cosmetic.
    void Shutdown();

    // Called by PhaseTracker after AdvanceTo / Reset commits the new phase.
    // Drops every event older than the current encounter's start (if the
    // player is currently in combat) or wipes the entire log (if not).
    void OnPhaseAdvanced();

    // Main-thread poll driven by the Tick driver. Does two things:
    //   1. Detects player combat-state changes (IsInCombat flips) and emits
    //      combat_start / combat_end events. Done by polling rather than via
    //      TESCombatEvent because that event does not reliably fire for the
    //      player as `actor` — hits and bleedout fire fine, but player-side
    //      combat-state changes can be missed entirely.
    //   2. Walks the "currently bleeding out" actor set; emits regain_footing
    //      for any that recovered within the distance gate, drops silently
    //      for any that died, despawned, or recovered out of range.
    void Poll();

    // Returns a JSON array of currently-retained internal events shaped
    // like SkyrimNet events:
    //   { type, localTime, gameTime, originatingActorName, targetActorName,
    //     text, ne_kind }
    // Oldest-first. `text` is pre-rendered with the same "[N ago]"
    // relative-game-time prefix FormatEventsText produces so the merger can
    // interleave with SkyrimNet events transparently. The `ne_kind` string
    // ("hit", "collapse", etc.) lets the merger tell internal events apart
    // from SkyrimNet's without re-parsing `type`.
    nlohmann::json GetRenderedTail(double currentGameTimeSeconds);

    // After a save loads, rebuild the in-memory bleedingOut set by scanning
    // high-process actors. Call from kPostLoadGame.
    void OnPostLoadGame();

    // SKSE co-save persistence. OnSave prunes-then-writes so on-disk content
    // matches the in-memory rules.
    void OnSave(SKSE::SerializationInterface* intfc);
    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
    void OnRevert();

    // Drain the session-only pending history queue for the
    // EventHistoryWriter. See WeatherEventLog::DrainHistoryTail for
    // shape / semantics — identical contract. hit / collapse /
    // regain_footing / combat_start / combat_end all get one entry
    // each; no condensation on this path (the history log is the
    // "raw record," not the LLM-facing tail).
    std::vector<EventLogUtil::HistoryEntry> DrainHistoryTail();
} // namespace NarrativeEngine::CombatEventLog
