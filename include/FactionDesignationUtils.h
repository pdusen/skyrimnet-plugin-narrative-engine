#pragma once

#include <cstdint>
#include <string_view>

namespace RE
{
    class Actor;
    class TESFaction;
} // namespace RE

// FactionDesignationUtils — the rank-based "designate one loaded actor
// at a time" pattern shared by letter and visit beats. Both plant a
// marker faction whose sole purpose is to steer a Find-Matching-
// Reference alias-fill toward the beat's chosen sender: the beat
// promotes exactly one loaded actor to rank `designatedRank`, sweeps any
// stragglers from prior dispatches back to `candidateRank`, and the
// alias's `GetFactionRank marker >= designatedRank` condition binds to
// the promoted actor. This is not normal faction / alliance logic —
// the faction is used as a one-actor-at-a-time selector, and the beat
// demotes back to `candidateRank` on every terminal path.
//
// The helpers here take rank values as parameters rather than baking in
// specific numbers so a future beat with a different rank scheme can
// reuse them without modification. `logTag` prefixes each log line so a
// caller (letter beat vs. visit beat) is grep-able from the log output.
namespace NarrativeEngine::FactionDesignationUtils
{
    // Sweep every loaded actor at rank >= `designatedRank` in `fact`
    // (except `target`) down to `candidateRank`. Used to clear stale
    // designated members left behind by a prior mid-dispatch crash /
    // interrupted rollback / save-mid-dispatch.
    //
    // Loaded actors only: senders are typically remote, so unloaded
    // persistent actors that were previously designated but never
    // demoted may be missed. In practice the normal flow demotes on
    // every terminal path, and the only way a straggler survives is a
    // crash inside the compose/dispatch window (~seconds).
    void SweepStaleDesignated(RE::TESFaction* fact,
                              RE::Actor* target,
                              std::int8_t designatedRank,
                              std::int8_t candidateRank,
                              std::string_view logTag);

    // Promote `sender` to `designatedRank`. Sweeps stale designated
    // members first so there's exactly one rank-`designatedRank` actor
    // when the caller's EnsureQuestStarted runs. Safe on already-
    // designated senders (no-op).
    void PromoteToDesignated(RE::TESFaction* fact,
                             RE::Actor* sender,
                             std::int8_t designatedRank,
                             std::int8_t candidateRank,
                             std::string_view logTag);

    // Return `sender` to `candidateRank`. Called on both success and
    // failure completion paths so the faction state is always cleaned
    // up. Safe on actors not in the faction (no-op).
    void DemoteToCandidate(RE::TESFaction* fact, RE::Actor* sender, std::int8_t candidateRank, std::string_view logTag);
} // namespace NarrativeEngine::FactionDesignationUtils
