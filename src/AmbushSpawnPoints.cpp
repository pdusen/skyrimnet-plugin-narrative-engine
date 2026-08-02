#include <AmbushSpawnPoints.h>

#include <CameraVisibility.h>
#include <logger.h>
#include <Settings.h>
#include <StuckRecovery.h>

#include <RE/B/BSNavmesh.h>
#include <RE/N/NavMesh.h>
#include <RE/T/TES.h>
#include <RE/T/TESObjectCELL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <string>

namespace NarrativeEngine::AmbushSpawnPoints
{
    namespace
    {
        // Radius multipliers, tried in order. Widening only — the
        // configured minimum is a hard floor, so a narrowing step would
        // just clamp back onto the first radius and re-sample the same
        // ring. Every radius is clamped into
        // [ambushMin.., ambushMax..] before use.
        //
        // Spaced 500 units apart against the 2500-unit band floor —
        // 2500, 3000, 3500, 4500, 5500 — rather than the round
        // multiples they started as. The near ring answers most
        // searches, and at the old 2000 the walk in was over in three
        // and a half seconds, which is too fast to read as an approach
        // and left nothing else time to work.
        constexpr float kRadiusMultipliers[] = {1.0f, 1.2f, 1.4f, 1.8f, 2.2f};

        // Azimuths sampled per ring, parallel to kRadiusMultipliers.
        //
        // Angular resolution is what decides whether the search can FIND
        // cover, and what it is looking for — the shadow behind a
        // boulder — is a fixed width in world units, not in degrees. One
        // azimuth count for every ring therefore over-samples the near
        // ring and under-samples the far one: at a flat 32 the gap
        // between samples runs 491 units at 2500 out to 1080 at 5500, wide
        // enough by the outer rings to step straight over most cover.
        // Scaling the count with the radius holds that gap near 400
        // units throughout.
        //
        // Worst case is 240 samples against a flat 160, but only the
        // open-plain case pays it: the search stops at the first ring
        // yielding a covered spot in the forward arc, and ring 0 — the
        // one that answers most searches — is unchanged at 32.
        constexpr int kAzimuthSamples[] = {32, 40, 48, 56, 64};
        static_assert(std::size(kAzimuthSamples) == std::size(kRadiusMultipliers));

        // How far attackers are spread around the winning point.
        // Tight enough to read as one group arriving together, loose
        // enough that actor collision capsules (~35 units) aren't
        // shoving each other apart on spawn. At this radius a ring of
        // four sits ~200 units between neighbours.
        constexpr float kClusterRadiusUnits = 150.0f;

        // Navmesh containment, water, and ground clearance all live in
        // StuckRecovery — the same primitives answer "can an actor stand
        // here" for a spawn candidate and for a rescue candidate, and
        // one copy of the navmesh triangle walk is enough.
        using StuckRecovery::kGroundClearanceUnits;

        // Runner-up spawn points kept for recovery, and how far apart
        // they have to be to count as distinct options. The separation
        // is generous because the whole value of a fallback is that it
        // is somewhere ELSE — terrain that failed to let an actor travel
        // does not improve a few hundred units along.
        constexpr std::size_t kMaxFallbacks = 6;
        constexpr float kFallbackSeparationUnits = 800.0f;

        // Largest elevation difference from the player a candidate may
        // have, in either direction.
        //
        // This is a REACHABILITY proxy, not an aesthetic one. The navmesh
        // gate answers containment only, so a ledge partway up a cliff is
        // "on navmesh" and still has no walkable route down — attackers
        // spawned there mill about until the beat abandons itself. There
        // is no connectivity query to ask (see the module header), so the
        // stand-in is elevation: unreachable ground is almost always a
        // long way up or down.
        //
        // Absolute rather than a slope ratio. A ratio scales the vertical
        // budget with horizontal distance, which is backwards — the outer
        // rings would get the loosest allowance, and they are the ones
        // most likely to reach across a valley onto something
        // disconnected. At the band's 2500-5500 span this cap is
        // equivalent to a slope limit of 22 degrees at the near ring and
        // 10 at the far one.
        //
        // Symmetric because a gorge floor is as unreachable as a ledge.
        constexpr float kMaxElevationDeltaUnits = 1000.0f;

