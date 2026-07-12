#pragma once

#include <RE/Skyrim.h>

// CameraVisibility — "could the player see this actor if they turned
// to face them" check. Distinct from `RE::Actor::HasLineOfSight`,
// which additionally gates on the player actor's field-of-view cone
// and therefore returns false whenever the camera happens to be
// pointed away from the target — even in open air with nothing
// between them.
//
// We want the facing-independent question because our callers
// (currently just the ReturnHome LOS-lost gate in NPCVisitBeat)
// want to know whether teleporting the target home would be
// visually noticeable to the player at all — which depends on
// obstruction geometry, not on where the camera currently points.
//
// Algorithm (cheapest gate first):
//   1. `actor->Get3D() == nullptr` → false. Not 3D-loaded, cell is
//      detached, actor isn't being rendered, nothing to see.
//   2. `distance(camera, actor) < kMinDistanceFloor` → true. Below
//      the floor, a from-camera raycast may start inside the actor's
//      own collision or inside cover; both cases produce spurious
//      results. We take "close = visible" as the safe default.
//   3. `player->HasLineOfSight(actor, _)` → true if it says yes.
//      Trusted as a POSITIVE short-circuit only. The engine call
//      does the raycast plus a FOV cone test; a `true` result means
//      both were satisfied and we're done. A `false` result may
//      just mean "outside the FOV cone" (that's the bug we're
//      here to work around), so we do NOT return early on false —
//      we fall through to the fan.
//   4. Fan of ~9 raycasts from camera position to sample points
//      distributed over the target: skeletal nodes (head, hips,
//      shoulders, hands) when available, plus a handful of bbox
//      extremes from `Get3D()->worldBound`. If ANY ray reaches its
//      sample point without being obstructed by non-target geometry,
//      the actor has at least one visible sliver and we return true.
//
// Threading: main thread only. Touches Havok (`RE::TES::Pick`) and
// the player scenegraph.
namespace NarrativeEngine::CameraVisibility
{
    // Returns true iff the player could see any part of `target`
    // by turning to face them (unobstructed line of sight from the
    // camera exists to at least one sampled point on the ref).
    // Returns false if `target` is null, has no 3D, or is fully
    // occluded.
    //
    // Accepts a TESObjectREFR* (rather than Actor*) so the
    // ReturnHome watchdog can call it on the alias's raw ref
    // without casting. Skeleton-node sampling is best-effort — for
    // non-actor refs, the NPC-prefixed node names simply won't
    // resolve and the fan falls back to bbox extremes, which is
    // still meaningful for any collidable mesh.
    //
    // Never throws; on any failure path (missing camera, missing
    // player, Havok call failure) returns false — the ReturnHome
    // caller's other checks (distance floor, timeout backstop)
    // ensure the visit still resolves.
    bool IsAnyPartVisibleFromCamera(RE::TESObjectREFR* target);
}
