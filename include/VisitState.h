#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <RE/Skyrim.h>

namespace SKSE { class SerializationInterface; }

// VisitState — single-slot data module for the in-flight NPCVisitAction.
//
// The state-machine phase is NOT stored here; it's carried on the stages of
// `_ne_VisitQuest`, which the engine auto-persists in the vanilla save.
// VisitState only owns the residual data the C++ side needs that isn't
// recoverable from the quest / aliases / SkyrimNet — briefing text,
// return-teleport bookkeeping, and a few in-flight counters — plus a
// per-process recent-history ring for the dashboard.
//
// The `DerivePhase()` helper reads the quest's current stage (plus the
// in-process `g_composingSender` flag) and returns a `Mode` enum for
// logging / dashboard convenience. It is not stored.
//
// Threading: all accessors are guarded by an internal mutex. Co-save
// callbacks fire off-thread; snapshot mutation from the main thread and
// the dashboard's per-tick reads both go through the same lock.
namespace NarrativeEngine::VisitState
{
    // SKSE co-save record type ID — 'NEVS' = NarrativeEngine Visit State.
    // Frozen; changing it would orphan any previously-saved snapshot.
    inline constexpr std::uint32_t kRecordTypeId = 'NEVS';

    // Derived state — reflects the current quest stage (+ the composing
    // flag). Not stored anywhere; recomputed from live state each time.
    enum class Mode : std::uint8_t
    {
        Idle,
        Composing,     // compose LLM call is in flight; quest not yet started
        Salutation,    // quest stage 10
        Discuss,       // quest stage 20
        OnHold,        // quest stage 25
        ReEngage,      // quest stage 27
        Valediction,   // quest stage 30
        ReturnHome,    // quest stage 50
    };

    // Reason a terminal history entry was recorded, for the dashboard.
    enum class Outcome : std::uint8_t
    {
        Completed,     // Discuss reached a natural conclusion via the poll
        Unsatisfied,   // sender left because the player ignored them
        RolledBack,    // Salutation timeout; beat never really started
        Aborted,       // hard-abort branch fired (death or outer timeout)
    };

    // The persisted data — cached briefing, return-teleport bookkeeping,
    // and running counters. Populated in Start's callback; cleared on
    // Reset().
    struct Snapshot
    {
        RE::FormID              senderFormID          = 0;
        RE::FormID              returnCellFormID      = 0;
        RE::NiPoint3            returnPosition        {};
        float                   returnAngleZ          = 0.0f;
        RE::FormID              returnAnchorFormID    = 0;

        std::string             briefingText;
        std::string             narrationText;
        std::string             topicTag;
        std::string             mood;
        std::vector<std::string> tags;

        // Wall-clock start time of the whole visit lifecycle (drives the
        // outer hard-timeout guard).
        double                  dispatchedAtRealSeconds = 0.0;

        // Consecutive `ContinueConversation` fires without a poll ever
        // returning `should_conclude: true` in between. Resets whenever a
        // fresh player speech turn lands.
        std::uint8_t            ignoreNudgeCount      = 0;

        // Consecutive natural-conclusion-poll failures (parse errors, LLM
        // timeouts). Hits `iVisitConclusionPollMaxConsecutiveFailures` →
        // hard-abort.
        std::uint8_t            consecutivePollFailures = 0;
    };

    // Dashboard's recent-history ring buffer entry. Per-process; not
    // persisted (dashboard-only convenience).
    struct HistoryEntry
    {
        double      dispatchedAt   = 0.0;
        std::string senderName;
        std::string topicTag;
        Outcome     outcome        = Outcome::Completed;
        double      durationSeconds = 0.0;
    };

    // Fixed size for the history ring — same order of magnitude as
    // Phase 03's per-action recency ring.
    inline constexpr std::size_t kHistoryRingSize = 10;

    // In-process flag signalling the Composing pseudo-state — the compose
    // LLM call is in flight but the quest hasn't been started yet. Set at
    // the top of NPCVisitAction::Start; cleared by the compose callback
    // once `EnsureQuestStarted` has returned. Read by DerivePhase() and
    // the re-entrancy guard in Start. Storage lives in the .cpp.
    void SetComposingSender(bool value);
    bool GetComposingSender();

    // Snapshot accessors — thread-safe, copy in / copy out.
    Snapshot  GetSnapshot();
    void      SetSnapshot(const Snapshot& snapshot);
    void      Reset();

    // Recent-history ring for the dashboard.
    void                       PushHistory(HistoryEntry entry);
    std::vector<HistoryEntry>  GetHistory();

    // Derive the current mode from live state — reads the composing flag
    // and the current stage of `_ne_VisitQuest` (looked up lazily by
    // EditorID). Returns `Idle` when the quest isn't resolved yet (e.g.
    // Phase 05 CK content hasn't been authored), so this is safe to call
    // before Step 7's CK authoring lands.
    Mode DerivePhase();

    // Co-save handlers.
    void OnSave(SKSE::SerializationInterface*);
    void OnLoad(SKSE::SerializationInterface*, std::uint32_t version, std::uint32_t length);
    void OnRevert();
}
