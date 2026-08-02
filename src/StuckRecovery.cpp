#include <StuckRecovery.h>

#include <logger.h>

#include <RE/B/BSNavmesh.h>
#include <RE/N/NavMesh.h>
#include <RE/T/TES.h>
#include <RE/T/TESObjectCELL.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace NarrativeEngine::StuckRecovery
{
    namespace
    {
        // Vertical tolerance for the navmesh containment test. A point
        // counts as on-mesh if it is within this many units above or
        // below the triangle's interpolated surface. Generous enough to
        // absorb the difference between the land-height query and the
        // navmesh's own surface (they disagree by a few units routinely
        // on sloped terrain), tight enough not to match a mesh on the
        // floor below when standing on a bridge.
        constexpr float kNavmeshZToleranceUnits = 96.0f;

        // How far below the water surface the ground has to be before a
        // point counts as submerged. A small tolerance rather than zero
        // because shoreline terrain and the water plane meet within a
        // few units of each other, and rejecting the entire waterline
        // would carve usable beach out of the search for no reason.
        constexpr float kWaterDepthToleranceUnits = 24.0f;

        // Lateral offsets tried when a close-in step lands somewhere an
        // actor can't stand. Small and few: this is a nudge off a bad
        // metre of ground, not a search — if the whole neighbourhood is
        // unusable, the next close-in step moves on past it anyway.
        constexpr float kCloseInNudgeUnits[] = {0.0f, 200.0f, 400.0f};
        constexpr int kCloseInNudgeAzimuths = 6;

        // Extra height added to a close-in destination, on top of the
        // ground clearance IsStandable already applies.
        //
        // Close-in steps land on a bare line toward the goal, chosen
        // without any of the vetting the fallback positions had, so they
        // are the likeliest of all our placements to sit inside a rock
        // or a tree trunk. Dropping the actor from above it instead
        // lets physics settle it ON the obstruction; being briefly
        // airborne is recoverable, being inside geometry is what we are
        // trying to escape.
        constexpr float kCloseInLiftUnits = 100.0f;

        constexpr float kPi = 3.14159265358979323846f;

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

        // A standable point at or near `probe`, for the close-in steps.
        bool StandableAtOrNear(const RE::NiPoint3& probe, RE::NiPoint3& out)
        {
            for (const float nudge : kCloseInNudgeUnits) {
                if (nudge <= 0.0f) {
                    if (IsStandable(probe, out)) {
                        return true;
                    }
                    continue;
                }
                for (int i = 0; i < kCloseInNudgeAzimuths; ++i) {
                    const float theta =
                        (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(kCloseInNudgeAzimuths);
                    const RE::NiPoint3 p{probe.x + nudge * std::cos(theta), probe.y + nudge * std::sin(theta), probe.z};
                    if (IsStandable(p, out)) {
                        return true;
                    }
                }
            }
            return false;
        }
    } // namespace

    const char* ActionName(Action action)
    {
        switch (action) {
        case Action::Moving:
            return "moving";
        case Action::WarpedToFallback:
            return "warped_to_fallback";
        case Action::WarpedCloser:
            return "warped_closer";
        case Action::Stranded:
            return "stranded";
        case Action::None:
        default:
            return "none";
        }
    }

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

    bool IsUnderwater(RE::TESObjectCELL* cell, const RE::NiPoint3& pos, float groundZ)
    {
        if (!cell) {
            auto* tes = RE::TES::GetSingleton();
            cell = tes ? tes->GetCell(pos) : nullptr;
        }
        if (!cell) {
            return false;
        }
        float waterZ = 0.0f;
        // Cell overload, not the TES one: it returns a bool for whether
        // water exists at all, rather than a sentinel height. Plenty of
        // Skyrim terrain sits at large negative Z, where a misread
        // sentinel would reject every candidate.
        if (!cell->GetWaterHeight(pos, waterZ)) {
            return false; // no water in this cell
        }
        return groundZ < waterZ - kWaterDepthToleranceUnits;
    }

    bool GroundPoint(const RE::NiPoint3& pos, RE::NiPoint3& out, float& outGroundZ)
    {
        auto* tes = RE::TES::GetSingleton();
        if (!tes) {
            return false;
        }
        float groundZ = 0.0f;
        if (!tes->GetLandHeight(pos, groundZ)) {
            return false;
        }
        out = pos;
        out.z = groundZ + kGroundClearanceUnits;
        outGroundZ = groundZ;
        return true;
    }

    bool IsStandable(const RE::NiPoint3& pos, RE::NiPoint3& out)
    {
        float groundZ = 0.0f;
        if (!GroundPoint(pos, out, groundZ)) {
            return false;
        }
        auto* tes = RE::TES::GetSingleton();
        auto* cell = tes ? tes->GetCell(out) : nullptr;
        if (IsUnderwater(cell, out, groundZ)) {
            return false;
        }
        return IsOnNavmesh(out);
    }

    void WarpTo(const MainThread::Token&, RE::Actor* actor, const RE::NiPoint3& pos)
    {
        if (!actor) {
            return;
        }
        actor->SetPosition(pos, /*a_updateCharController=*/true);
        actor->Update3DPosition(/*a_warp=*/true);
        // Without this the actor keeps running the movement it had
        // queued against the old position and walks straight back.
        actor->EvaluatePackage();
    }

    Escort::Escort(std::string label) : m_label(std::move(label)) {}

    void Escort::Begin(std::vector<RE::NiPoint3> fallbacks)
    {
        m_fallbacks = std::move(fallbacks);
        m_nextFallback = 0;
        m_tracks.clear();
        logger::info("StuckRecovery[{}]: escort begun with {} fallback position(s)", m_label, m_fallbacks.size());
    }

    void Escort::Track(RE::Actor* actor, const RE::NiPoint3& placedAt)
    {
        if (!actor) {
            return;
        }
        auto& track = m_tracks[actor->GetFormID()];
        track.lastPos = placedAt;
    }

    bool Escort::DueForCheck(double elapsedSeconds, const Options& opts)
    {
        m_sinceCheck += elapsedSeconds;
        if (m_sinceCheck < opts.checkIntervalSeconds) {
            return false;
        }
        m_sinceCheck = 0.0;
        return true;
    }

    void Escort::Forget(RE::FormID actorId)
    {
        m_tracks.erase(actorId);
    }

    void Escort::Clear()
    {
        m_fallbacks.clear();
        m_nextFallback = 0;
        m_tracks.clear();
        m_sinceCheck = 0.0;
    }

    Outcome Escort::Update(const MainThread::Token& token,
                           RE::Actor* actor,
                           const RE::NiPoint3& goal,
                           const Options& opts)
    {
        Outcome outcome;
        if (!actor) {
            return outcome;
        }

        auto& track = m_tracks[actor->GetFormID()];
        const auto pos = actor->GetPosition();
        const float goalDist = pos.GetDistance(goal);

        // Fighting. Not our business at any range — see the header. The
        // baseline is still advanced so that if combat ends and the
        // actor goes back to travelling, the next check measures from
        // where it actually is.
        if (actor->IsInCombat()) {
            track.lastPos = pos;
            outcome.action = Action::Moving;
            return outcome;
        }

        // Arrived. Standing still at this range is fighting, not being
        // trapped, and warping an actor mid-melee would be worse than
        // any problem this module solves.
        if (goalDist <= opts.arrivedDistanceUnits) {
            track.lastPos = pos;
            outcome.action = Action::Moving;
            return outcome;
        }

        const float moved = pos.GetDistance(track.lastPos);
        track.lastPos = pos;
        if (moved >= opts.movementThresholdUnits) {
            // Travelling under its own power. Note the cursor is NOT
            // rewound: a position that already failed to work is not
            // worth re-trying if this actor stalls again later.
            outcome.action = Action::Moving;
            return outcome;
        }

        if (track.stranded) {
            outcome.action = Action::Stranded;
            return outcome;
        }

        // Step 1 — the caller's unused-but-validated positions. These
        // are the good option: vetted by a real spawn search and far
        // enough away to be genuinely different terrain.
        if (m_nextFallback < m_fallbacks.size()) {
            const auto dest = m_fallbacks[m_nextFallback++];
            WarpTo(token, actor, dest);
            track.lastPos = dest;
            outcome.action = Action::WarpedToFallback;
            outcome.movedTo = dest;
            logger::info("StuckRecovery[{}]: '{}' moved only {:.0f}u at ({:.0f},{:.0f},{:.0f}) -> fallback "
                         "{} of {} at ({:.0f},{:.0f},{:.0f})",
                         m_label,
                         actor->GetName(),
                         moved,
                         pos.x,
                         pos.y,
                         pos.z,
                         m_nextFallback,
                         m_fallbacks.size(),
                         dest.x,
                         dest.y,
                         dest.z);
            return outcome;
        }

        // Step 2 — out of vetted positions, so close the gap directly.
        // Each stall moves the actor another step along the line it was
        // supposed to walk, which both shortens the route and skips
        // whatever it was caught on.
        if (goalDist <= opts.minGoalDistanceUnits) {
            track.stranded = true;
            outcome.action = Action::Stranded;
            logger::warn("StuckRecovery[{}]: '{}' is stuck {:.0f}u from its goal with no fallbacks left and "
                         "no room to close in — giving up on it",
                         m_label,
                         actor->GetName(),
                         goalDist);
            return outcome;
        }

        const float target = std::max(opts.minGoalDistanceUnits, goalDist - opts.closeInStepUnits);
        const float fraction = (goalDist - target) / std::max(1.0f, goalDist);
        const RE::NiPoint3 probe{pos.x + (goal.x - pos.x) * fraction,
                                 pos.y + (goal.y - pos.y) * fraction,
                                 pos.z + (goal.z - pos.z) * fraction};

        RE::NiPoint3 dest{};
        if (!StandableAtOrNear(probe, dest)) {
            // Not fatal: the actor keeps its position and the next
            // interval steps again from here, which probes further
            // along the same line.
            ++track.closeInSteps;
            logger::warn("StuckRecovery[{}]: '{}' close-in step {} found nowhere standable near "
                         "({:.0f},{:.0f},{:.0f}); will step again",
                         m_label,
                         actor->GetName(),
                         track.closeInSteps,
                         probe.x,
                         probe.y,
                         probe.z);
            return outcome;
        }

        dest.z += kCloseInLiftUnits;

        WarpTo(token, actor, dest);
        track.lastPos = dest;
        ++track.closeInSteps;
        outcome.action = Action::WarpedCloser;
        outcome.movedTo = dest;
        logger::info("StuckRecovery[{}]: '{}' moved only {:.0f}u and has no fallbacks left -> closed in to "
                     "{:.0f}u from goal at ({:.0f},{:.0f},{:.0f}) (step {})",
                     m_label,
                     actor->GetName(),
                     moved,
                     dest.GetDistance(goal),
                     dest.x,
                     dest.y,
                     dest.z,
                     track.closeInSteps);
        return outcome;
    }
} // namespace NarrativeEngine::StuckRecovery
