#include <AmbushSpawnPoints.h>

#include <CameraVisibility.h>
#include <logger.h>
#include <Settings.h>

#include <RE/B/BSNavmesh.h>
#include <RE/N/NavMesh.h>
#include <RE/T/TES.h>
#include <RE/T/TESObjectCELL.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace NarrativeEngine::AmbushSpawnPoints
{
    namespace
    {
        // Azimuths sampled per radius. Angular resolution is what
        // decides whether the search can FIND cover: the shadow behind a
        // boulder at 2500 units is only a few hundred units of arc wide,
        // and 16 spokes leaves ~980 units between samples — enough to
        // step straight over it. 32 halves that. The cost is paid once
        // per ambush, and the search stops as soon as the forward arc
        // yields a covered spot.
        constexpr int kAzimuthSamples = 32;

        // Radius multipliers, tried in order. Widening only — the
        // configured minimum is a hard floor, so a narrowing step would
        // just clamp back onto the first radius and re-sample the same
        // ring. Every radius is clamped into
        // [ambushMin.., ambushMax..] before use.
        constexpr float kRadiusMultipliers[] = {1.0f, 1.25f, 1.5f, 2.0f, 2.5f};

        // How far attackers are spread around the winning point.
        // Tight enough to read as one group arriving together, loose
        // enough that actor collision capsules (~35 units) aren't
        // shoving each other apart on spawn. At this radius a ring of
        // four sits ~200 units between neighbours.
        constexpr float kClusterRadiusUnits = 150.0f;

        // Vertical tolerance for the navmesh containment test. A point
        // counts as on-mesh if it is within this many units above or
        // below the triangle's interpolated surface. Generous enough to
        // absorb the difference between the land-height query and the
        // navmesh's own surface (they disagree by a few units routinely
        // on sloped terrain), tight enough not to match a mesh on the
        // floor below when standing on a bridge.
        constexpr float kNavmeshZToleranceUnits = 96.0f;

        // Lift applied to a validated ground position before handing it
        // out, so the spawned actor starts just above the surface and
        // settles down rather than starting embedded and being pushed
        // out sideways.
        constexpr float kGroundClearanceUnits = 8.0f;

        // How far below the water surface the ground has to be before a
        // point counts as submerged. A small tolerance rather than zero
        // because shoreline terrain and the water plane meet within a
        // few units of each other, and rejecting the entire waterline
        // would carve usable beach out of the search for no reason.
        constexpr float kWaterDepthToleranceUnits = 24.0f;

        // True when `pos` sits under the local water surface.
        //
        // Cell overload, not the TES one: it returns a bool for whether
        // water exists at all, rather than a sentinel height. Plenty of
        // Skyrim terrain sits at large negative Z, where a misread
        // sentinel would reject every candidate.
        bool IsUnderwater(RE::TESObjectCELL* cell, const RE::NiPoint3& pos, float groundZ)
        {
            if (!cell) {
                return false;
            }
            float waterZ = 0.0f;
            if (!cell->GetWaterHeight(pos, waterZ)) {
                return false; // no water in this cell
            }
            return groundZ < waterZ - kWaterDepthToleranceUnits;
        }

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
            int rejectedUnderwater = 0;
            int rejectedNoNavmesh = 0;
            int rejectedVisible = 0;
            int survived = 0;

            std::string Describe() const
            {
                return "sampled=" + std::to_string(sampled) + " noCell=" + std::to_string(rejectedNoCell) + " interior="
                       + std::to_string(rejectedInterior) + " noGround=" + std::to_string(rejectedNoGround)
                       + " underwater=" + std::to_string(rejectedUnderwater)
                       + " noNavmesh=" + std::to_string(rejectedNoNavmesh)
                       + " visible=" + std::to_string(rejectedVisible) + " survived=" + std::to_string(survived);
            }
        };

        // Barycentric point-in-triangle on the XY projection. Returns
        // the interpolated Z at (px, py) when inside.
        bool PointInTriangleXY(float px,
                               float py,
                               const RE::NiPoint3& a,
                               const RE::NiPoint3& b,
                               const RE::NiPoint3& c,
                               float& zOut)
        {
            const float v0x = c.x - a.x;
            const float v0y = c.y - a.y;
            const float v1x = b.x - a.x;
            const float v1y = b.y - a.y;
            const float v2x = px - a.x;
            const float v2y = py - a.y;

            const float dot00 = v0x * v0x + v0y * v0y;
            const float dot01 = v0x * v1x + v0y * v1y;
            const float dot02 = v0x * v2x + v0y * v2y;
            const float dot11 = v1x * v1x + v1y * v1y;
            const float dot12 = v1x * v2x + v1y * v2y;

            const float denom = dot00 * dot11 - dot01 * dot01;
            if (std::fabs(denom) < 1e-6f) {
                // Degenerate (zero-area) triangle — navmeshes do
                // contain a few. Nothing can be inside it.
                return false;
            }
            const float inv = 1.0f / denom;
            const float u = (dot11 * dot02 - dot01 * dot12) * inv;
            const float v = (dot00 * dot12 - dot01 * dot02) * inv;

            if (u < 0.0f || v < 0.0f || (u + v) > 1.0f) {
                return false;
            }
            // Barycentric interpolation of the vertex heights.
            zOut = a.z + u * (c.z - a.z) + v * (b.z - a.z);
            return true;
        }
    } // namespace

    bool IsOnNavmesh(const RE::NiPoint3& pos)
    {
        auto* tes = RE::TES::GetSingleton();
        if (!tes) {
            return false;
        }
        auto* cell = tes->GetCell(pos);
        if (!cell) {
            return false;
        }

        // navMeshes lives in the cell's RUNTIME_DATA block, whose
        // offset moved in 1.6.629 — GetRuntimeData() resolves the right
        // one for the running build.
        auto* navMeshes = cell->GetRuntimeData().navMeshes;
        if (!navMeshes) {
            // A loaded exterior cell with no navmesh array at all is
            // rare but real (some ocean / border cells). Nothing to
            // stand on as far as pathing is concerned.
            return false;
        }

        for (const auto& meshPtr : navMeshes->navMeshes) {
            auto* mesh = meshPtr.get();
            if (!mesh) {
                continue;
            }
            const auto& verts = mesh->vertices;
            const auto& tris = mesh->triangles;
            for (const auto& tri : tris) {
                const auto i0 = tri.vertices[0];
                const auto i1 = tri.vertices[1];
                const auto i2 = tri.vertices[2];
                if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) {
                    continue;
                }
                float surfaceZ = 0.0f;
                if (!PointInTriangleXY(
                        pos.x, pos.y, verts[i0].location, verts[i1].location, verts[i2].location, surfaceZ)) {
                    continue;
                }
                if (std::fabs(surfaceZ - pos.z) <= kNavmeshZToleranceUnits) {
                    return true;
                }
            }
        }
        return false;
    }

    std::vector<RE::NiPoint3> Find(RE::Actor* player, int distanceUnits, int count)
    {
        std::vector<RE::NiPoint3> out;
        if (!player || count <= 0) {
            return out;
        }

        const auto& cfg = Settings::Get();
        const bool debug = cfg.debugMode;

        auto* tes = RE::TES::GetSingleton();
        if (!tes) {
            logger::warn("AmbushSpawnPoints: no TES singleton; cannot search.");
            return out;
        }

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

        for (const float mult : kRadiusMultipliers) {
            // Clamp into the configured band. Below the floor an ambush
            // is visibly conjured; above the ceiling it is too far away
            // to read as one.
            const float radius = std::clamp(requested * mult, floorDistance, ceilDistance);
            if (radius == lastRadius) {
                continue; // clamping collapsed this step onto the last
            }
            lastRadius = radius;
            if (!radiiTried.empty()) {
                radiiTried += ", ";
            }
            radiiTried += std::to_string(static_cast<int>(radius));

            for (int i = 0; i < kAzimuthSamples; ++i) {
                const float theta =
                    (2.0f * 3.14159265358979323846f * static_cast<float>(i)) / static_cast<float>(kAzimuthSamples);
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

                // Gate 4 — not underwater. Navmesh covers lake and
                // river bed, so containment alone accepts a spot under
                // water and the attacker spawns swimming.
                if (IsUnderwater(cell, probe, groundZ)) {
                    ++tally.rejectedUnderwater;
                    continue;
                }

                // Gate 5 — standable. See the header's note on why this
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

                // Gate 6 — solidly behind cover. Last because it is by
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

            // Stop widening once the forward arc has yielded something.
            // Rear-hemisphere hits alone are NOT enough to stop on:
            // they're the fallback, and a wider ring may still have a
            // hidden spot in front. Survivors accumulate across radii,
            // so nothing found so far is thrown away.
            const bool haveForward =
                std::any_of(survivors.begin(), survivors.end(), [](const Candidate& c) { return c.IsForward(); });
            if (haveForward) {
                break;
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
        // the azimuth directly behind, then farthest away. Those two
        // keys compose naturally — every ring samples the same azimuths,
        // so the rear-most azimuth ties across radii and the distance
        // key picks the outermost of them.
        const bool usingFallback = survivors.empty();
        std::vector<Candidate>& pool = usingFallback ? uncovered : survivors;

        if (pool.empty()) {
            // Genuinely nowhere to stand — not even uncovered ground.
            // The caller turns this into a clean COMPOSE failure; the
            // tally says which gate did the killing.
            logger::info("AmbushSpawnPoints: no usable point near {}u (radii tried: {}). Gates: {}",
                         distanceUnits,
                         radiiTried,
                         tally.Describe());
            return out;
        }

        if (usingFallback) {
            logger::info("AmbushSpawnPoints: no covered point near {}u after {} samples — falling back to "
                         "the most distant point nearest the player's rear. Gates: {}",
                         distanceUnits,
                         tally.sampled,
                         tally.Describe());
            std::sort(pool.begin(), pool.end(), [](const Candidate& a, const Candidate& b) {
                if (std::fabs(a.facingDot - b.facingDot) > 0.01f) {
                    return a.facingDot < b.facingDot; // -1 is directly behind
                }
                return a.actualDistance > b.actualDistance; // farthest first
            });
        } else {
            // Rank: forward arc before rear as a hard tier, then by the
            // combined angle-and-distance score within each tier.
            std::sort(pool.begin(), pool.end(), [](const Candidate& a, const Candidate& b) {
                if (a.IsForward() != b.IsForward()) {
                    return a.IsForward();
                }
                return a.score < b.score;
            });
        }

        const Candidate& winner = pool.front();

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
                // Same water gate: a dry winning point says nothing
                // about a jittered point 150 units away.
                auto* pCell = tes->GetCell(p);
                if (!IsUnderwater(pCell, p, groundZ) && IsOnNavmesh(p)) {
                    out.push_back(p);
                    continue;
                }
            }
            out.push_back(winner.pos);
        }

        if (debug) {
            logger::debug("AmbushSpawnPoints: requested {}u x{} -> winner ({:.0f},{:.0f},{:.0f}) "
                          "dist={:.0f} facingDot={:.2f} score={:.2f} arc={} (radii tried: {}). Gates: {}",
                          distanceUnits,
                          count,
                          winner.pos.x,
                          winner.pos.y,
                          winner.pos.z,
                          winner.actualDistance,
                          winner.facingDot,
                          winner.score,
                          usingFallback        ? "uncovered-fallback"
                          : winner.IsForward() ? "forward"
                                               : "rear-fallback",
                          radiiTried,
                          tally.Describe());
        }

        return out;
    }

    std::vector<RE::NiPoint3> Find(const MainThread::Token&, RE::Actor* player, int distanceUnits, int count)
    {
        return Find(player, distanceUnits, count);
    }
} // namespace NarrativeEngine::AmbushSpawnPoints
