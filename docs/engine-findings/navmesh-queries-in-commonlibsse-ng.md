# Navmesh queries in CommonLibSSE-NG

## TL;DR

CommonLibSSE-NG exposes **no callable navmesh query of any kind** — no
reachability test, no closest-point-on-navmesh, no pathfinding entry
point, not even a "is this position walkable" helper. The navmesh
headers are pure data layout with zero `REL::Relocation` bindings, so
there is nothing to call.

What it *does* expose is the raw triangle data, reachable from any
loaded cell. That is enough to answer **containment** ("is there navmesh
under this point?") by testing the point against the triangles yourself.
It is not enough to answer **connectivity** ("is that navmesh the same
island the player is standing on?") without writing a graph walk.

`AmbushSpawnPoints::IsOnNavmesh` implements the containment half. The
connectivity half is deliberately not implemented — see *What we did
not build* below.

## What's actually in the headers

Six headers look promising and none of them help directly:

| Header | What's in it |
| --- | --- |
| `RE/N/NavMesh.h` | `NavMesh : TESForm, TESChildCell, BSNavmesh`. Overrides plus four save-related virtuals. |
| `RE/B/BSNavmesh.h` | The data: `vertices`, `triangles`, `extraEdgeInfo`, `doorPortals`, `meshGrid`, and more. |
| `RE/N/NavMeshInfoMap.h`, `RE/B/BSNavmeshInfoMap.h` | Layout only. |
| `RE/B/BSPathingCell.h`, `RE/P/PathingCell.h` | `PathingCell : BSPathingCell`, layout only. |
| `RE/B/BSPrecomputedNavmeshInfoPathMap.h` | Layout only. |

Confirmed mechanically — grepping every one of those files for
`REL::Relocation` returns nothing, which is the tell: CommonLibSSE-NG
binds engine functions through relocations, so a header with none has no
callable functions at all.

`RE::TES` is the other place you'd look, and it has `GetLandHeight`,
`GetCell`, `GetWaterHeight`, `GetLandMaterialType`, and `Pick` — useful,
but all about terrain and collision, not pathing.

## The data path that does work

```text
RE::TES::GetSingleton()->GetCell(pos)      // TESObjectCELL*
  -> cell->GetRuntimeData().navMeshes      // NavMeshArray*
     -> ->navMeshes                        // BSTArray<BSTSmartPointer<NavMesh>>
        -> mesh->vertices                  // BSTArray<BSNavmeshVertex>  (.location is NiPoint3)
        -> mesh->triangles                 // BSTArray<BSNavmeshTriangle> (.vertices[3] are indices)
```

Two things to get right:

- **`navMeshes` is not a direct member of `TESObjectCELL`.** It lives in
  the `RUNTIME_DATA` block, whose offset changed in 1.6.629. Reaching it
  as `cell->navMeshes` doesn't compile; use
  `cell->GetRuntimeData().navMeshes`.
- **Vertex coordinates are assumed world-space**, not cell-local, so the
  point-in-triangle test compares directly against a world position.
  This is the one assumption here that has **not** been confirmed in a
  running game yet. It matches how NAVM vertex data is described for
  exteriors, but if it turns out to be cell-local the symptom is
  unmistakable: the containment gate rejects *every* candidate and the
  spawn search reports `noNavmesh=16` at every radius. If that shows up
  in the log, subtract the cell's world-space origin before testing.

Containment is then a barycentric point-in-triangle test on XY plus a
vertical tolerance against the interpolated Z. A tolerance is required:
the navmesh surface and `GetLandHeight` routinely disagree by a few
units on sloped terrain, so an exact-Z test rejects valid ground. We use
±96 units — loose enough to absorb that, tight enough not to match the
floor below when standing on a bridge.

Watch for degenerate (zero-area) triangles; real navmeshes contain them,
and the barycentric denominator goes to zero. Guard on
`fabs(denom) < 1e-6`.

## What we did not build

**Connectivity.** Knowing a point is on navmesh does not mean an actor
there can reach the player. An unreachable ledge across a ravine is
perfectly good navmesh. Answering that properly means a BFS over the
triangle adjacency graph (`BSNavmeshTriangle::triangles[3]` holds
neighbor indices, `0xFFFF` for none) plus cross-mesh traversal through
`extraEdgeInfo` portals and `doorPortals` — and in exteriors the player
and the candidate are usually in *different* cells, so it has to cross
mesh boundaries to be worth anything.

That's a real piece of work with real risk, and it buys down a failure
mode that has two cheaper backstops.

**The post-spawn settle check** (`AmbushBeat`'s `Settling` COMPOSE
sub-phase) is the first. Containment tells you there is navmesh under a
*point*; it cannot tell you an actor placed there will come to rest on
it. So after the aliases fill, the beat waits ~1.5 s for gravity to act,
then compares each attacker's resting position against where it was put:
more than 300 units of vertical drift, or a resting position that fails
`IsOnNavmesh`, counts as not settled. Offenders are relocated once onto
the search winner's position — the best-validated point available — and
re-checked. Note that `Actor` overrides `SetPosition` with a second
`a_updateCharController` parameter; pass `true`, or the physics body
stays behind and the actor walks straight back.

**Abandon-by-timeout** is the second, and covers what the settle check
structurally cannot: an attacker that settled perfectly well on navmesh
that simply isn't connected to the player's. That ends the encounter
rather than wedging it, so a stranded spawn costs one wasted beat.

Between them, connectivity is worth revisiting only if field testing
shows stranded attackers are common rather than occasional.

## False leads

- **`RE::TES::GetLandHeight` is not a walkability test.** It returns the
  terrain height, happily, for the middle of a lake, the inside of a
  mountain, and a cliff face. It answers "how high is the ground" and
  nothing more. Use it to *place* a validated point, not to validate one.
- **Looking for `PathingRequest` / `MovementController` entry points.**
  These exist in the engine but aren't bound in CommonLibSSE-NG either,
  and driving the real pathing system to ask a yes/no question would be
  wildly disproportionate.
- **Assuming `NavMesh` has query methods because it's a `TESForm`.** It
  has four virtuals past the `TESForm` overrides and they are all about
  saving.
