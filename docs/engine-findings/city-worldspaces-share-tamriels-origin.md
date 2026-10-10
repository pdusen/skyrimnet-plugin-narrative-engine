# City worldspaces share Tamriel's origin, and the parent-use flags do not say so

## TL;DR

Whiterun, Riften, Solitude, Markarth and Windhelm are each their own
`TESWorldSpace`, but their coordinates are Tamriel's coordinates. A position
read inside Whiterun can be compared against a position in Tamriel, routed
against Tamriel's road graph, and used as a bearing, with no conversion.

What establishes that is the **parent worldspace link** (`TESWorldSpace::parentWorld`),
not the parent-use flags. `ParentUseFlag::kUseLandData` looks like the right
question and is not: **Markarth does not set it** and is still in Tamriel's
frame.

## The evidence

A city gate and the spot it puts you on are one doorway measured twice: the
door reference stands in the city worldspace, and `ExtraTeleport`'s landing
position is recorded in Tamriel. If the two worldspaces measured from
different origins, those two readings would be tens of thousands of units
apart. Measured over every load door in the Spriggit export whose landing is
at exterior scale:

| Worldspace | Gates | Smallest gap | Largest gap |
| --- | --- | --- | --- |
| WhiterunWorld | 1 | 124 | 124 |
| RiftenWorld | 4 | 76 | 479 |
| SolitudeWorld | 9 | 312 | 792 |
| WindhelmWorld | 2 | 26 | 123 |
| MarkarthWorld | 2 | 103 | 160038 |

A gateway is 100–200 units deep, so those are the same doorway. Markarth's
second reading is a door that lands somewhere other than Tamriel and is not a
counter-example; its gate reads 103.

The cell grid says the same thing independently. `WhiterunOrigin` is at grid
(4, -2) and holds objects at x ≈ 20372, y ≈ -7896 — which divided by 4096 is
(4, -2), the standard convention, and is where Whiterun sits on Tamriel's map.

## Why the flags mislead

`PNAM` / `parentUseFlags` says which **data** a child borrows from its parent —
landscape, LOD, water, climate, sky. It does not say which **origin** the child
measures from. The two happen to coincide for four cities and not for the
fifth:

| Worldspace | UseLandData | UseMapData | Same origin as Tamriel |
| --- | --- | --- | --- |
| WhiterunWorld | yes | yes | yes |
| RiftenWorld | yes | yes | yes |
| SolitudeWorld | yes | yes | yes |
| WindhelmWorld | yes | yes | yes |
| MarkarthWorld | **no** | yes | **yes** |

Markarth is carved into a cliff and supplies its own landscape rather than
drawing Tamriel's, which is exactly what clearing `UseLandData` means. It says
nothing about where the city is.

Nor does `UseMapData` work as a substitute — Blackreach, Sovngarde and most
dungeon worldspaces set it too.

## What to use instead

Walk `parentWorld` to its root and compare roots:

```cpp
RE::TESWorldSpace* GroundFrameOf(RE::FormID worldSpace)
{
    auto* ws = RE::TESForm::LookupByID<RE::TESWorldSpace>(worldSpace);
    for (int depth = 0; ws && ws->parentWorld && depth < 8; ++depth) {
        ws = ws->parentWorld;
    }
    return ws;
}
```

Two worldspaces share a frame when `GroundFrameOf` returns the same record for
both. That admits Tamriel and every city under it, and admits two cities to
each other. It rejects Solstheim, whose `parentWorld` is null, and Apocrypha,
whose chain roots at Solstheim rather than Tamriel — neither of which offers a
bearing to somebody standing in Skyrim.

It also admits Blackreach and Sovngarde, which root at Tamriel while meaning
nothing against it. That is knowingly accepted: no unique, memory-carrying,
visit-eligible NPC lives in either, and the cost of a wrong acceptance (a
visitor approaching from an odd direction) is far below the cost of a wrong
rejection (the beat declining outright), which is the bug this replaced.

## A city's location is on the worldspace, not on its cells

Not one of WhiterunWorld's 113 exterior cells fills `XLCN`. The field is set
once, on the worldspace record, where it reads `WhiterunLocation` —
`TESWorldSpace::location` at offset 0x228.

So `TESObjectCELL::GetLocation()` answers for a visitor indoors at the
Bannered Mare, whose interior cell does name its location, and for nobody
standing out in the street. Anything resolving "which place is this NPC in"
for a city dweller has to ask the worldspace as well, and `HomeMarkerBearing`
tries it as the rung directly under the cell.

## Where this bit us

`VisitArrivalPoint::Find` declined every visit whose sender lived inside a
walled city while the player was anywhere else in Skyrim. The cross-worldspace
branch searched the loaded cell grid for a door joining the two, which can only
succeed when the player is standing at the city — so a sender in Whiterun and a
player in Eastmarch produced `tier=none (arrival_no_point)` every time. Five
cities hold a large share of the NPCs worth visiting, so this read as "NPC
visits are always failing".

The fix anchors such a sender on their home location's map marker and routes
from that, which needs exactly the frame guarantee above. See
`HomeMarkerBearing` in `src/VisitArrivalPoint.cpp`.

## Not authoritative for

Mod-added worldspaces. A mod city that declares no parent is rejected by this
rule even if its author placed it in Tamriel's coordinates, and one that
declares Tamriel as parent is accepted even if it does not. Both are the right
default — the parent link is the only statement the record makes about it —
but neither is a guarantee about content outside the vanilla masters.
