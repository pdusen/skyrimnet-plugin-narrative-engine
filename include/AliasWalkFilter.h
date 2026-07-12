#pragma once

#include <string>

#include <RE/Skyrim.h>

// AliasWalkFilter — shared "is this NPC currently spoken-for by some
// other quest's scripted content?" gate. Consulted during sender
// candidate viability walks (letter + visit) to drop NPCs who are
// mid-questline scene, mid-scripted-package, or otherwise being
// puppeteered by a running quest so we don't compete with that
// authored content.
//
// The signal we look at is Skyrim's own alias-fill state: a running
// quest that owns an NPC for scene purposes fills that NPC into one
// of the quest's `BGSBaseAlias` slots, and (if the alias contributes
// AI packages) the engine records those packages on the actor's
// `ExtraAliasInstanceArray` entry for that (quest, alias) pair. The
// same data is what drives the alias package stack the actor is
// actually running. So: walk the actor's `ExtraAliasInstanceArray`,
// and if any entry belongs to a foreign quest that is currently
// running AND
//   (a) the actor is mid-scene (`GetCurrentScene() != nullptr`), OR
//   (b) the alias dispenses an AI package to the actor
//       (`instancedPackages` non-empty), OR
//   (c) the alias has the `kReserves` flag set — Skyrim's alias-fill
//       machinery treats reserved fills as exclusive holds and would
//       silently skip the actor during our Sender FMR, causing
//       EnsureQuestStarted to return false with all our aliases
//       unfilled. Rejecting reserved candidates upstream avoids the
//       whole doomed dispatch.
// the actor is story-active/unavailable and should be filtered out
// of the sender candidate pool.
//
// Self-exclusion: entries belonging to NarrativeEngine's own quests
// (identified by ESP source file, so we don't need to enumerate
// every NE quest EditorID) are ignored — otherwise a sender who's
// currently mid-visit would get flagged by their own visit quest.
//
// Threading: main thread only. Reads engine state (extraList,
// quest state, alias arrays). The BSReadWriteLock on
// ExtraAliasInstanceArray IS acquired via BSReadLockGuard during
// the walk.
namespace NarrativeEngine::AliasWalkFilter
{
    // Returns true when the actor is "spoken-for" by some foreign
    // quest's scripted content and should be excluded from sender
    // candidate pools.
    //
    // On a true return, `reasonOut` (if provided) is populated with a
    // short human-readable string describing the winning signal
    // (e.g. `"story-alias:MG04Ancano/pkgs=2"`, `"scene:MG04Scene07"`).
    // This is surfaced by the SenderCandidatePool skip-summary logs
    // so we can eyeball which quest is holding an NPC hostage.
    //
    // On a false return (actor is free), `reasonOut` is left
    // untouched.
    //
    // The `debug` flag toggles the per-alias trace log lines — set to
    // true when Settings::Get().debugMode is on so we can watch the
    // walk decide, false otherwise to keep normal logs clean.
    bool IsActorStoryActive(RE::Actor* actor, std::string* reasonOut = nullptr, bool debug = false);
} // namespace NarrativeEngine::AliasWalkFilter
