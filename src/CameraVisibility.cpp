#include <CameraVisibility.h>

#include <logger.h>
#include <Settings.h>

#include <RE/A/Actor.h>
#include <RE/B/bhkPickData.h>
#include <RE/B/bhkWorld.h>
#include <RE/B/BSFixedString.h>
#include <RE/N/NiAVObject.h>
#include <RE/N/NiBound.h>
#include <RE/N/NiPoint3.h>
#include <RE/P/PlayerCamera.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TES.h>
#include <RE/T/TESObjectCELL.h>

#include <array>
#include <cmath>
#include <cstddef>

namespace NarrativeEngine::CameraVisibility
{
    namespace
    {
        // Below this distance (game units) we skip the raycast fan
        // and just return "visible". Under ~300u the from-camera
        // ray can start inside the actor's bone collision (giving a
        // spurious "hit target" on the actor itself, which is a
        // safe true) or inside cover geometry (giving a spurious
        // "obstructed" result, which is unsafe). Skipping obstruction
        // checks below the floor sidesteps both. Well under the
        // 2000u floor the ReturnHome watchdog itself uses before
        // considering LOS-lost, so this is redundant safety at a
        // distance the caller already treats as "in view."
        constexpr float kMinDistanceFloor = 300.0f;

        // Sample-point count: 6 skeleton nodes + 4 bbox extremes.
        // Kept compact — Havok raycasts are microsecond-scale on
        // modern hardware but the ReturnHome tick fires only once
        // per second, so we're comfortably cheap.
        constexpr const char* kSkeletonNodeNames[] = {
            "NPC Head [Head]",
            "NPC Spine1 [Spn1]",     // chest / upper torso
            "NPC L Clavicle [LClv]", // left shoulder
            "NPC R Clavicle [RClv]", // right shoulder
            "NPC L Hand [LHnd]",     // left hand extreme
            "NPC R Hand [RHnd]",     // right hand extreme
        };

        // Fraction-of-ray-length that a hit must be past to count as
        // "reached the endpoint." Skeleton nodes sit inside their
        // bone's collidable — a ray targeted at the node coordinate
        // registers a hit slightly before hitFraction=1.0 as it
        // strikes the bone's collision shape. 0.95 gives us enough
        // slack that ordinary bone thickness (~15u on a ~2000u ray)
        // clears the check, while still catching obstruction hits
        // (which almost always land at hitFraction << 0.95).
        constexpr float kReachedFractionThreshold = 0.95f;

        // Ray-from = camera world-space translation from the active
        // camera's cameraRoot node. Returns false if the singleton
        // or its root isn't populated (e.g., very early load).
        bool GetCameraWorldPos(RE::NiPoint3& out)
        {
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (!cam)
                return false;
            if (!cam->cameraRoot)
                return false;
            out = cam->cameraRoot->world.translate;
            return true;
        }

        // Fill `out` with N sample world positions on `target`.
        // Skeleton nodes are preferred (they follow the animated
        // pose). When a node isn't found we skip it silently rather
        // than substituting the actor origin — filling with copies
        // of the origin would just be N redundant rays.
        //
        // Bbox extremes come from `Get3D()->worldBound` (an
        // NiBound: center + radius sphere). For a standing character
        // the bounding sphere is a decent approximation of "the
        // volume the mesh occupies"; sampling top/bottom/side/side
        // captures the sticking-out-of-cover cases skeletal nodes
        // alone might miss (weapon on back, extended cloak, etc.).
        std::size_t GatherSamplePoints(RE::TESObjectREFR* target, std::array<RE::NiPoint3, 12>& out)
        {
            std::size_t n = 0;

            if (auto* root = target->Get3D()) {
                for (const char* nodeName : kSkeletonNodeNames) {
                    if (n >= out.size())
                        break;
                    auto* node = root->GetObjectByName(nodeName);
                    if (!node)
                        continue;
                    out[n++] = node->world.translate;
                }

                const auto& bound = root->worldBound;
                // Only emit bbox extremes when the bound looks
                // meaningful (positive radius). NiBound is
                // uninitialized on freshly-attached 3D on some
                // paths — a zero radius would collapse all four
                // extremes back onto the center, which is wasted
                // rays but not incorrect.
                if (bound.radius > 1.0f && n + 4 <= out.size()) {
                    const auto& c = bound.center;
                    const float r = bound.radius;
                    // Vertical extremes (top of head / feet).
                    out[n++] = {c.x, c.y, c.z + r};
                    out[n++] = {c.x, c.y, c.z - r};
                    // Lateral extremes (widest silhouette from
                    // most camera angles).
                    out[n++] = {c.x + r, c.y, c.z};
                    out[n++] = {c.x - r, c.y, c.z};
                }
            }

            // Fallback: if we somehow got zero samples (no 3D
            // nodes and no bbox), seed with the actor's data.location
            // so the caller isn't returning false purely on
            // "no samples to test." In practice we shouldn't reach
            // this because IsAnyPartVisibleFromCamera short-circuits
            // on Get3D()==nullptr up front.
            if (n == 0) {
                out[n++] = target->GetPosition();
            }
            return n;
        }