        using StuckRecovery::IsOnNavmesh;
        using StuckRecovery::IsUnderwater;

        struct Candidate
        {
            RE::NiPoint3 pos;
            float actualDistance = 0.0f;
            // Dot product of (candidate - player) against the player's
            // facing, normalized. -1 is directly behind, +1 directly
            // ahead.
            float facingDot = 0.0f;

            // Combined placement cost, lower is better. Angular
            // deviation from dead-ahead and distance within the
            // configured band, each normalized to [0,1] and weighted
            // equally.
            //
            // Equal weighting is the point: neither axis should be a
            // pure tiebreaker for the other. Dead-ahead at the near end
            // of the band beats square-to-the-side at the same distance,
            // but square-to-the-side up close beats dead-ahead two
            // thousand units further out.
            float score = 0.0f;

            // Ahead of the player or square to either side. This is the
            // preferred arc: an ambush wants to be walked into, not to
            // arrive from behind. Anything in the rear hemisphere is a
            // fallback, taken only when the forward arc has nothing
            // hidden enough to spawn in.
            bool IsForward() const
            {
                return facingDot >= 0.0f;
            }
        };

        // Per-search diagnostics. A failed search MUST be able to say
        // which gate killed it — that is the single most useful log
        // line in this beat, and without it "no ambush happened" is
        // indistinguishable from "the Director didn't pick one".
        struct GateTally
        {
            int sampled = 0;
            int rejectedNoCell = 0;
            int rejectedInterior = 0;
            int rejectedNoGround = 0;
            int rejectedElevation = 0;
            int rejectedUnderwater = 0;
            int rejectedNoNavmesh = 0;
            int rejectedVisible = 0;
            int survived = 0;

            std::string Describe() const
            {
                return "sampled=" + std::to_string(sampled) + " noCell=" + std::to_string(rejectedNoCell) + " interior="
                       + std::to_string(rejectedInterior) + " noGround=" + std::to_string(rejectedNoGround)
                       + " elevation=" + std::to_string(rejectedElevation) + " underwater="
                       + std::to_string(rejectedUnderwater) + " noNavmesh=" + std::to_string(rejectedNoNavmesh)
                       + " visible=" + std::to_string(rejectedVisible) + " survived=" + std::to_string(survived);
            }
        };

        // Ranking for covered candidates: forward arc before rear as a
        // hard tier, then by the combined angle-and-distance score
        // within each tier.
        void RankCovered(std::vector<Candidate>& pool)
        {
            std::sort(pool.begin(), pool.end(), [](const Candidate& a, const Candidate& b) {
                if (a.IsForward() != b.IsForward()) {
                    return a.IsForward();
                }
                return a.score < b.score;
            });
        }

        // Runner-up positions from a RANKED pool: everything after the
        // winner that is far enough from it, and from everything already
        // taken, to be a genuinely different piece of terrain.
        //
        // Shared between the widening loop's stop condition and the
        // final result, so "how many fallbacks do we have" is answered
        // the same way in both places — an approximation in the loop
        // would let it stop early and come up short.
        std::vector<RE::NiPoint3> SelectFallbacks(const std::vector<Candidate>& ranked)
        {
            std::vector<RE::NiPoint3> out;
            if (ranked.empty()) {
                return out;
            }
            out.reserve(kMaxFallbacks);
            const auto& winner = ranked.front().pos;
            for (std::size_t i = 1; i < ranked.size() && out.size() < kMaxFallbacks; ++i) {
                const auto& p = ranked[i].pos;
                if (p.GetDistance(winner) < kFallbackSeparationUnits) {
                    continue;
                }
                const bool tooClose = std::any_of(out.begin(), out.end(), [&p](const RE::NiPoint3& kept) {
                    return p.GetDistance(kept) < kFallbackSeparationUnits;
                });
                if (!tooClose) {
                    out.push_back(p);
                }
            }
            return out;
        }
    } // namespace

