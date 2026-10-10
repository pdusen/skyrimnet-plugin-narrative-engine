# TES::GetLandHeight answers only inside the attached cell grid

## TL;DR

`RE::TES::GetLandHeight` returns `false` for every position in a cell that is not in the
attached exterior grid, and the boundary is exactly the grid's edge — not a distance, not
view range, not LOD coverage. There is no landscape to read outside the grid and the engine
does not pretend otherwise.

Worse, **it writes `-2048.0` into the out parameter when it fails.** That is a plausible
Skyrim ground height, not an obvious sentinel, so a caller that trusts the float and ignores
the bool silently places things 2,048 units below sea level. Always branch on the return
value.

## The evidence

Probed from a running game (v0.6.1, Phase 16 Step 1), standing outdoors in Tamriel at
`(35879, -17024, -4204)` — cell `(8, -5)`, with 25 attached exterior cells in a grid of
length 5, so the attached block was cells `(6..10, -7..-3)`.

Four cardinal rays from the player, 19 samples each at 2,048-unit steps out to 38,912 units.
76 samples in all:

| Samples | `GetLandHeight` returned `true` | returned `false` |
| --- | --- | --- |
| 18 in an attached cell | 18 | 0 |
| 58 in an unattached cell | 0 | 58 |

**Zero disagreements in either direction.** Attachment predicts the answer perfectly.

The boundary falls on the grid edge on all four rays, with no transition zone:

| Ray | Last `true` | First `false` |
| --- | --- | --- |
| +X | offset 8,192 — cell (10, -5) | offset 10,240 — cell (11, -5) |
| -X | offset 10,240 — cell (6, -5) | offset 12,288 — cell (5, -5) |
| +Y | offset 8,192 — cell (8, -3) | offset 10,240 — cell (8, -2) |
| -Y | offset 10,240 — cell (8, -7) | offset 12,288 — cell (8, -8) |

Cells (11, -5), (5, -5), (8, -2) and (8, -8) are the first cell outside the 5x5 block in each
direction. The asymmetry between rays is only where in its cell the player happened to stand.

The 18 answers inside the grid are real terrain, not a constant: heights ran from -4,773.6 to
+2,689.1 against a player Z of -4,204, which is the mountains east of where the probe was
taken. And all 58 failures wrote exactly `-2048.0`, with the out parameter pre-set to `0.0f`
before each call — so the engine writes that value rather than leaving the float untouched.

Cost is negligible: all 76 calls completed inside the same logged millisecond.

## What this means

Any position query that has to work beyond the loaded grid needs its own elevation source.
`GetLandHeight` cannot be the fallback, because it does not answer there at all.

This is the same shape as the Phase 14 finding that `GetLandHeight` answers nothing while the
player is indoors: in both cases there is no landscape loaded to read, and interiors are just
the case where the grid is empty rather than merely finite.

`StuckRecovery::GroundPoint` and `IsStandable` are built on it and are therefore in-grid
queries by construction — which is correct for them, since an actor outside the grid is in low
process and not standing on anything the engine is simulating.

NarrativeEngine's approach chain takes the consequence: a bridge point outside the grid gets
the height its own chain line interpolates, plus a small upward buffer, and is never
validated. Precision there does not matter, because an actor warped outside the grid stays
put until background travel carries them in, and gets grounded the moment they become a real
loaded actor.

## False leads

- **`parentUseFlags` / LOD coverage.** Landscape LOD is rendered far past the attached grid,
  so it is tempting to assume the height data behind it is queryable. It is not; the rendered
  LOD mesh and `GetLandHeight`'s source are not the same data.
- **Reading the float without the bool.** `-2048.0` looks like an answer. Skyrim terrain
  genuinely sits at large negative Z — the player in this very probe stood at -4,204 — so
  there is no value you can test for that distinguishes "no data" from "deep terrain".
- **Probing one direction only.** A single ray that runs into the sea or off the worldspace
  edge would have produced an inconclusive run; the four-ray sweep is what makes the
  attachment correlation airtight.