        // Fire one raycast from `from` to `to` via TES::Pick and
        // return true if the ray reached the endpoint without being
        // stopped by geometry short of it.
        //
        // We check by hit-fraction rather than by identifying the
        // hit collidable's owning REFR. A ray targeted at a bone
        // node's world position lands at the bone's collision
        // shape a few units before the exact endpoint, so a strict
        // "hitFraction==1.0" test would reject legitimate visibility.
        // A fraction near 1.0 (kReachedFractionThreshold) means
        // either nothing was hit or the hit was at/very-near the
        // endpoint — both of which mean the sample point is
        // reachable in an obstruction sense.
        bool RaycastReachedEndpoint(const RE::NiPoint3& from, const RE::NiPoint3& to)
        {
            auto* tes = RE::TES::GetSingleton();
            if (!tes)
                return false;

            // Skyrim's Havok world uses a different unit scale than
            // the game-space coords we've been carrying around.
            // `bhkWorld::GetWorldScale()` gives the ratio to apply
            // to positions before passing them into the pick data.
            const float scale = RE::bhkWorld::GetWorldScale();

            RE::bhkPickData pd;
            pd.rayInput.from = RE::hkVector4(from.x * scale, from.y * scale, from.z * scale, 0.0f);
            pd.rayInput.to = RE::hkVector4(to.x * scale, to.y * scale, to.z * scale, 0.0f);
            // Default rayOutput fields are initialized to "no hit"
            // (rootCollidable=nullptr, hitFraction=1.0). We rely on
            // that: a no-hit result correctly reads as "reached."
            pd.rayOutput.hitFraction = 1.0f;

            tes->Pick(pd);

            return pd.rayOutput.hitFraction >= kReachedFractionThreshold;
        }
    } // namespace

    bool IsAnyPartVisibleFromCamera(RE::TESObjectREFR* target)
    {
        if (!target)
            return false;

        // Gate 1 — no 3D means the target isn't rendered anywhere,
        // full stop. Common for far / unloaded actors and the exact
        // "safe to teleport home" case the caller wants.
        auto* root = target->Get3D();
        if (!root)
            return false;

        RE::NiPoint3 cameraPos;
        if (!GetCameraWorldPos(cameraPos)) {
            // Camera not resolvable — fail closed. Caller has
            // other backstops (ReturnHome timeout) so a spurious
            // "not visible" here just costs us one tick.
            return false;
        }

        const auto targetPos = target->GetPosition();
        const float dx = targetPos.x - cameraPos.x;
        const float dy = targetPos.y - cameraPos.y;
        const float dz = targetPos.z - cameraPos.z;
        const float distSq = dx * dx + dy * dy + dz * dz;

        // Gate 2 — close-range visibility. Below the floor, we're
        // inside the region where raycasts behave badly. Treat as
        // visible so the caller's "sender still on-screen" logic
        // never trips a spurious teleport at pointe-blanke range.
        if (distSq <= kMinDistanceFloor * kMinDistanceFloor) {
            return true;
        }

        // Gate 3 — engine LOS as a POSITIVE-ONLY short-circuit.
        // Do NOT invert: a `false` from the engine can be the
        // FOV-cone false-negative case that motivated writing this
        // whole helper. Only the `true` result short-circuits.
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            bool losIgnored = false;
            if (player->HasLineOfSight(target, losIgnored)) {
                return true;
            }
        }

        // Gate 4 — fan of raycasts to sampled points on the target.
        std::array<RE::NiPoint3, 12> samples;
        const std::size_t nSamples = GatherSamplePoints(target, samples);
        if (nSamples == 0)
            return false;

        const bool debug = Settings::Get().debugMode;
        std::size_t reachedCount = 0;

        for (std::size_t i = 0; i < nSamples; ++i) {
            if (RaycastReachedEndpoint(cameraPos, samples[i])) {
                ++reachedCount;
                if (!debug) {
                    // Non-debug: short-circuit on first success —
                    // we only need "any part visible."
                    return true;
                }
                // Debug: keep counting so the summary log is
                // informative.
            }
        }

        if (debug) {
            logger::debug("CameraVisibility: target=0x{:X} samples={} reached={} "
                          "(cam=({:.0f},{:.0f},{:.0f}) target=({:.0f},{:.0f},{:.0f}) "
                          "dist={:.0f})",
                          target->GetFormID(),
                          nSamples,
                          reachedCount,
                          cameraPos.x,
                          cameraPos.y,
                          cameraPos.z,
                          targetPos.x,
                          targetPos.y,
                          targetPos.z,
                          std::sqrt(distSq));
        }

        return reachedCount > 0;
    }
} // namespace NarrativeEngine::CameraVisibility
