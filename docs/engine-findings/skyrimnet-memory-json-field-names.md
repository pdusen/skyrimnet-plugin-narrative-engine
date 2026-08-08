# SkyrimNet's memory JSON does not match its own documentation

`PublicGetMemoriesForActor` returns field names that differ from the ones its doc comment in
`PublicAPI.h` advertises. Three of the five documented names are wrong, and the wrongness fails
**silently** — so code written from the header looks correct, compiles, runs, and quietly discards
every row.

This has now bitten twice: once in the letter beat (Phase 4) and again in gossip harvesting
(Phase 13 Milestone 2), where it cost an in-game test run.

## The actual shape

A row returned by `PublicGetMemoriesForActor(formId, maxCount, contextQuery)`:

A real row, captured from a running game (content and id arrays abridged):

```json
{
  "id": 1478,
  "content": "We reached Paratus Decimius in the Mzulft Oculory — he mistook Varian for Gavros…",
  "type": "KNOWLEDGE",
  "tags": ["paratus decimius", "mzulft", "oculory", "eye of magnus", "…"],
  "importance_score": 0.9200000166893005,
  "decayed_importance": 0.04548140615224838,
  "age_hours": 1407.0,
  "game_time": 1275971.072388,
  "game_time_str": "1970-01-15T13:26:11Z",
  "creation_time": 1781146468.035227,
  "creation_time_str": "2026-06-10T22:54:28Z",
  "emotion": "concerned",
  "location": "Mzulft Oculory, Mzulft, Hold: Eastmarch",
  "actor_uuid": 7721022111582155498,
  "related_actors": [1025366256383505927, "…"],
  "related_event_ids": [4902, 4903, "…"],
  "is_active": true
}
```

| Documented in `PublicAPI.h` | Actually returned  | Notes                                              |
| --------------------------- | ------------------ | -------------------------------------------------- |
| `text`                      | `content`          |                                                    |
| `importance`                | `importance_score` | `decayed_importance` is also present — see below   |
| `timestamp`                 | `age_hours`        | **Real-world**, not game time. Use `game_time`.    |
| `id`                        | `id`               | correct                                            |
| `type`                      | `type`             | correct                                            |

The doc comment also omits most of the row. The real one includes `tags`, `game_time`,
`creation_time`, `creation_time_str`, `game_time_str`, `decayed_importance`, `related_actors`,
`related_event_ids`, `is_active` and more. **`tags` in particular is present** — tags written at
`AddMemory` time *can* be filtered on at read time, contrary to what its absence from the doc
comment implies.

## `age_hours` is real-world time — this is the dangerous one

`age_hours` is **wall-clock hours since the row was written**, derived from `creation_time`. It has
nothing to do with the game clock. A raw row dump:

```json
{ "id": 1478, "age_hours": 1407.0,
  "creation_time": 1781146468.0, "creation_time_str": "2026-06-10T22:54:28Z",
  "game_time": 1275971.07,       "game_time_str": "1970-01-15T13:26:11Z" }
```

1407 h is 58.6 days, and that memory was harvested on 2026-08-08 — exactly 58.6 real days after
its `creation_time`. In game it was under half a day old.

**Pick up a save after a two-month break and every memory in it reads as ancient.** Gossip
harvesting compared `age_hours` to an in-world window and rejected all 162 candidate memories as
"too old", every one of them between 58 and 85 "days". The correct field is:

```text
in-world age (seconds) = EventLogUtil::NowGameTimeSeconds() - row["game_time"]
```

`game_time` is game-seconds since game start, the same base
`Calendar::GetDaysPassed() * 86400` produces. `game_time_str` renders it as an offset from the Unix
epoch, so `1970-01-15` means 14.56 game days elapsed — a convenient sanity check.

`decayed_importance` decays on that same real-world clock, so it imports the identical bug through
a different door. Use `importance_score`.

### How it was confirmed

Not by reasoning about the calendar — an alternate-start mod makes the in-game date useless for
inferring elapsed game days. Instead, for all 162 logged ages, a single best-fit "now" was solved
for under each hypothesis:

| Hypothesis           | Max error | Mean error | Implied "now"       |
| -------------------- | --------- | ---------- | ------------------- |
| from `creation_time` | 0.064 d   | 0.027 d    | the actual run time |
| from `game_time`     | 14.03 d   | 3.44 d     | before the newest memory existed (negative age) |

The first recovered the wall-clock time of the test run to within 20 minutes from nothing but the
logged ages and the stored `creation_time` values.

## Corroboration

Three independent sources agree, and they were checked because the header alone cannot be trusted:

1. **The database schema.** `SkyrimNet/sql/migrations/0003_vector_memory_system.sql` defines the
   `memories` table with `content TEXT`, `importance_score REAL DEFAULT 0.5`,
   `memory_type TEXT DEFAULT 'EXPERIENCE'`, `game_time REAL`.
2. **Shipped code in this repo.** `SenderCandidatePool::FilterAndShapeMemories` reads
   `importance_score`, `content` and `age_hours`, and has done since the letter beat shipped.
   `LetterComposer.cpp` carries a comment saying the doc names "no longer match runtime output …
   verified against a captured memory payload — do not 'fix' back to the doc names."

   Note those consumers got the *names* right but still read `age_hours` as though it were game
   time — `SenderCandidatePool`'s sender watermark and `FilterDialogueByMemoryAge` in both
   composers each mix it with `Calendar::GetHoursPassed()`. Those are latent bugs of the same
   kind; they have not surfaced because their other uses of the field only ever *sort* by it,
   which is order-preserving and so indifferent to the unit.
3. **Observed runtime behaviour.** A gossip harvest sweep examined 162 memories and rejected all
   162 as below the importance floor, every one reporting `imp=0.00`.

## Why it fails silently

`nlohmann::json::value(key, default)` returns the default when the key is absent. A wrong field
name therefore yields `0.0`, `""` or `0` rather than an error, and the row is then rejected by a
threshold that looks like it is doing its job. Nothing logs, nothing throws, and the rejection
reason names the wrong culprit — "low importance" rather than "no such field".

Two consequences worth internalising:

- **A rejection counter is not evidence a filter works.** In the gossip case the log line
  faithfully reported `162 low-importance` while the real fault was upstream of the comparison.
  Logging the *value* alongside the verdict is what exposed it: `imp=0.00` on all 162, which is not
  a distribution any real corpus produces.
- **A wrong name can hide behind an earlier gate.** Only the first bad field surfaces; the rest sit
  latent until it is fixed. Gossip's `content` and `age_hours` bugs were invisible while
  `importance_score` killed every row first. Fix the whole shape at once, not one field per test
  run.

## Types in practice

The DB column is `memory_type` but the JSON key really is `type`, confirmed by a raw row dump.

Worth knowing before writing a type allowlist — the distribution on a real save (1427 memories):

| type         | count | avg importance |
| ------------ | ----- | -------------- |
| KNOWLEDGE    |   881 | 0.65           |
| EXPERIENCE   |   437 | 0.55           |
| RELATIONSHIP |    96 | 0.58           |
| LOCATION     |     6 | 0.73           |
| TRAUMA       |     5 | 0.78           |
| SKILL        |     2 | 0.60           |

`KNOWLEDGE` is both the largest bucket and the highest-scoring one — SkyrimNet files most narrative
material there, including long, specific, entirely gossip-worthy accounts. `JOY` does not appear at
all. An allowlist written from the enum names in the header will not survive contact with real
data; check the distribution first.

## Diary entries share this table

SkyrimNet folds diary entries into the same table this endpoint queries, typed `EXPERIENCE`, with
no server-side flag distinguishing them. The only discriminator is that the `content` string starts
with `Diary Entry:`. Any consumer that filters on memory type will pick them up. The letter and
visit composers filter them out deliberately; so does gossip harvesting.

## Rule

Do not write a SkyrimNet response parser from `PublicAPI.h`'s doc comments. Check the SQL schema
under `SkyrimNet/sql/migrations/`, check an existing consumer in this repo, or capture a live
payload — and when a filter rejects everything, log the parsed value next to the verdict before
believing the verdict.