    Result Find(RE::Actor* player, int distanceUnits, int count)
    {
        Result result;
        auto& out = result.spawnPoints;
        if (!player || count <= 0) {
            return result;
        }

        const auto& cfg = Settings::Get();
        const bool debug = cfg.debugMode;

        auto* tes = RE::TES::GetSingleton();
        if (!tes) {
            logger::warn("AmbushSpawnPoints: no TES singleton; cannot search.");
            return result;
        }

        // Instrumentation only. The whole search is one main-thread
        // block, so its cost is a hitch the player feels directly —
        // worth knowing before anyone widens the sampling further.
        const auto startedAt = std::chrono::steady_clock::now();
        const auto msSince = [](std::chrono::steady_clock::time_point from) {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
        };

        const auto playerPos = player->GetPosition();
        const float playerAngleZ = player->GetAngleZ();
        // Skyrim's Z angle is clockwise from +Y, so the facing vector is
        // (sin, cos) rather than the (cos, sin) you'd write by reflex.
        const float faceX = std::sin(playerAngleZ);
        const float faceY = std::cos(playerAngleZ);

        const float requested = static_cast<float>(distanceUnits);
        const float floorDistance = static_cast<float>(cfg.ambushMinSpawnDistanceUnits);
        const float ceilDistance = std::max(floorDistance, static_cast<float>(cfg.ambushMaxSpawnDistanceUnits));
        // Denominator for the distance half of the placement score.
        const float bandSpan = std::max(1.0f, ceilDistance - floorDistance);

        // Points that passed every gate except cover. Only consulted
        // when nothing covered exists anywhere.
        std::vector<Candidate> survivors;
        std::vector<Candidate> uncovered;
        GateTally tally;
        std::string radiiTried;
        float lastRadius = -1.0f;

        for (std::size_t ring = 0; ring < std::size(kRadiusMultipliers); ++ring) {
            // Clamp into the configured band. Below the floor an ambush
            // is visibly conjured; above the ceiling it is too far away
            // to read as one.
            const float radius = std::clamp(requested * kRadiusMultipliers[ring], floorDistance, ceilDistance);
            if (radius == lastRadius) {
                continue; // clamping collapsed this step onto the last
            }
            lastRadius = radius;
            const int azimuths = kAzimuthSamples[ring];
            if (!radiiTried.empty()) {
                radiiTried += ", ";
            }
            radiiTried += std::to_string(static_cast<int>(radius));
            radiiTried += "/";
            radiiTried += std::to_string(azimuths);

            for (int i = 0; i < azimuths; ++i) {
                const float theta =
                    (2.0f * 3.14159265358979323846f * static_cast<float>(i)) / static_cast<float>(azimuths);
                RE::NiPoint3 probe{
                    playerPos.x + radius * std::cos(theta), playerPos.y + radius * std::sin(theta), playerPos.z};
                ++tally.sampled;

                // Gate 1 — cell loaded. Cheapest, and everything below
                // needs the cell anyway.
                auto* cell = tes->GetCell(probe);
                if (!cell) {
                    ++tally.rejectedNoCell;
                    continue;
                }
                // Gate 2 — exterior only. This beat spawns a travelling
                // approach; interiors have neither the room nor the
                // sightlines for it.
                if (cell->IsInteriorCell()) {
                    ++tally.rejectedInterior;
                    continue;
                }

                // Gate 3 — resolvable ground height. Void fails here.
                // Water does NOT: there is perfectly good terrain under
                // a lake, which is why the next gate exists.
                float groundZ = 0.0f;
                if (!tes->GetLandHeight(probe, groundZ)) {
                    ++tally.rejectedNoGround;
                    continue;
                }
                probe.z = groundZ + kGroundClearanceUnits;

                // Gate 4 — roughly on the player's level. Ahead of the
                // two expensive gates below because it is a subtraction,
                // and in mountainous terrain it culls candidates that
                // would otherwise pay for a navmesh scan and nine
                // raycasts before being ranked as unreachable anyway.
                if (std::fabs(probe.z - playerPos.z) > kMaxElevationDeltaUnits) {
                    ++tally.rejectedElevation;
                    continue;
                }

                // Gate 5 — not underwater. Navmesh covers lake and
                // river bed, so containment alone accepts a spot under
                // water and the attacker spawns swimming.
                if (IsUnderwater(cell, probe, groundZ)) {
                    ++tally.rejectedUnderwater;
                    continue;
                }

                // Gate 6 — standable. See the header's note on why this
                // is containment rather than reachability.
                if (!IsOnNavmesh(probe)) {
                    ++tally.rejectedNoNavmesh;
                    continue;
                }

                const float dx = probe.x - playerPos.x;
                const float dy = probe.y - playerPos.y;
                const float actual = std::sqrt(dx * dx + dy * dy);
                const float invLen = actual > 1e-3f ? 1.0f / actual : 0.0f;

                Candidate c;
                c.pos = probe;
                c.actualDistance = actual;
                c.facingDot = (dx * faceX + dy * faceY) * invLen;
                // 0 dead ahead, 0.5 square to the side, 1 directly
                // behind.
                const float angleCost = (1.0f - c.facingDot) * 0.5f;
                // 0 at the near end of the band, 1 at the far end.
                const float distCost = std::clamp((actual - floorDistance) / bandSpan, 0.0f, 1.0f);
                c.score = angleCost + distCost;

                // Gate 7 — solidly behind cover. Last because it is by
                // far the most expensive (nine Havok raycasts).
                //
                // The silhouette is widened by the cluster radius, so
                // this asks whether the whole group would be hidden,
                // not just the point at its centre. Attackers ring out
                // to kClusterRadiusUnits, and one of them standing
                // clear of the rock the others are behind is the same
                // failure as no cover at all.
                //
                // Failing here is NOT elimination: the point has already
                // passed every other gate, so it is somewhere an
                // attacker can legitimately stand. It is kept as a
                // last-resort candidate for the open-plain case, where
                // no cover exists anywhere and declining to spawn at all
                // would be the worse answer.
                if (!CameraVisibility::IsPositionBehindCover(probe, kActorHeightUnits, kClusterRadiusUnits)) {
                    ++tally.rejectedVisible;
                    uncovered.push_back(c);
                    continue;
                }

                survivors.push_back(c);
                ++tally.survived;
            }

            // Stop widening once the forward arc has yielded something
            // AND there are enough separated runner-ups behind it.
            //
            // Rear-hemisphere hits alone are NOT enough to stop on:
            // they're the fallback, and a wider ring may still have a
            // hidden spot in front. Survivors accumulate across radii,
            // so nothing found so far is thrown away.
            //
            // The fallback count is the second condition because those
            // positions are StuckRecovery's entire supply, and stopping
            // at the first covered spot routinely left one or two — a
            // near ring's survivors are all clustered together, so they
            // collapse to a single option under the separation rule.
            // Widening costs a few milliseconds and cannot cost us the
            // winner: extra rings only ever add candidates that score
            // WORSE than a forward one already found, so the ranking
            // front is fixed once haveForward is true.
            const bool haveForward =
                std::any_of(survivors.begin(), survivors.end(), [](const Candidate& c) { return c.IsForward(); });
            if (haveForward) {
                auto ranked = survivors;
                RankCovered(ranked);
                if (SelectFallbacks(ranked).size() >= kMaxFallbacks) {
                    break;
                }
            }
        }

        // Last resort: nothing covered anywhere, but standable ground
        // exists. An open plain has no cover by definition, and refusing
        // to spawn at all there would mean whole regions where ambushes
        // simply never happen. Spawn anyway, as far out of the way as
        // the terrain allows.
        //
        // Note this tier prefers the REAR, inverting the covered tiers'
        // preference. Without cover, the only thing left to hide behind
        // is the player's own back, so the ranking becomes: closest to
        // the azimuth directly behind, then farthest away.
        //
        // Rings no longer share an azimuth set, so the rear-most sample
        // of each ring lands at a slightly different angle. The spread is
        // bounded by the coarsest ring's half-step (5.625 degrees at 32
        // azimuths, or 0.005 of facingDot), which stays inside the
        // comparator's 0.01 tolerance — so those samples still tie on
        // angle and the distance key still picks the outermost.
        // Ring loop only — the clustering below adds its own per-attacker
        // navmesh checks, so the two are reported separately.
        const double searchMs = msSince(startedAt);

        const bool usingFallback = survivors.empty();
        std::vector<Candidate>& pool = usingFallback ? uncovered : survivors;

        if (pool.empty()) {
            // Genuinely nowhere to stand — not even uncovered ground.
            // The caller turns this into a clean COMPOSE failure; the
            // tally says which gate did the killing.
            logger::info("AmbushSpawnPoints: no usable point near {}u of player ({:.0f},{:.0f},{:.0f}) "
                         "(radii tried: {}) in {:.1f}ms. Gates: {}",
                         distanceUnits,
                         playerPos.x,
                         playerPos.y,
                         playerPos.z,
                         radiiTried,
                         searchMs,
                         tally.Describe());
            return result;
        }

        if (usingFallback) {
            logger::info("AmbushSpawnPoints: no covered point near {}u of player ({:.0f},{:.0f},{:.0f}) after "
                         "{} samples in {:.1f}ms — falling back to the most distant point nearest the "
                         "player's rear. Gates: {}",
                         distanceUnits,
                         playerPos.x,
                         playerPos.y,
                         playerPos.z,
                         tally.sampled,
                         searchMs,
                         tally.Describe());
            std::sort(pool.begin(), pool.end(), [](const Candidate& a, const Candidate& b) {
                if (std::fabs(a.facingDot - b.facingDot) > 0.01f) {
                    return a.facingDot < b.facingDot; // -1 is directly behind
                }
                return a.actualDistance > b.actualDistance; // farthest first
            });
        } else {
            RankCovered(pool);
        }

        const Candidate& winner = pool.front();

        // Runner-ups, for StuckRecovery. Separation is the whole point:
        // two candidates fifty units apart are one option, not two, and
        // an actor that can't travel from the winner won't fare better
        // fifty units along.
        result.fallbacks = SelectFallbacks(pool);

        // Cluster: ring the winner so attackers arrive as a group
        // rather than stacked on one coordinate. Each jittered point is
        // re-grounded and re-checked for navmesh; a point that fails
        // falls back to the winner itself, which is already validated.
        out.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            if (i == 0) {
                out.push_back(winner.pos);
                continue;
            }
            const float theta =
                (2.0f * 3.14159265358979323846f * static_cast<float>(i)) / static_cast<float>(std::max(1, count));
            RE::NiPoint3 p{winner.pos.x + kClusterRadiusUnits * std::cos(theta),
                           winner.pos.y + kClusterRadiusUnits * std::sin(theta),
                           winner.pos.z};
            float groundZ = 0.0f;
            if (tes->GetLandHeight(p, groundZ)) {
                p.z = groundZ + kGroundClearanceUnits;
                // Same water and elevation gates: a dry, on-level winning
                // point says nothing about a jittered point 150 units
                // away, which is easily enough to step off a ledge.
                auto* pCell = tes->GetCell(p);
                if (std::fabs(p.z - playerPos.z) <= kMaxElevationDeltaUnits && !IsUnderwater(pCell, p, groundZ)
                    && IsOnNavmesh(p)) {
                    out.push_back(p);
                    continue;
                }
            }
            out.push_back(winner.pos);
        }

        if (debug) {
            logger::debug("AmbushSpawnPoints: requested {}u x{} from player ({:.0f},{:.0f},{:.0f}) -> "
                          "winner ({:.0f},{:.0f},{:.0f}) dist={:.0f} dz={:.0f} facingDot={:.2f} score={:.2f} "
                          "arc={} fallbacks={} (radii tried: {}) search={:.1f}ms total={:.1f}ms. Gates: {}",
                          distanceUnits,
                          count,
                          playerPos.x,
                          playerPos.y,
                          playerPos.z,
                          winner.pos.x,
                          winner.pos.y,
                          winner.pos.z,
                          winner.actualDistance,
                          winner.pos.z - playerPos.z,
                          winner.facingDot,
                          winner.score,
                          usingFallback        ? "uncovered-fallback"
                          : winner.IsForward() ? "forward"
                                               : "rear-fallback",
                          result.fallbacks.size(),
                          radiiTried,
                          searchMs,
                          msSince(startedAt),
                          tally.Describe());
        }

        return result;
    }

    Result Find(const MainThread::Token&, RE::Actor* player, int distanceUnits, int count)
    {
        return Find(player, distanceUnits, count);
    }
} // namespace NarrativeEngine::AmbushSpawnPoints
