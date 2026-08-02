# Phase 11 — Ambush Beat

A ground-up reimplementation of the ambush beat. The Phase 03 version shipped, ran, and taught us where its
design was wrong; it was removed wholesale ahead of this phase (see **Prior removal** below). This phase builds
the replacement on a different spine: C++ owns spawning, positioning, and lifecycle; the quest owns persistence
and state; the attacker roster is chosen by the Director from a game-state-filtered menu instead of being baked
into ESP records.

Four things change relative to the old implementation:

1. **Alias fill can no longer silently fail the whole beat.** Attacker aliases are `Optional` with no fill rule,
   force-filled from C++ after quest start. The old design's find-a-marker → create-six-refs-at-marker chain had
   a single point of failure that took the entire encounter with it.
2. **Spawn points are computed, not discovered.** A geometric ring search around the player replaces the
   "find an authored marker of an approved type inside the player's current Location" alias rule.
3. **The Director's chosen attacker count is honored.** The old beat parsed `bandit_count`, logged it, and then
   spawned six regardless, because six alias slots either all filled or all failed.
4. **The attacker roster is game-state dependent.** Bandits are one group among several. The Reach can field
   Forsworn; a player sworn to one side of the civil war can be hunted by the other.

---

## Why this phase exists

The old ambush beat had one structural flaw that produced most of its observed failures, and three design gaps
that made it less useful than it should have been.

The structural flaw: everything hung off `SpawnMarker`, a Find-Matching-Reference alias gated on five stacked
conditions (marker type in an approved FormList, inside the player's current `Location`, 3000–8000 units out,
no line of sight, exterior). All six attacker aliases were `Create Reference to Object → At: SpawnMarker`. When
`SpawnMarker` failed to fill — no approved marker in the loaded area, none inside the player's Location, all of
them in view, or the player standing somewhere with no `Location` at all — every attacker alias failed with it.
`EnsureQuestStarted` still returned true, the beat advanced to `RUNNING`, and it polled `IsCompleted()` forever
because completion was driven by `OnDeath` on attackers that had never existed. The beat wedged until aborted.
Alias fill is also one-shot at quest start, so there was no retry.

The three gaps:

- **Fixed six-slot roster.** `bandit_count` was unusable by construction.
- **Marker-dependent placement.** Even when it worked, spawn position was wherever an approved marker happened
  to sit, not where the encounter would read best.
- **Hardcoded attackers.** The leveled base forms lived in the alias records themselves
  (`LCharBanditMelee1HTank`, `LCharBanditMeleeAny`, `LCharBanditMissile`), so "Forsworn instead of bandits" was
  not expressible at any price.

### Prior removal

The old implementation was removed in its entirety before this phase begins. That removal was deliberately
staged to protect existing testers' saves:

- `src/AmbushBeat.cpp`, `include/AmbushBeat.h`, all `Plugin.cpp` wiring, the `BeatRegistry` enable gate, and
  every `ambush*` field in `Settings` / `NarrativeEngine.ini` — deleted.
- `_ne_BanditAmbushTravel` (`000802`) — record deleted.
- `_ne_BanditAmbushQuest` (`000800`) — **kept** as a stage-only shell (stages 0/10/200, no aliases, no scripts,
  no fragments). Keeping the record means a save's ChangeForm for that FormID still resolves to a real form
  instead of being discarded on load.
- All three `.psc` files deleted; `build.ps1` gained a prune pass so their orphaned `.pex` no longer ship.
- `_ne_AmbushSpawnMarkerTypeList` (`000801`) — deleted, then **restored** once it turned out `_ne_VisitQuest`'s
  `SpawnMarker` alias depended on it. Despite the ambush-flavored EditorID the list was shared by both beats,
  and deleting it left that alias's first fill condition (`IsInList <list> == 1`) pointing at nothing. Because
  the alias is not flagged `Optional`, it could never fill and `EnsureQuestStarted` failed outright — NPC Visit
  was dead on every build between the removal commit and the restore. The record is back at `000801` with its
  original 32 entries and a beat-neutral EditorID, `_ne_SpawnMarkerTypeList`.

**`000800` and `000802` are retired.** This phase allocates fresh FormIDs starting at `000831`. A save holding
a ChangeForm for a retired FormID must never later find a different record type sitting there. `000801` is not
retired — it holds the same record, with the same contents, that it always held.

---

## Scope

### In scope

- A new **`AmbushBeat`** implementing `IBeat`, registered in `BeatRegistry`, `Name()` = `"ambush"`.
- New ESP content: quest `_ne_AmbushQuest` with eight `Optional` attacker aliases and a forced `PlayerRef`
  alias; travel package `_ne_AmbushApproach`; Papyrus script `_ne_AmbushQuest.psc` carrying exactly two
  functions.
- A new **`AmbushAttackerGroups`** module plus a user-editable data file,
  `Data/SKSE/Plugins/NarrativeEngine/AttackerGroups.ini`. Each group carries display metadata, a roster of
  vanilla leveled-character forms, and declarative eligibility constraints over current game state. The module
  owns parsing, per-group validation, and the `EligibleGroups()` / `Find()` lookups; it authors no groups
  itself. Adding or replacing groups is a text edit with no rebuild.
- A new **`AmbushSpawnPoints`** module: geometric spawn-point search around the player. Ring sampling at the
  Director's requested distance, validated for exterior / loaded / navmesh-reachable / out-of-view, ranked,
  and clustered for N attackers.
- C++-side spawning via `RE::TESDataHandler::CreateReferenceAtLocation`, alias fill via VM dispatch of
  `FillAttackerSlot`, verify-on-next-tick readback, `Actor::EvaluatePackage()` to bind the approach package.
- Full four-state lifecycle with completion on all-dead, plus abandon-on-distance and abandon-on-timeout —
  the old beat could only complete via all-dead, so a fleeing player wedged it.
- `ambush_attacker_candidates` injection into the beat-select prompt, following the existing
  `letter_sender_candidates` / `visit_sender_candidates` pattern in `BuildBeatSelectPromptContext`.
- A per-beat cosave record under a **new** type ID (`'NAMB'`), carrying the cooldown stamp and the chosen
  group id. `'NBAM'` is permanently retired.
- New `[Beats]` INI keys for count / distance / cooldown / engage / abandon tuning.

### Deferred (explicitly out)

- **Reusing the retired records.** `_ne_BanditAmbushQuest` stays an inert shell; nothing in this phase touches
  it.
- **Ambusher dialogue, taunts, or SkyrimNet-driven negotiation.** The encounter resolves in vanilla combat.
  Attackers are ordinary hostile NPCs with no narrative surface beyond their group identity.
- **Non-hostile ambush variants** (shakedown, toll demand, surrender-or-fight). The beat's polarity is `Raise`
  and its only outcome is a fight.
- **Scanning a directory of group files.** One file, one location. Letting other mods drop
  `AttackerGroups/*.ini` alongside ours is a natural later extension the format already supports, but this
  phase reads exactly one file.
- **Runtime reload of the group file.** Read once at `kDataLoaded`; editing it requires a restart.
- **Eligibility beyond the shipped key vocabulary.** No weather, moon-phase, or crime/bounty rules, and no
  boolean-expression grammar — keys AND together and there is no `AnyOf`, which is why the two Vigilant
  groups ship as separate entries. Unknown keys are ignored rather than erroring, so adding keys later is
  backward-compatible; the vocabulary grew twice during design for exactly that reason.
- **Reward / loot shaping.** Attackers carry whatever their leveled lists give them.
- **Ambushes anywhere but the open road and wilderness.** See **Where ambushes may happen** below.
- **Reactive difficulty scaling** beyond what the vanilla leveled lists already do.
- **Any change to `Region`, `HoldGrid`, `LocationKeywords`, or `CameraVisibility`.** All four are consumed
  as-is.

---

## Design overview

### Why aliases, and why `ForceRefTo` is safe here

Spawned references are held in quest aliases rather than tracked by FormID in the cosave. Three things fall out
of that choice:

- **Persistence.** Alias-held references are persistent references. They survive cell unload and cell reset and
  are saved with the quest. FormID tracking would require the cosave to carry the in-flight roster and
  re-acquire it on every load.
- **Package binding.** The alias carries `_ne_AmbushApproach` in its `PackageData`, and the engine instances the
  package per alias instance (`BGSRefAliasInstanceData::instancedPackages`). Binding an AI package to an
  arbitrary spawned actor with no alias would require either `AIProcess::SetRunOncePackage` or a RefCollection
  - marker-faction workaround; neither is needed.
- **Cleanup.** The set of things to tear down is enumerable by walking the aliases, not by trusting a list we
  maintained ourselves across saves.

Phase 04 abandoned `ForceRefTo` for the letter sender and chose a marker faction instead. That decision does
**not** transfer here, for two specific reasons recorded in
[`PHASE_04_LETTER_POOL_AND_NPC_LETTER_ACTION.md`](PHASE_04_LETTER_POOL_AND_NPC_LETTER_ACTION.md):

- Phase 04's failure (a) was `ForceRefTo` **on a stopped quest** — the engine's fill pass at quest start could
  discard the force. This phase fills **after** `EnsureQuestStarted` has returned, which is the
  documented-supported ordering.
- Phase 04's failure (b) was that the CK's Fill Type UI offers no truly-empty option, and its nearest
  approximation (`Specific Reference` with nothing picked) marks the alias fill-**failed**, poisoning dependent
  fill rules. That is a Creation Kit UI limitation. Our attacker aliases have no dependents, and `Optional`
  with no fill rule is the shape we author.

`ForceRefTo` has no native binding in CommonLibSSE-NG — `RE::BGSRefAlias` (`RE/B/BGSRefAlias.h`) exposes only
`GetReference()` and `GetActorReference()`. So the fill goes through a two-function Papyrus script on the quest,
dispatched with the existing `QuestUtils::VMDispatchOnQuest`. `RE::Actor::StartCombat` has no native binding
either, which is the second function's reason to exist. Everything else the beat needs is native.

`VMDispatchOnQuest` is fire-and-forget — it queues the call and returns whether queuing succeeded, not whether
the call did. So COMPOSE dispatches the fills on one tick and **reads the aliases back on a later tick** to
confirm. This is the same verify-after-dispatch shape `NPCVisitBeat` already uses for its alias fills.

### Beat shape

Standard four-state `IBeat` lifecycle. COMPOSE is multi-step and runs as a sub-state machine, mirroring
`NPCLetterBeat`'s structure:

| Sub-phase         | Work                                                                              |
| ----------------- | --------------------------------------------------------------------------------- |
| `SelectingPoints` | Resolve group + count; run `AmbushSpawnPoints::Find`. Failure → CLEANUP.          |
| `StartingQuest`   | `EnsureQuestStarted`; quest self-advances stage 0 → 10.                           |
| `Spawning`        | `CreateReferenceAtLocation` per attacker; VM-dispatch `FillAttackerSlot(i, ref)`. |
| `VerifyingFill`   | Read back `BGSRefAlias::GetReference()` on slots `0..N-1`. All present → proceed. |
| `Arming`          | `SetActorValue(kAggression, 0)`, `EvaluatePackage()` per attacker; stage 10 → 20. |

RUNNING polls at the same 5 s cadence the old beat used (20 ticks at the 250 ms master poll). Each poll walks
the filled alias slots and classifies each attacker as alive / dead / gone. Three completion routes:

- **All dead** — the intended outcome. Stage 20 → 200, → CLEANUP, cooldown stamped.
- **Abandoned by distance** — every surviving attacker is beyond `iAmbushAbandonDistanceUnits` from the player.
  The player outran it. → CLEANUP, cooldown stamped.
- **Abandoned by timeout** — `iAmbushMaxDurationSeconds` of unpaused real time elapsed. → CLEANUP, cooldown
  stamped.

RUNNING also drives the engage handoff: any attacker inside `iAmbushEngageDistanceUnits` that hasn't yet
engaged gets `SetActorValue(kAggression, 2)` natively plus a VM-dispatched `EngageAttacker(i)`. This replaces
the per-attacker `RegisterForSingleUpdate` polling loop the old alias script ran, per
`feedback_tick_driven_accumulators`.

### Stale-state self-validation

The new beat registers under the same name the old one used (`"ambush"`). `BeatSystem::OnLoad`
(`src/BeatSystem.cpp:497`) resets `BEAT_RUNNING` to idle when the named beat isn't in the registry — that
handled the removal build, but it will **not** fire once a beat named `"ambush"` exists again. A tester
updating straight from the pre-removal build to this one can therefore restore `BEAT_RUNNING` / `RUNNING` with
no world state behind it.

Two defenses, both required:

- **New cosave type ID.** `'NAMB'`, not `'NBAM'`. Stale ambush payloads hit `Plugin.cpp`'s default arm and are
  skipped with a warning rather than being misread as the new shape.
- **RUNNING self-validation.** The first RUNNING tick after a load checks that `_ne_AmbushQuest` is actually at
  stage 20 and that at least one attacker alias is filled. If not, it transitions straight to CLEANUP. This is
  wanted regardless of the collision case — it's also the recovery path for a crash or a forced abort.

### Attacker groups

The group table is **data, not code**. It ships as a structured configuration file that the plugin parses at
`kDataLoaded`; `AmbushAttackerGroups` owns the parse, the validation, and the runtime lookup, but authors
nothing. Adding a group — or replacing the whole set — is a text edit with no rebuild.

#### Why INI, and why its own file

Evaluated against two criteria: easy to extend by hand, and hard to break by accident.

**The failure mode decides it.** JSON and YAML both fail the *entire file* on a single bad character — one
trailing comma, one tab where spaces were expected. For a file whose entire purpose is "users add entries,"
that is the wrong trade: a typo in someone's fifth group must not cost them the four that parsed fine.
SimpleIni fails per line, and this loader goes further and fails **per group** — each section validates
independently, invalid ones are skipped with a specific logged reason, and everything else still loads.

INI also has no brackets, commas, quotes, or nesting to balance, so the three classic config traps (JSON's
trailing comma, YAML's hard-error tabs, YAML's `no` parsing as `false`) have no equivalent here. It needs no
new dependency — `simpleini` is already in `vcpkg.json` and is already how `Settings` reads config — and it is
the idiom every Skyrim user knows from every other SKSE plugin.

Runners-up, for the record: **JSON** needs no new dependency either and nlohmann does support comments
(`ignore_comments`, `json.hpp:4046`), but the all-or-nothing parse failure is disqualifying. **YAML** is the
most readable when correct and the most fragile when hand-edited, and would add a dependency — there is no
YAML parser in `vcpkg.json` today. **CSV** is rejected outright: a missing comma silently shifts every column,
producing wrong data instead of a loud error.

It is a **separate file** from `NarrativeEngine.ini` because that file is tunables and this is content. A
broken group file must not be able to take the settings surface down with it, and users should be able to
swap group sets independently.

#### File format

`Data/SKSE/Plugins/NarrativeEngine/AttackerGroups.ini`, shipped from
`statics/SKSE/Plugins/NarrativeEngine/AttackerGroups.ini`.

```ini
; Each [Group:<id>] section defines one attacker group. <id> is what the
; Director returns as attacker_group -- keep it snake_case and unique.
;
; A group that fails validation is SKIPPED with a reason in the log; the rest
; of the file still loads. One bad group cannot break the others.

[Group:forsworn]
Enabled     = true
DisplayName = Forsworn
Flavor      = Reachmen raiders who treat every road through the Reach as theirs.

; Roster. Values are EditorIDs of leveled-character (LVLN) records.
; LineForm may be repeated to draw rank-and-file from more than one list.
LineForm    = LCharForswornMelee1H
RangedForm  = LCharForswornMissile
LeaderForm  = LCharForswornBossMelee1H

; Eligibility. Every key is optional; absent means "no constraint". Repeating a
; key ORs its values; different keys AND together. A group with no eligibility
; keys at all is always eligible.
RequireHold = ReachHoldLocation

[Group:stormcloak_raiders]
Enabled       = true
DisplayName   = Stormcloak raiders
Flavor        = Sons of Skyrim hunting the Legion's sworn allies.
LineForm      = LCharSoldierSons
RequireGlobal = CWPlayerAllegiance = 1
```

**Eligibility vocabulary**, all optional and AND-ed together:

| Key                                                | Matches against                    |
| -------------------------------------------------- | ---------------------------------- |
| `RequireHold` / `ForbidHold`                       | `Region::ForPlayer().holdFormID`   |
| `RequirePlayerInFaction` / `ForbidPlayerInFaction` | `Actor::IsInFaction` on the player |
| `RequirePlayerKeyword` / `ForbidPlayerKeyword`     | `Actor::HasKeyword` on the player  |
| `RequireQuestComplete` / `ForbidQuestComplete`     | `TESQuest::IsCompleted()`          |
| `RequireQuestStage` / `ForbidQuestStage`           | `TESQuest::GetCurrentStageID()`    |
| `RequireGlobal`                                    | `TESGlobal::value`                 |
| `RequireGameHour`                                  | `RE::Calendar::GetHour()`          |
| `MinPlayerLevel` / `MaxPlayerLevel`                | `Actor::GetLevel()`                |

Unknown keys log a warning and are ignored, so a group file written for a later build never hard-fails an
earlier plugin.

**`CooldownGameHours`** is a per-group cooldown, defaulting to
`kDefaultGroupCooldownHours` (24). A group is stamped when an ambush using it **actually spawns** — at `Arming`,
the first point where attackers demonstrably exist in the world — so a compose that fails anywhere earlier never
costs a group its turn. While cooling down the group is filtered out of `EligibleGroups`, which means the
Director never sees it rather than being asked to avoid it. `0` disables the cooldown for that group.

This is orthogonal to `iAmbushPerBeatCooldownGameHours`, and the two stack: the beat-level one governs how often
*any* ambush fires, this one how often a *particular* group can be the one that does.

Stamps are keyed by group id, not by index, so the table survives the user reordering, adding, or removing
groups between sessions; an id that no longer exists is simply never consulted. They ride in the `'NAMB'` cosave
record, which went to **version 2** to carry them. Version 1 records still load and simply arrive with no
cooldowns — every group reads as ready, which is the safe direction for a one-time upgrade.

**`Enabled`** is a master switch, not an eligibility key: `Enabled = false` keeps a group's configuration
intact but removes it from consideration entirely. Every shipped group sets it explicitly to `true` so the
line is already there to flip — a player who dislikes one group edits one word instead of deleting or
commenting out a block they may want back later. A disabled group is still **parsed and validated**, so
re-enabling it a month later can't surprise you with an error that was silently hiding; it just never reaches
`EligibleGroups`. Omitting the key entirely defaults to `true`.

**Hold matching** resolves the named EditorID to a `BGSLocation` and compares its FormID against
`Region::ForPlayer().holdFormID`. That call is the composed answer: `src/Region.cpp:151` tries
`HoldGrid::LookupPlayer()` — the precomputed exterior-cell → hold grid — first, and falls back to the
parent-location walk only when the grid has no entry. This matters for this beat specifically, because
ambushes fire in open wilderness, and open wilderness is exactly where the parent-walk alone leaves holes
(see the rationale in `include/HoldGrid.h`). Do not call `HoldGrid` directly; `Region` already composes both.

When no hold resolves at all — an unclassified or mod-added worldspace — `holdFormID` is `0`, every group
carrying a `RequireHold` constraint becomes ineligible, and the unconstrained fallback group carries the tick.
That is the correct behavior and it is why an unconstrained group must always exist.

**Global matching** takes the form `RequireGlobal = <GlobalEditorID> <op> <value>`, where `<op>` is one of
`=`, `!=`, `>`, `<`, `>=`, `<=`. Whitespace around the operator is optional. The key is repeatable, and each
occurrence must hold. This exists because a lot of Skyrim progression state — including the civil-war
allegiance the shipped groups depend on — lives in `GlobalVariable` records rather than in faction membership.

**Hour matching** takes the form `RequireGameHour = <start>-<end>`, in 24-hour game time, read from
`RE::Calendar::GetHour()`. The window **wraps across midnight**, so `20-6` means 8pm through 6am — which is
the case that actually matters, and getting it wrong would silently invert a nocturnal group's schedule. The
key is repeatable, and repeated windows OR together.

**Keyword matching** resolves the named EditorID to a `BGSKeyword` and tests `Actor::HasKeyword` on the
player. This is how vampirism is detected: turning swaps the player's race to `<Race>Vampire`, and all twelve
vanilla vampire races carry the `Vampire` keyword `0A82BB` — as does Dawnguard's `DLC1VampireBeastRace`, so
the check holds in Vampire Lord form too. It generalizes to anything the game marks with a keyword.

**Quest matching** comes in two forms. `RequireQuestComplete = <QuestEditorID>` tests
`TESQuest::IsCompleted()` and is the one to reach for by default — no magic stage numbers, and it handles
quests with several completion stages automatically (`MQ201` completes at either 250 or 255, and
`RequireQuestComplete` doesn't care which). `RequireQuestStage = <QuestEditorID> <op> <n>` compares
`GetCurrentStageID()` for finer control, with the caveat that it reads the *latest* stage rather than "was
this stage ever reached" — CommonLibSSE-NG exposes no `GetStageDone` equivalent, and vanilla stages are
usually but not always monotonic. Note that `TESQuest::IsRunning` is **not** used by either form: it shares
the `kEnabled` bit with `IsEnabled` and returns true for never-started enabled quests.

**EditorIDs, not `Plugin.esm|0xFormID`.** The codebase already depends on `LookupByEditorID` throughout
(`Region`, `LocationKeywords`, `LetterPool`, `NPCVisitBeat`), so powerofthree's Tweaks is already a documented
dependency. EditorIDs are self-documenting, findable in xEdit or the CK, and spare the user any load-order
reasoning.

#### How the civil-war groups detect allegiance

This was traced through the vanilla scripts rather than assumed, and the answer changed the design.

`CWScript::SetPlayerAllegiance(int FactionToJoin, int MakePlayerInvolved)` (`CWScript.psc:1760`) does three
things when the player picks a side:

```papyrus
CWPlayerAllegiance.value = FactionToJoin        ; GlobalShort: 1 = Imperials, 2 = Sons
Game.GetPlayer().AddToFaction(getFaction(iImperials))
Game.GetPlayer().RemoveFromFaction(getFaction(iSons))
```

`getFaction()` (`CWScript.psc:1517`) returns `CWImperialFaction` / `CWSonsFaction` when its
`ReturnNPCFaction` parameter is left at its `false` default. So **yes — the player genuinely is added to
`CWImperialFaction` / `CWSonsFaction` on joining**, and removed from the opposite one. It's called from the
`CW` master quest's stage fragments at four sites (`QF_CW_00019E53.psc:278`, `:306`, `:394`, `:507`), covering
both join paths. Vanilla's own allegiance test, `CWScript::GetActorAllgeiance()` at `:1721`, is exactly
`IsInFaction(CWImperialFaction) || IsInFaction(GovImperial)`.

**But the shipped groups key off the global instead**, because faction membership has one bounded lie in it.
`CWAttackCityDragonReachDoor.psc:17` adds the player to `CWImperialFaction` **regardless of which side they
are on**, as an explicitly temporary hack during the Battle for Whiterun — its own comment says
*"temporarily add them to the Imperial faction so the stop combat prevents people from following them into the
castle."* It's undone at `CWAttackCityConfrontationTriggerScript.psc:29`, but only when the player trips that
trigger. Save inside that window and a Stormcloak player reads as Imperial.

`CWPlayerAllegiance` (`GlobalShort`, `023A43:Skyrim.esm`, default `0`) has no such window: `0` undecided,
`1` Imperial, `2` Sons, written once in `SetPlayerAllegiance` and never temporarily flipped. One global read,
no edge case.

`RequirePlayerInFaction` stays in the vocabulary regardless — it is the right tool for guild membership
(Thieves Guild, the Companions, the College) where there is no equivalent global. It simply isn't the best
tool for this particular question.

#### Validation contract

Per group, all failures skip that group only and log a reason naming the section:

- `<id>` non-empty, snake_case, and unique within the file.
- At least one `LineForm`, and every named roster form resolves to an actual `RE::TESLevCharacter`. When a
  form resolves to something else, log the real form type — that catches an `NPC_` EditorID pasted in by
  mistake.
- Every `RequireHold` / `ForbidHold` value resolves to a `BGSLocation` **carrying the `LocTypeHold` keyword**.
  The keyword check is what catches a city location (`WhiterunLocation`) named where a hold
  (`WhiterunHoldLocation`) was meant — without it that group would simply never match, silently.
- Every faction key resolves to an `RE::TESFaction`.
- Every `RequireGlobal` value parses as `<EditorID> <op> <number>` with a recognized operator, and the named
  EditorID resolves to an `RE::TESGlobal`.
- Every `RequireGameHour` value parses as `<start>-<end>` with both bounds in `[0, 24)`. Equal bounds are an
  error rather than a zero-width or all-day window — the intent is ambiguous, so refuse to guess.
- Every keyword key resolves to an `RE::BGSKeyword`, and every quest key to an `RE::TESQuest`.
- `Enabled`, if present, parses as a boolean. A malformed value is an error rather than a silent `false` —
  a group vanishing because someone typed `Enabled = yes` would be near-impossible to diagnose.
- `MinPlayerLevel` / `MaxPlayerLevel` parse as integers and don't invert.

The loader closes with one summary line, e.g.
`AttackerGroups: loaded 14 of 15 groups, 1 disabled (skipped 'rieklings': LineForm
'DLC2LCharRieklingMelee' did not resolve)`.

#### Roster composition

For N attackers: one leader if `LeaderForm` is set and `N >= 3`; roughly one ranged per three attackers if
`RangedForm` is set; the balance drawn from `LineForm` (round-robin when repeated). Deterministic given N, so
the mix is reproducible from the logs.

#### Shipped default content

Fifteen groups, each with `Enabled = true` set explicitly. Every FormID verified against the Spriggit
export at `C:\Projects\spriggit-output\`. The file ships heavily commented — with INI the documentation
lives next to the data.

**`bandits`** — the always-eligible fallback, no eligibility keys.

- Line `LCharBanditMeleeAny` · Ranged `LCharBanditMissile` · Leader `LCharBanditBoss`

**`forsworn`** — `RequireHold = ReachHoldLocation`

- Line `LCharForswornMelee1H` · Ranged `LCharForswornMissile` · Leader `LCharForswornBossMelee1H`

**`stormcloak_raiders`** — `RequireGlobal = CWPlayerAllegiance = 1` (player sided with the Legion)

- Line `LCharSoldierSons`

**`imperial_patrol`** — `RequireGlobal = CWPlayerAllegiance = 2` (player sided with the Stormcloaks)

- Line `LCharSoldierImperial`

**`thalmor`** — `RequireQuestComplete = MQ201`. Before Diplomatic Immunity the Thalmor have no particular
reason to know who the player is; after infiltrating the Embassy, they emphatically do.

- Line `LCharThalmorMelee1H` · Ranged `LCharThalmorMissile` · Leader `LCharThalmorMagicBoss`

**`necromancers`** — `RequireHold = RiftHoldLocation`

- Line `LCharWarlockNecromancer` · No ranged slot (they are all casters) · Leader `LCharWarlockBossNecro`

**`witches`** — `RequireHold = ReachHoldLocation` and `RequireHold = FalkreathHoldLocation` (the key is
repeatable, and repeated values OR together — this is the two-hold case the OR semantics exist for).

- Line `LCharWitchAny` · No ranged or leader slot. Hagravens exist only as `LCharHagravenCompanion`
  variants, so a coven has no hagraven matriarch unless someone adds one by hand.

**`alikr`** — `RequireHold = WhiterunHoldLocation` and `RequireQuestStage = MS08 = 200`. Stage 200 is the
ending where the player helps Saadia escape and kills Kematu; stage 201 is the one where they hand her over.
An equality test rather than `>=` is deliberate — `>=` would also match 201, which is the ending where the
Alik'r have no grudge at all. See the note below on why equality is safe here.

- Line `LCharAlikrMelee1H`, `LCharAlikrMelee2H` (repeated `LineForm`) · Ranged `LCharAlikrMissile` ·
  Leader `LCharAlikrMagic`

**`cultists`** — `RequireQuestStage = DLC2MQ01 >= 10` and `ForbidQuestComplete = DLC2MQ06`. Miraak's
followers start hunting the player once the Dragonborn questline opens and stop once Miraak is dead. Note
this group is Dragonborn-dependent but **not** Solstheim-scoped: cultists come to the mainland, which is the
whole point of them.

- Line `DLC2LCharCultist` · No ranged slot · Leader `DLC2LCharCultistSummoner`

**`vampires`** — `RequireHold = HjaalmarchHoldLocation` and `RequireHold = HaafingarHoldLocation`, plus
`RequireGameHour = 20-6` and `ForbidPlayerKeyword = Vampire`. The two holds with an established nest:
Movarth's coven outside Morthal, and Castle Volkihar off the Haafingar coast. They hunt at night, and they
do not ambush their own kind. Note the composition — the repeated `RequireHold` ORs the two holds together,
then ANDs against the hour window and the keyword test, so the group fires only in one of those two holds,
only after dark, and only for a non-vampire player.

- Line `LCharVampire` · No ranged slot · Leader `LCharVampireBoss`

**`penitus_oculatus`** — `RequireQuestComplete = DB02` and `ForbidQuestComplete = DBDestroy`. `DB02` is
"With Friends Like These…", the quest where the player is abducted by Astrid, kills one of the three
captives at her direction, and is inducted — so completing it satisfies both halves of "joined the
Brotherhood and committed at least one murder for them" in a single check. `DBDestroy` is "Destroy the Dark
Brotherhood!".

Deliberately looser than gating on `DB11` ("Hail Sithis!"). The Penitus Oculatus are the Empire's
counter-assassination arm, and hunting known Brotherhood members is their job description — they do not
need the player to have personally killed the Emperor before taking an interest.

The `ForbidQuestComplete` is belt-and-braces rather than load-bearing: in vanilla the two are mutually
exclusive, since `DBDestroy` is triggered by killing Astrid during `DB02` and fails it. Kept anyway, because
it costs one line, states the intent for a reader who doesn't know the questline's shape, and survives a mod
that reworks the branch.

- Line `LCharPenitusOculatus` · No ranged or leader slot

**`vigilants_vampire_hunt`** — `RequirePlayerKeyword = Vampire`

- Line `LCharVigilantOfStendarr` · No ranged or leader slot

**`vigilants_beast_hunt`** — `RequireGlobal = PlayerIsWerewolf = 1`

- Line `LCharVigilantOfStendarr` · No ranged or leader slot

**`solstheim_reavers`** — `RequireHold = DLC2SolstheimLocation`. The island's bandit analogue; the reaver
identity lives in the NPC records and outfits, not in the list naming, which is why these are `DLC2LCharBandit*`.

- Line `DLC2LCharBanditMelee1H` · Ranged `DLC2LCharBanditMissile` · Leader `DLC2LCharBanditBoss`

**`rieklings`** — `RequireHold = DLC2SolstheimLocation`. Small, numerous, and tribal; reads best at the upper
end of the attacker-count range.

- Line `DLC2LCharRieklingMelee` · Ranged `DLC2LCharRieklingMissile` · Leader `DLC2LCharMountedRiekling`
  (boar-mounted)

Corresponding FormIDs: `LCharBanditMeleeAny` `03DECD`, `LCharBanditMissile` `01E770`, `LCharBanditBoss`
`03DF16`, `LCharForswornMelee1H` `01E792`, `LCharForswornMissile` `01E794`, `LCharForswornBossMelee1H`
`0442F2`, `LCharSoldierSons` `01FC5C`, `LCharSoldierImperial` `01FC5B`, `ReachHoldLocation` `016769`,
`CWPlayerAllegiance` `023A43`, `LCharThalmorMelee1H` `02B129`, `LCharThalmorMissile` `02B12A`,
`LCharThalmorMagicBoss` `07DCA9`, `LCharWarlockNecromancer` `01E777`, `LCharWarlockBossNecro` `0E104F`,
`LCharVampire` `033973`, `LCharVampireBoss` `0339A9`, `LCharPenitusOculatus` `07D99F`,
`LCharVigilantOfStendarr` `0BFB53`, `MQ201` `035D5F`, `DB02` `01EA51`, `DBDestroy` `0934FB`,
`Vampire` (keyword) `0A82BB`,
`PlayerIsWerewolf` `0ED06C`, `LCharWitchAny` `074F9D`, `LCharAlikrMelee1H` `06766F`, `LCharAlikrMelee2H`
`067670`, `LCharAlikrMissile` `067671`, `LCharAlikrMagic` `06766E`, `MS08` `01CF25`, `WhiterunHoldLocation`
`016772`, `FalkreathHoldLocation` `01676F`, `RiftHoldLocation` `01676C`, `HjaalmarchHoldLocation` `01676E`,
`HaafingarHoldLocation` `016770`. Dragonborn:
`DLC2LCharCultist` `030CDC`, `DLC2LCharCultistSummoner` `03564D`, `DLC2MQ01` `017F8E`, `DLC2MQ06` `0179D7`,
`DLC2LCharBanditMelee1H` `01E8A9`,
`DLC2LCharBanditMissile` `01E8AA`, `DLC2LCharBanditBoss` `01E8B4`, `DLC2LCharRieklingMelee` `01B653`,
`DLC2LCharRieklingMissile` `01B654`, `DLC2LCharMountedRiekling` `038EB0`, `DLC2SolstheimLocation` `016E2A`.

**Exactly one group — `bandits` — carries no eligibility keys**, and that is now load-bearing rather than
incidental. It is the sole guarantee that the eligible set is non-empty on an otherwise-valid tick, so the
shipped file must never lose it: disabling `bandits` without adding another unconstrained group leaves the
beat unable to fire anywhere the player hasn't earned a gated group. The loader logs a warning at load time
when no enabled group is unconstrained, because that is a configuration mistake the user cannot otherwise
see. (`Enabled = false` on `bandits` is a legitimate thing to want — it just needs to be a deliberate choice
made with a replacement in hand.)

Everything else layers on top of that floor as the player's world state earns it, which is the point: a
level-3 character fresh out of Helgen sees bandits and not much else, while a vampire Dragonborn who has
infiltrated the Thalmor Embassy, murdered the Emperor, and crossed the Alik'r has a substantially more
interesting set of enemies — and one that shifts as they cross hold borders.

Several groups are mutually exclusive by construction rather than by convention. The two civil-war groups
can't both fire because one global can't hold two values. The two Vigilant groups can't both fire because
vanilla does not permit being simultaneously vampire and lycanthrope — they are split into two entries
because the eligibility keys AND together and there is no `AnyOf`, and splitting turns out to be the better
outcome anyway: "Vigilants hunting a vampire" and "Vigilants hunting a beast" want different `Flavor` text
for the Director to reason about, even though they share a roster.

**Why the Alik'r equality test is safe.** `RequireQuestStage` reads `GetCurrentStageID()`, which is the
*latest* stage, so an equality test only works if nothing advances the quest past it. `MS08` has two
`CompleteQuest` stages, 200 and 201, plus a stage 255 that no fragment sets and a stage 300 flagged
`FailQuest`. In normal play the quest terminates at 200 or 201 and stays there. Step 3's verification
confirms this rather than trusting it — if the stage does move, the fallback is to gate on the
`MS08AlikrFaction` relationship instead.

Everything past this point is a user edit, not a code change, which is the whole reason the table moved out
of C++.

**Solstheim qualifies as a hold.** `DLC2SolstheimLocation` carries exactly one keyword, `016771`, which is
`LocTypeHold` — so it passes the loader's own hold validation, and `HoldGrid` seeds from it the same way it
seeds from the nine mainland holds. That the DLC island is modelled as a tenth hold is what makes
region-locking these two groups possible at all.

**Three groups require `Dragonborn.esm`** — the two Solstheim ones plus `cultists`, which is
DLC-dependent without being Solstheim-scoped. No special handling is needed: on a load order without the
DLC, their roster EditorIDs don't resolve, the loader skips exactly those three with a logged reason, and the
other twelve load normally. A user without the DLC sees `loaded 12 of 15 groups` and a working beat. This
is the per-group failure isolation doing the job it was designed for, not a special case.

**Rieklings deliberately use the non-Thirsk lists.** `DLC2LCharRieklingThirskMelee` / `...Missile` exist
separately for the Thirsk tribe, which turns friendly if the player sides with it. The generic wild-riekling
lists stay hostile regardless, which is what an ambush needs.

**Considered and rejected**, so the reasoning isn't re-derived later:

- **Ash Spawn** (`DLC2LCharAshSpawnAll` `0322BD`, `DLC2LCharAshSpawnMagic` `0322C2`) — the forms are all
  present and the group would be region-locked to Solstheim like the other two, but the choreography is wrong
  for this beat. Ash Spawn are slow, ground-emergent constructs rather than travelers, so "materialize at
  distance, jog in, engage at close range" reads badly for them. Every other group in the table is something
  that could plausibly have been walking toward you already.
- **Falmer, Draugr** — overwhelmingly interior-dwelling, and this beat is exterior-only.
- **Hired thugs** — the best *thematic* fit for a Director-driven beat by some distance ("someone you wronged
  sent people"), but there is no leveled list for them. `WIThug`, `WIThugCommonerM`, and `WIThugNordM` are
  `NPC_` records used by the vanilla World Interactions system, with `WIThug` templating off the
  `LvlBanditBoss` `03DF17` NPC for level scaling. Spawning would work — `TESNPC` is a `TESBoundObject` like
  `TESLevCharacter` — but the loader's validation contract requires `TESLevCharacter`, and relaxing it to
  accept bare NPC records is a real decision (it also opens the door to users naming unique named NPCs) that
  this phase shouldn't make in passing. This is the one candidate that would justify revisiting that rule.
- **Silver Hand** — no leveled-character lists exist for them at all; they're hand-placed.
- **Conjurers** (`LCharWarlockConjurer` `06D269`, `LCharWarlockBossConjurer`) — mechanically distinct from
  necromancers, but as an *ambush* they read as the same beat: hostile mages jumped you. Folded out to keep
  the Director's menu from diluting. Deliberately left as a two-line user addition for anyone who wants it.
- **Werewolves** (`LCharWerewolf`, `LCharWerewolfBoss`) — closer to a creature pack than a faction; a
  coordinated werewolf ambush has no in-fiction organization behind it outside the Companions questline.
- **Creature packs** (wolves, trolls, ice wraiths) — buildable, but "a faction ambushes you" and "wildlife
  attacks you" are different narrative beats, and the second one vanilla already delivers constantly.

### Where ambushes may happen

Ambushes fire **outdoors, on the road or in open wilderness, and nowhere else.** Three hard exclusions:

- **Any interior cell.** The beat spawns a travelling approach from thousands of units away; interiors have
  neither the room nor the sightlines.
- **Locations marked Safe** (`LocationKeywords::IsSafe`) — towns, farms, inns, guild halls. Guards on patrol
  and NPCs with schedules already own those cells, and an ambush there reads as nonsense.
- **Locations marked Dangerous** (`LocationKeywords::IsDangerous`) — dungeons, bandit camps, lairs. Vanilla
  already populates them with combat; layering a Director-issued fight on top stacks into gauntlets.

Both keyword predicates walk the `BGSLocation::parentLoc` chain, so a child location inherits its parent's
classification the way vanilla quest conditions read it.

**Enforced twice, deliberately.** `AmbushLocationBlocker` is the single shared predicate behind both gates so
they cannot drift apart:

1. **Before the LLM request** — in `IsAvailable`, so `ambush` never enters the candidate list and the Director
   is never offered a beat it couldn't legally run.
2. **At compose time** — the first thing `TickCompose` does, before any group resolution or spawning. A
   beat-select round trip takes seconds, and the player can walk into a cave or through a town gate inside that
   window. Failure here is a clean COMPOSE failure with `failure_reason` of `interior`, `safe_location`, or
   `dangerous_location`, and no cooldown stamp.

Neither gate marshals to the main thread. Both read a stable singleton pointer, a `constexpr` member load
(`GetParentCell`), a flag, and a `parentLoc` walk over cached keyword pointers — the same shape `IsAvailable`
already performs off-main, sanctioned by `docs/MAIN_THREAD_STUTTER_AUDIT.md`.

The second gate is also what covers **force-dispatch**, which deliberately bypasses `IsAvailable` — a debug
dispatch from inside an inn now fails cleanly by name rather than spawning bandits into the common room.

### Spawn point selection

`AmbushSpawnPoints::Find(player, distanceUnits, count)` returns a ranked list of world positions, or empty.

1. **Sample.** 16 azimuths evenly spaced around the player at the current radius. Radii widen through
   ×1, ×1.25, ×1.5, ×2, ×2.5, each clamped into `[iAmbushMinSpawnDistanceUnits, iAmbushMaxSpawnDistanceUnits]`
   — a band of 2000–5000 units by default. Widening only: the minimum is a hard floor, so a narrowing step
   would just clamp back onto the first ring and re-sample it.
2. **Validate** each candidate, cheapest gate first: cell loaded; exterior; ground height resolvable; not
   underwater; on navmesh; not visible from the camera.
3. **Rank** survivors: **forward arc before rear** as a hard tier, then by a combined placement score
   within each tier — see **Placement scoring** below.
4. **Cluster.** Take the winner and jitter `count` positions around it within `kClusterRadiusUnits` so
   attackers don't stack on one point.

Three tiers, in preference order:

| Tier | Condition                                | Ranked by                                   |
| ---- | ---------------------------------------- | ------------------------------------------- |
| 1    | Covered, forward arc                     | Placement score                             |
| 2    | Covered, rear                            | Placement score                             |
| 3    | **Uncovered** — nothing covered anywhere | Nearest the rear azimuth, then **farthest** |

Tier 3 exists so an open plain can't make ambushes impossible. Failing the cover gate is not elimination: the
point has already cleared every other gate, so it is somewhere an attacker can legitimately stand, and it is
kept as a last resort. Only a point with no standable ground at all is discarded outright.

Note that tier 3 **inverts** the arc preference. With no cover to hide behind, the only thing left is the
player's own back, so the ranking becomes "closest to directly behind, then as far away as possible". Those two
keys compose naturally: every ring samples the same azimuths, so the rear-most azimuth ties across radii and
the distance key picks the outermost of them.

`Find` now returns empty only when there is nowhere standable at all — not merely nowhere hidden. The debug
line reports `arc=forward`, `arc=rear-fallback`, or `arc=uncovered-fallback` so which tier was used is
visible, and tier 3 additionally logs at `info` since it means the terrain gave us nothing to work with.

**The forward arc is the point of the beat.** An ambush should be walked *into* — somewhere ahead of the player
or square to either side. Anything in the rear hemisphere is a **fallback**, taken only when the forward arc has
nothing sufficiently hidden.

#### Placement scoring

Within a tier, candidates are ranked by a single cost, lower being better, with **angle and distance weighted
equally**:

```text
score = (1 - facingDot) / 2                                  // 0 dead ahead, 0.5 side, 1 behind
      + clamp((distance - minSpawn) / (maxSpawn - minSpawn))  // 0 near end of band, 1 far end
```

Equal weighting is the whole design: neither axis is a mere tiebreaker for the other. Worked examples against
the shipped 2000–5000 band:

| Candidate           | Angle cost | Distance cost | Score     |
| ------------------- | ---------- | ------------- | --------- |
| Dead ahead @ 2000   | 0.000      | 0.000         | **0.000** |
| Square right @ 2000 | 0.500      | 0.000         | **0.500** |
| Dead ahead @ 4000   | 0.000      | 0.667         | **0.667** |

So dead-ahead beats square-to-the-side at equal distance, but square-to-the-side up close beats dead-ahead two
thousand units further out. An earlier version treated the whole forward arc as equally good and used distance
only to break ties, which made a distant head-on spot outrank a near one off to the side.

That priority also drives when the search stops widening. Rear-hemisphere hits alone are *not* enough to stop
on — the search keeps widening while only fallbacks have been found, because a larger ring may still have cover
in front. Survivors accumulate across radii, so nothing found on the way is discarded; only a forward hit ends
the search early. The debug line reports `arc=forward` or `arc=rear-fallback` so which happened is visible.

The cover gate carries much more weight under this ordering than it did when the search preferred the rear:
positions in front of the player are usually *in view*, so "in front but hidden behind something" is the
genuinely scarce combination the search is hunting for.

#### The cover gate

`CameraVisibility::IsPositionBehindCover(pos, bodyHeight, coverRadius)` — a deliberately stricter and
differently-shaped question than `IsAnyPartVisibleFromCamera`, which judges an existing actor:

- **Silhouette, not a line.** Nine rays: three heights × three lateral offsets, offset perpendicular to the
  line of sight. A single vertical column can be blocked by a fencepost while the body is plainly visible
  either side of it.
- **Widened to the cluster.** The lateral offsets span `kClusterRadiusUnits`, so the test asks whether the
  whole group would be hidden. One attacker standing clear of the rock the others are behind is the same
  failure as no cover at all.
- **Every ray must be blocked.** One clear line means the player can see the spot.
- **Fails toward "not covered."** If the camera can't be resolved, or the position is inside the raycast
  minimum distance, the answer is no. This is the *opposite* fail direction from
  `IsAnyPartVisibleFromCamera`, and it has to be: there, a wrong answer costs a teleport the player might
  notice; here it lets them watch attackers materialize in front of them.

`filterInfo` on the pick is set to `COL_LAYER::kLOS`, the layer the engine uses for its own sight checks.
Leaving it at the default `0` (`kUnidentified`) casts against whatever rules layer 0 happens to carry — which
is not the question being asked, and is the likeliest explanation for the field report of attackers spawning in
plain view with only 4 of 32 candidates rejected as visible.

`CameraVisibility::IsAnyPartVisibleFromCamera` is the visibility gate — it exists precisely because
`Actor::HasLineOfSight` also gates on the player's FOV cone and returns false whenever the camera happens to
point away, which is the wrong question here.

**The navmesh gate is the highest-risk piece of this phase.** It's the difference between "attackers converge on
the player" and "attackers stand in a rock." It is prototyped in Step 4 and proven in Step 5 before any of the
real COMPOSE logic depends on it. If no usable navmesh query surfaces, the documented fallback is a
ground-height raycast plus a post-spawn settle check, with the position rejected if the actor doesn't settle.

### Cleanup ordering

**The quest teardown is deferred to the next dispatch.** The aliases are what anchor the spawned references as
persistent, so stopping the quest releases them and the engine reaps the corpses on the spot. An earlier version
of this design stopped and reset the quest at the end of the encounter and expected the corpses to survive it;
they did not, and the player couldn't loot anything they'd just killed.

So the encounter's own CLEANUP does not stop the quest at all:

1. Walk alias slots `0..7`, collect every filled reference.
2. For each **surviving** attacker: `Disable()` then `SetDelete(true)`.
3. Leave **corpses** in place, still held by their aliases. That is what keeps them lootable.
4. `SetStage(200)` (marked Complete Quest) — but **no** `Stop()` and **no** `Reset()`. The quest stays open,
   parked at its terminal stage, holding the bodies.
5. Stamp the per-beat cooldown — **only** when COMPOSE succeeded, so a failed compose doesn't burn 24 in-game
   hours.

The deferred half runs in COMPOSE's `StartingQuest` sub-phase, immediately before `EnsureQuestStarted`:
`RetireQuest` deletes whatever is still in the aliases, then `Stop()` and `Reset()`. By then the player has had
the full cooldown — 24 in-game hours by default — to loot and move on.

`RetireQuest` reads the **aliases**, not the in-memory created-reference list, precisely because it may be
running after a save/load: the aliases persist with the quest, the in-memory list does not. It is idempotent, so
the first ambush of a save calls it against a never-started quest harmlessly.

Two consequences worth stating:

- **`IsAvailable` must not treat a non-zero stage as "busy."** The quest sits at stage 200 between encounters,
  so only the genuinely in-flight stages (10 and 20) block. Reading "any non-zero stage" as busy would let the
  beat fire exactly once per save.
- **The quest stays running between ambushes.** It carries no `Name` and its stage log entries have no text, so
  it never surfaces in the player's journal. The cost is up to eight persistent references held until the next
  ambush, which is bounded and deliberate.

`Abort()` is the exception: it runs CLEANUP *and* `RetireQuest` immediately, corpses included. Its contract is
that the world-side effects are gone when it returns, and an aborted encounter isn't one the player earned loot
from.

---

## Open engine questions

Each is resolved by an early step, with its fallback documented so no step can dead-end.

1. **Does `CreateReferenceAtLocation` resolve a `TESLevCharacter` base into a concrete NPC the way `PlaceAtMe`
   does?** Resolved in Step 5. Fallback: resolve the leveled list ourselves via `TESLeveledList` and pass the
   resulting `TESNPC*` as the base.
2. **Does a force-filled alias retain its reference after the actor dies, or is `AllowDead` required?** Resolved
   in Step 5. Fallback: set `AllowDead` on all eight attacker aliases in the CK and re-verify. (Step 1 sets it
   pre-emptively, since it costs nothing if unnecessary.)
3. **What does Mutagen emit for an `Optional` alias with no fill rule, and does the engine treat it as empty
   rather than fill-failed?** Resolved in Step 1. Fallback: author with `Optional` checked in the CK, inspect
   the serialized YAML, and hand-adjust the record if the CK insists on writing a fill rule.
4. **Is there a usable navmesh-reachability query in CommonLibSSE-NG?** Resolved in Step 4. Fallback:
   ground-height raycast plus a post-spawn settle check, rejecting positions where the actor doesn't settle.

**Closed before implementation:** *"Is the player actually added to `CWImperialFaction` / `CWSonsFaction` on
joining a side?"* — yes, traced through `CWScript::SetPlayerAllegiance` and `getFaction()` in the vanilla
Papyrus sources. See **How the civil-war groups detect allegiance** above; the shipped groups key off the
`CWPlayerAllegiance` global anyway, because faction membership has a temporary Battle-for-Whiterun window in
which it lies.

---

## ESP content

All new records are owned by `NarrativeEngine.esp` and must fall inside the ESL range `[0x000800..0x000FFF]`
that `check-esl-formids.ps1` enforces. Highest currently allocated is `000830` (`_ne_MCMQuest`).

| Record               | Type | FormID   | Notes                                          |
| -------------------- | ---- | -------- | ---------------------------------------------- |
| `_ne_AmbushQuest`    | QUST | `000831` | Nine aliases, three stages, one script         |
| `_ne_AmbushApproach` | PACK | `000832` | Travel-to-`PlayerRef`, run speed, weapon drawn |

### Quest stages

| Stage | Flags          | Meaning                                                       |
| ----- | -------------- | ------------------------------------------------------------- |
| 0     | Startup Stage  | Quest started. Fragment: `SetStage(10)`.                      |
| 10    | —              | Spawning. C++ creates references and force-fills the aliases. |
| 20    | —              | Engaged. Fill verified; attackers armed and approaching.      |
| 200   | Complete Quest | Terminal. Set by C++ during CLEANUP.                          |

### Aliases

| Alias                     | ID  | Fill                     | Flags      | Packages             |
| ------------------------- | --- | ------------------------ | ---------- | -------------------- |
| `PlayerRef`               | 0   | Forced → `000014:Skyrim` | —          | —                    |
| `Attacker01`…`Attacker08` | 1–8 | none                     | `Optional` | `_ne_AmbushApproach` |

Eight slots is the hard ceiling on attacker count; `iAmbushMaxAttackerCount` is clamped to it at settings-read
time so a hand-edited INI can't request slot 9. Adding slots later is a copy-paste in the YAML plus a
`NextAliasID` bump.

### Papyrus

`esp/Source/Scripts/_ne_AmbushQuest.psc` — two functions, no state, no polling. Both exist only because the
underlying engine call has no CommonLibSSE-NG binding.

---

## Settings

New `[Beats]` keys. The old `iAmbush*BanditCount` names are **not** reused — the roster is no longer
bandit-specific, and the old keys were deleted with the old beat, so there is no back-compat obligation.

| Key                                | Default | Meaning                                                      |
| ---------------------------------- | ------- | ------------------------------------------------------------ |
| `bEnableAmbush`                    | `true`  | Registry enable gate.                                        |
| `iAmbushDefaultAttackerCount`      | `3`     | Used when the Director omits or out-ranges `attacker_count`. |
| `iAmbushMinAttackerCount`          | `2`     | Lower clamp.                                                 |
| `iAmbushMaxAttackerCount`          | `6`     | Upper clamp, itself clamped to the eight authored slots.     |
| `iAmbushDefaultSpawnDistanceUnits` | `2000`  | Starting radius. Not Director-selectable — see below.        |
| `iAmbushMinSpawnDistanceUnits`     | `2000`  | Hard floor; the search never goes below it.                  |
| `iAmbushMaxSpawnDistanceUnits`     | `5000`  | Ceiling the search widens toward.                            |
| `iAmbushEngageDistanceUnits`       | `1500`  | Approach → hostile handoff range.                            |
| `iAmbushAbandonDistanceUnits`      | `8000`  | All survivors beyond this → abandoned.                       |
| `iAmbushMaxDurationSeconds`        | `600`   | Unpaused real-time cap on RUNNING.                           |
| `iAmbushPerBeatCooldownGameHours`  | `24`    | In-game-hour cooldown after a completed or abandoned ambush. |

---

## Persistence

Cosave record `'NAMB'`, version 1:

```text
double  lastCompletionGameHours   // per-beat cooldown stamp
u32     groupIdLen
char*   groupId                   // chosen group, for post-reload logging / narration
```

Deliberately thin. The quest stage is the phase, and the aliases are the roster — neither needs mirroring into
the cosave. `OnLoad` on an unknown version clears to defaults, matching every other beat's record.

---

## Prompt integration

`BuildBeatSelectPromptContext` (`src/BeatSystem.cpp:708`) gains an `ambush_attacker_candidates` array on the
ambush candidate's JSON entry, populated from `AmbushAttackerGroups::EligibleGroups()`. Each entry:
`{id, display_name, flavor}`. `narrative_engine_action_select.prompt` gains a matching
`{%- if length(cand.ambush_attacker_candidates) > 0 %}` block, following the two blocks already there.

Parameters the Director returns for this beat:

- `attacker_group` — REQUIRED, string, must match an `id` from `ambush_attacker_candidates`.
- `attacker_count` — REQUIRED, integer, clamped to the configured range.
- `narration_prose` — REQUIRED, string. One to three sentences of in-world prose naming WHO is attacking and
  WHAT their grievance is. Surfaced as silent narration once the fight starts — see **Encounter narration**.

**Spawn distance is deliberately not a parameter.** It was one originally, and the Director was uniformly bad
at it: across three test encounters it asked for 256, 128, and 256 units — two to four metres. Those clamped up
to `iAmbushMinSpawnDistanceUnits`, so the practical effect of offering the knob was that every ambush spawned
at the *minimum* distance rather than the configured default. Where attackers appear is staging, not narrative
judgement, and the model has no useful view of the terrain it would be reasoning about. It reads straight from
`iAmbushDefaultSpawnDistanceUnits` now, and `Description()` tells the Director not to send anything else.

`attacker_group` and `narration_prose` are both free-form LLM-returned strings and route through
`LLMTextSanitizer::Sanitize` at the extraction site per `docs/LLM_RESPONSE_HANDLING.md`. `attacker_group` is
additionally re-validated against the eligible set as it stood at `OnStart` — not at prompt-build time.

These are declared in `Description()` rather than in the prompt template, because the template's `parameters`
field is explicitly free-form and states that "the chosen action validates its own parameter shape". The
per-beat description is the only place a beat's parameter schema is expressed.

Note this is a **separate** field from the response's global `parameter_justification`, which stays as it is
and continues to feed the letter and visit composers. That one is Director-frame reasoning about the choice;
`narration_prose` is in-world prose about the event, and the two want different voices.

### Encounter narration

The Director is told to name the player rather than reach for "the player" or an epithet like "the intruder",
and `player_context.player_name` was added to the beat-select prompt so it has the name to hand instead of
having to mine it out of the recent-events text. `Description()` points at that field explicitly.

`narration_prose` is held from `OnStart` and submitted exactly once, as a silent scene event, **when the player
themselves enters combat**. Not at spawn, and not when the attackers engage: the prose says who is jumping the
player and why, so it should land when the fight is real to them rather than while a group is still jogging
over a hill.

The check runs on every RUNNING tick rather than inside the 5-second poll gate — it costs one
`EngineUtils::IsPlayerInCombat()` bool read, and the log entry should be roughly contemporaneous with the fight
starting. A latch makes it fire once per encounter however combat starts and stops, and an empty prose value
latches without dispatching.

Delivery is `_ne_AmbushQuest.RunAmbushNarration`, mirroring `_ne_VisitQuest`'s Valediction silent scene event:
`SkyrimNetApi.RegisterPersistentEvent(content, originator, player)` with the lead attacker as originator, so the
memory system associates the event with whoever actually jumped the player. It goes through Papyrus because
`RegisterPersistentEvent` has no native binding — the native `SkyrimNetAPI` surface has no equivalent.

`Description()` is rewritten. The old text promised "leveled bandits (up to six)" and would be lying about both
halves.

---

## File map

```text
NEW  include/AmbushBeat.h                 IBeat implementation + persistence namespace
NEW  src/AmbushBeat.cpp
NEW  include/AmbushAttackerGroups.h       group-file parse, validation, runtime lookup
NEW  src/AmbushAttackerGroups.cpp
NEW  statics/SKSE/Plugins/NarrativeEngine/AttackerGroups.ini   the group table itself (user-editable)
NEW  include/AmbushSpawnPoints.h          geometric spawn-point search
NEW  src/AmbushSpawnPoints.cpp
NEW  esp/Source/Scripts/_ne_AmbushQuest.psc
NEW  esp/plugin/Quests/_ne_AmbushQuest - 000831_NarrativeEngine.esp.yaml
NEW  esp/plugin/Packages/_ne_AmbushApproach - 000832_NarrativeEngine.esp.yaml
EDIT src/Plugin.cpp                       register beat; cosave save/load/revert wiring
EDIT src/BeatRegistry.cpp                 bEnableAmbush gate
EDIT include/Settings.h, src/Settings.cpp new [Beats] keys
EDIT statics/SKSE/Plugins/NarrativeEngine.ini
EDIT src/BeatSystem.cpp                   ambush_attacker_candidates injection
EDIT statics/.../narrative_engine_action_select.prompt
EDIT README.md                            restore the Ambush bullet
```

---

## Implementation plan

Sequential. **Step 1 is all of the Creation Kit work in one place** — every manual CK operation this phase needs
is collected there so the user does one CK session and then hands off. Every step after it is **entirely
Claude's work** or **entirely the user's work**; none is mixed. Verification is attributed separately, since a
step Claude implements may still need a running game to confirm.

---

### Step 1 — Creation Kit content: quest, aliases, package, script

- [X] Complete

**[USER]**

**Goal:** Author every ESP record and the Papyrus script the rest of the phase depends on. This is the only CK
session in the phase — nothing after this step opens the Creation Kit.

**Sub-tasks:**

1. Create `esp/Source/Scripts/_ne_AmbushQuest.psc` with exactly this content:

   ```papyrus
   Scriptname _ne_AmbushQuest extends Quest

   ; Attacker slots, wired to the quest's Attacker01..Attacker08 aliases by name.
   ; Filled from C++ after the quest is running -- see PHASE_11 for why forced
   ; fill on a *running* quest is safe where Phase 04's stopped-quest fill was not.
   ReferenceAlias Property Attacker01 Auto
   ReferenceAlias Property Attacker02 Auto
   ReferenceAlias Property Attacker03 Auto
   ReferenceAlias Property Attacker04 Auto
   ReferenceAlias Property Attacker05 Auto
   ReferenceAlias Property Attacker06 Auto
   ReferenceAlias Property Attacker07 Auto
   ReferenceAlias Property Attacker08 Auto

   ReferenceAlias Property PlayerRef Auto

   ; Both functions below exist only because the underlying engine call has no
   ; CommonLibSSE-NG binding. Everything else this beat needs is native C++.

   ; ReferenceAlias.ForceRefTo has no native binding.
   Function FillAttackerSlot(int aiIndex, ObjectReference akRef)
       ReferenceAlias slot = GetAttackerSlot(aiIndex)
       if slot == None || akRef == None
           Debug.Trace("[_ne_AmbushQuest] FillAttackerSlot: bad index " + aiIndex)
           return
       endif
       slot.ForceRefTo(akRef)
   EndFunction

   ; Actor.StartCombat has no native binding.
   Function EngageAttacker(int aiIndex)
       ReferenceAlias slot = GetAttackerSlot(aiIndex)
       if slot == None
           return
       endif
       Actor attacker = slot.GetActorReference()
       Actor player = PlayerRef.GetActorReference()
       if attacker == None || player == None
           return
       endif
       attacker.StartCombat(player)
   EndFunction

   ReferenceAlias Function GetAttackerSlot(int aiIndex)
       if aiIndex == 0
           return Attacker01
       elseif aiIndex == 1
           return Attacker02
       elseif aiIndex == 2
           return Attacker03
       elseif aiIndex == 3
           return Attacker04
       elseif aiIndex == 4
           return Attacker05
       elseif aiIndex == 5
           return Attacker06
       elseif aiIndex == 6
           return Attacker07
       elseif aiIndex == 7
           return Attacker08
       endif
       return None
   EndFunction
   ```

2. Open the Creation Kit through MO2; load `NarrativeEngine.esp` as the active file. Compile
   `_ne_AmbushQuest.psc` in the script editor.
3. Create the quest `_ne_AmbushQuest`:
   - EditorID: `_ne_AmbushQuest`.
   - Quest Data: **Start Game Enabled** OFF, **Run Once** OFF, **Allow Repeated Stages** ON. Event = None.
     Priority `90`.
   - **Scripts tab**: attach the `_ne_AmbushQuest` script.
4. On the Aliases tab, add the `PlayerRef` alias first (it must be alias ID 0, and the package targets it):
   - Fill Type: **Forced Reference** → `PlayerRef` (`000014:Skyrim.esm`).
   - Flags: `Optional` **OFF**.
5. Create the AI package `_ne_AmbushApproach`:
   - Package type: **Travel**.
   - Destination: `Alias:PlayerRef`, radius `200`.
   - Preferred Speed: **Run**. Flags: `Weapon Drawn` ON, `Allow Swimming` ON.
   - Flags: `Ignore Combat` **OFF**, `Interrupt Override` **None** — per
     `docs/engine-findings/ai-package-flags-for-script-driven-combat-handoff.md`, an approach package that
     ignores combat prevents combat from ever preempting it.
   - Package Condition: `GetDistance` on `Alias:PlayerRef`, Comparison `>`, Value `600`. Run On: Subject.
6. Back on `_ne_AmbushQuest`'s Aliases tab, add eight reference aliases `Attacker01` … `Attacker08`. For **each**:
   - Fill Type: leave **unset** — no Find Matching Reference, no Create Reference to Object, no Forced
     Reference, no conditions. These are filled from C++ at runtime.
   - Flags: `Optional` **ON**. `Allow Dead` **ON** (so the alias keeps its reference after the attacker dies —
     both death detection and cleanup read through the alias).
   - Packages tab (on the alias): add `_ne_AmbushApproach`.
7. On the Stages tab, add stages 0, 10, 20, 200:
   - Stage 0: Flags = **Startup Stage** ON. Fragment: `SetStage(10)`.
   - Stage 10: no flags, empty fragment.
   - Stage 20: no flags, empty fragment.
   - Stage 200: Flags = **Complete Quest** ON. Empty fragment.
8. Save the ESP. Run `pwsh -File build.ps1 build` so the ESP is serialized back into `esp/plugin/`.

**Specifics:**

- `PlayerRef` must be created before the package, because the package's destination targets it. It must also be
  the first alias so it lands at alias ID 0.
- Do not attach any script to the attacker aliases. All per-attacker behavior lives in C++ this time; the old
  implementation's `_ne_BanditAmbushQuest_SpawnedBandit.psc` polling loop is not being reintroduced.
- If the CK refuses to save an alias with no fill type at all, pick `Forced Reference` and leave the reference
  slot empty, then check the serialized YAML in the next step — what matters is that the alias record carries
  no fill data and the `Optional` flag is set.

**Verify [USER]:**

- The CK compiled `_ne_AmbushQuest.psc` with no errors.
- Open xEdit on `NarrativeEngine.esp`. Confirm one QUST `_ne_AmbushQuest` with nine aliases (PlayerRef plus
  Attacker01–08), stages 0/10/20/200 with the Complete Quest flag on 200, and the `_ne_AmbushQuest` script
  attached; and one PACK `_ne_AmbushApproach` with the travel destination, run speed, and the `>600` distance
  condition.
- Boot Skyrim. The SKSE log shows no ESP-load errors and no script-binding warnings on `_ne_AmbushQuest`.

**Verify [CLAUDE]:**

- `pwsh -File check-esl-formids.ps1` passes and reports two new records.
- Read the serialized YAML for the quest. Confirm each `Attacker0N` alias carries the `Optional` and
  `AllowDead` flags, has `_ne_AmbushApproach` in its `PackageData`, and has **no** `CreateReferenceToObject`,
  no `ForcedReference`, and no `Conditions` block. This is the resolution of the "what does Mutagen emit for a
  fill-rule-less alias" open question — record the answer in this doc's Post-implementation section.
- `pwsh -File format.ps1` passes.

---

### Step 2 — Settings + INI surface

- [X] Complete

**[CLAUDE]**

**Goal:** Land the full `[Beats]` configuration surface for the beat before any code reads it, so later steps
never have to touch `Settings` again.

**Files:** `include/Settings.h`, `src/Settings.cpp`, `statics/SKSE/Plugins/NarrativeEngine.ini`.

**Sub-tasks:**

1. Add all eleven fields from the **Settings** section above, with the documented defaults.
2. Add the matching `ini.GetBoolValue` / `ini.GetLongValue` reads in `ReadIniInto`, in the same block-per-beat
   layout the file already uses.
3. Clamp `ambushMaxAttackerCount` to the eight authored alias slots on the read path — define the slot count as
   a named constant so Step 6 can reference the same value rather than duplicating the literal.
4. Mirror every key into `NarrativeEngine.ini` with a comment block explaining the count/distance clamps and
   the two abandon gates.
5. Run `pwsh -File format.ps1`.

**Specifics:**

- Clamp on the read path, not at use sites — that's the pattern `Settings::ReadIniInto` already follows for the
  recent-events tail floor.
- `statics/` edits only reach the runtime mod folder via `build.ps1` (`feedback_statics_require_build`).

**Verify [CLAUDE]:**

- `pwsh -File build.ps1 build` succeeds.
- Set `iAmbushMaxAttackerCount=99` in the deployed INI, boot, and confirm the SKSE log reports the clamped
  value of 8 rather than 99.

---

### Step 3 — `AmbushAttackerGroups`: group-file loader, validation, and eligibility

- [X] Complete (Claude's half; user in-game verification outstanding)

**[CLAUDE]**

**Goal:** Parse and validate `AttackerGroups.ini`, expose the eligibility filter, and ship the default file.
Standalone and independently observable before anything consumes it. The load-bearing property this step must
deliver is **per-group failure isolation** — one malformed group never costs the user the others.

**Files:** `include/AmbushAttackerGroups.h`, `src/AmbushAttackerGroups.cpp`,
`statics/SKSE/Plugins/NarrativeEngine/AttackerGroups.ini`, `src/Plugin.cpp` (load at `kDataLoaded`).

**Sub-tasks:**

1. Define the runtime types: `Roster` (resolved `TESLevCharacter*` line list plus optional ranged and leader),
   `Eligibility` (resolved hold FormIDs, faction FormIDs, level bounds), `Group` (id, display name, flavor,
   roster, eligibility), and `EligibilityContext` (player pointer plus the `Region::Resolution`, captured once
   by the caller rather than re-resolved per group).
2. Implement `Load()`: read the file with `CSimpleIniA` with `SetMultiKey(true)` so repeated keys work, walk
   every `[Group:<id>]` section, and build one `Group` per valid section.
3. Implement the validation contract from the design overview, **per group**, each failure logging a reason
   naming the section and skipping only that group. Include the `LocTypeHold` keyword check on hold values —
   that is what catches `WhiterunLocation` typed where `WhiterunHoldLocation` was meant, which would otherwise
   fail silently forever.
4. Log unknown keys at `warn` and ignore them, so a group file authored against a later build never hard-fails
   this one. Close with the summary line from the design overview, reporting loaded, skipped, and disabled
   counts separately — "disabled" is a deliberate user choice and must not read as a failure.
   Additionally, warn when no *enabled* group is unconstrained: that state leaves the beat unable to fire in
   large parts of the world and is invisible from inside the game.
5. Implement `EligibleGroups(const EligibilityContext&)` returning `std::vector<const Group*>`, and
   `Find(std::string_view id)`. Groups with `Enabled = false` are validated at load like any other but are
   never returned by either — a disabled group must be invisible to the Director, not merely deprioritised.
   Hold matching compares against `Region::ForPlayer().holdFormID` — the composed grid-then-parent-walk
   answer. **Do not call `HoldGrid` directly.** Global matching reads
   `RE::TESGlobal::value` and applies the parsed operator; parse the operator once at load, not per evaluation.
6. Implement `ComposeRoster(const Group&, int count)` per the composition rule — leader if set and
   `count >= 3`, one ranged per three if set, balance round-robin across the `LineForm` list.
7. Author the shipped default `AttackerGroups.ini` with the fifteen groups from the design overview, heavily
   commented: what each key does, that unknown keys are ignored, and that a bad group is skipped rather than
   fatal.
8. Call `Load()` from `Plugin.cpp` at `kDataLoaded`, **after** `HoldGrid::Initialize()` — the hold-keyword
   validation needs form lookups available.
9. Add a debug-mode log line listing eligible group ids each time `EligibleGroups` is called, gated on
   `Settings::Get().debugMode`.
10. Run `pwsh -File format.ps1`.

**Specifics:**

- Missing file is not an error condition worth failing the plugin over, but it *is* worth shouting about: log
  at `error`, load zero groups, and let `IsAvailable` gate the beat off entirely. A silently disabled beat is
  worse than a loud one.
- Resolve every EditorID once at load and store resolved pointers / FormIDs. Do not re-resolve per tick;
  `LookupByEditorID` is not free and eligibility runs on the Director's cadence.
- `Region::ForPlayer()` is main-thread and uses `LookupByEditorID`. Eligibility therefore runs main-thread; the
  caller in Step 6 marshals accordingly.
- Do not invent EditorIDs or FormIDs for any vanilla record. Every value in the shipped file is verified
  against `C:\Projects\spriggit-output\`; anything added later goes through the same lookup per
  `docs/VANILLA_RECORD_REFERENCE.md`.
- `statics/` edits only reach the runtime mod folder via `build.ps1` (`feedback_statics_require_build`).

**Verify [CLAUDE]:**

- `pwsh -File build.ps1 build` succeeds and the file lands at
  `<mod folder>/SKSE/Plugins/NarrativeEngine/AttackerGroups.ini`.

**Verify [USER]:**

With `bDebugMode=true` and a save handy:

1. Boot. The log's summary line reports `loaded 15 of 15 groups, 0 disabled` with Dragonborn installed, or
   `loaded 12 of 15` without it — in which case confirm the three skipped groups are named and the beat still
   works.
2. On an early-game save, stand in Whiterun Hold wilderness at midday. The debug log lists exactly `bandits`
   and nothing else.
3. Fast-travel to Markarth or anywhere in The Reach. The log now lists `bandits`, `forsworn`, and `witches`.
   Then walk out into open Reach wilderness away from any settlement and confirm both are **still** listed —
   this is the `HoldGrid` coverage check, and the case the parent-walk alone would miss. Repeat in Falkreath
   Hold and confirm `witches` appears there too but `forsworn` does not: that is the repeated-key OR working.
4. Travel to The Rift and confirm `necromancers` appears; confirm it is absent everywhere else.
5. **Hour-window check.** Stand in Hjaalmarch or Haafingar — `vampires` is hold-scoped, so testing the hour
   window anywhere else will show nothing and read as a false failure. Wait until after 20:00 with `t`,
   confirm `vampires` appears, and confirm it disappears again after 06:00. This is the midnight-wrap case:
   if `vampires` is instead eligible from 06:00 to 20:00, the window logic is inverted. While you're there,
   confirm it is absent in a neighbouring hold at the same hour — that is the AND across the hold and hour
   keys.
6. **Keyword check.** On a save where the player is a vampire, standing in Hjaalmarch or Haafingar after
   dark, confirm `vampires` is *absent* and `vigilants_vampire_hunt` is present. On a werewolf save, confirm
   `vigilants_beast_hunt` is present and `vigilants_vampire_hunt` is not; cross-check
   `getglobalvalue PlayerIsWerewolf` reads `1`.
7. **Quest-completion check.** On a save that has finished Diplomatic Immunity, confirm `thalmor` appears;
   on one that hasn't, confirm it doesn't. For `penitus_oculatus`, confirm it appears on a save that has
   completed "With Friends Like These…" (`DB02`) and *not* on a pre-Brotherhood save; if you have a save that
   destroyed the Brotherhood instead, confirm it stays absent there too. For `cultists`, check against
   `DLC2MQ01` reaching stage 10 — if the cultist gate opens at the wrong narrative moment, adjust the stage
   number in the shipped file and note the corrected value.
8. **Alik'r branch check.** On a save that finished In My Time of Need by killing Kematu, confirm `alikr`
   appears in Whiterun Hold and nowhere else. On a save that handed Saadia over, confirm it never appears.
   Then run `getstage MS08` on the first save and confirm it still reads `200` — if the quest has advanced
   past it, the equality test is unsafe and the gate needs rethinking.
9. On a save that has joined the Imperial Legion, confirm `stormcloak_raiders` appears and `imperial_patrol`
   does not. On a Stormcloak save, confirm the reverse. On a save that has not picked a side, confirm neither
   appears. Cross-check with `getglobalvalue CWPlayerAllegiance` in the console — it should read `1` for
   Imperial, `2` for Stormcloak, `0` for undecided, and the eligible set should agree with it.
10. **Solstheim coverage check.** Travel to Solstheim and stand in open ashland away from Raven Rock. Confirm
   the log lists `solstheim_reavers` and `rieklings` alongside whichever unconstrained groups apply, and that
   `forsworn` and the civil-war groups are absent. This is the one place `HoldGrid` has to seed
   from a DLC worldspace — if the two Solstheim groups never appear despite the file loading them, the grid
   isn't covering `DLC2SolstheimWorld` and that's a `HoldGrid` finding, not a group-file one.
11. **Failure-isolation test — this is the step's most important check.** Edit the deployed
   `AttackerGroups.ini` and break the `forsworn` group in three separate runs: (a) point `LineForm` at a
   nonsense EditorID, (b) point `RequireHold` at `WhiterunLocation` (a real location that is not a hold), and
   (c) add a junk key like `Foo = Bar`. Each run, confirm the log names the offending section and the specific
   reason, that (a) and (b) skip only `forsworn` while every other group still loads, and that (c) logs a
   warning but loads `forsworn` normally.
12. **`Enabled` check.** Set `Enabled = false` on `forsworn`, boot, and confirm the summary line reports it
    as *disabled* rather than skipped or failed, and that it never appears in the eligible list even in the
    Reach. Set it back to `true` and confirm it returns. Then set `Enabled = false` on `bandits` and confirm
    the loader warns that no unconstrained group remains.
13. Delete `AttackerGroups.ini` entirely, boot, and confirm the log shows a loud error, zero groups loaded,
    and the Dispatch tab shows `ambush` as unavailable rather than the game misbehaving.

---

### Step 4 — `AmbushSpawnPoints` module

- [X] Complete

**[CLAUDE]**

**Goal:** The geometric spawn-point search, standalone and observable, before the beat depends on it. This is
the step that resolves the navmesh open question.

**Files:** `include/AmbushSpawnPoints.h`, `src/AmbushSpawnPoints.cpp`.

**Sub-tasks:**

1. Implement `Find(RE::Actor* player, int distanceUnits, int count)` returning
   `std::vector<RE::NiPoint3>` (empty on failure) per the four-stage design: sample, validate, rank, cluster.
2. Investigate the navmesh-reachability query surface in CommonLibSSE-NG before writing the gate. If a usable
   query exists, use it. If not, implement the documented fallback (ground-height raycast, with the post-spawn
   settle check deferred to Step 5 where an actor actually exists) and record the finding in
   `docs/engine-findings/`.
3. Use `CameraVisibility::IsAnyPartVisibleFromCamera` for the visibility gate.
4. Emit a debug-mode log line per search: requested radius, radii actually tried, candidates sampled, how many
   survived each gate, and the winning position. A search that fails must say which gate killed it — this is
   the single most useful diagnostic in the phase.
5. Run `pwsh -File format.ps1`.

**Specifics:**

- Main-thread only. Every call reads engine state.
- Widen the radius before giving up, never below `iAmbushMinSpawnDistanceUnits` — spawning closer than
  the configured floor is worse than not spawning.
- Failure returns empty and is not an error; the caller turns it into a clean COMPOSE failure.

**Verify [CLAUDE]:**

- `pwsh -File build.ps1 build` succeeds.
- The navmesh finding — query used, or fallback taken and why — is written up in `docs/engine-findings/` and
  linked from this doc's Post-implementation section.

---

### Step 5 — `AmbushBeat` skeleton, registration, persistence, and single-attacker spike

- [X] Complete (Claude's half; user in-game verification outstanding)

**[CLAUDE]**

**Goal:** A registered, force-dispatchable beat whose COMPOSE spawns exactly **one** attacker into slot 0 and
stops there. This is a deliberate spike: it exercises `CreateReferenceAtLocation`, the VM-dispatched
`FillAttackerSlot`, the alias readback, and the package bind end-to-end at the smallest possible size, and it
resolves two open engine questions before Step 6 builds the real flow on top.

**Files:** `include/AmbushBeat.h`, `src/AmbushBeat.cpp`, `src/Plugin.cpp`, `src/BeatRegistry.cpp`.

**Sub-tasks:**

1. `AmbushBeat` implementing `IBeat`: `Name()` = `"ambush"`, `Polarity()` = `Raise`, `IsAvailable` gating on
   the location rules (see **Where ambushes may happen**), quest-not-in-flight, per-beat cooldown, and a
   non-empty eligible group set. `Description()` stays a short placeholder until Step 8.
2. `'NAMB'` cosave record per the **Persistence** section, with `OnSave` / `OnLoad` / `OnRevert`. Wire all
   three into `src/Plugin.cpp` alongside the other beats' records. **Do not reuse `'NBAM'`.**
3. Register the beat in `Plugin.cpp` and add the `bEnableAmbush` gate to `BeatRegistry.cpp`.
4. Spike COMPOSE, as a sub-state machine that Step 6 will extend rather than replace:
   `AmbushSpawnPoints::Find(player, default, 1)` → `EnsureQuestStarted` → `CreateReferenceAtLocation` with
   `LCharBanditMeleeAny` and `a_forcePersist=true` → VM-dispatch `FillAttackerSlot(0, ref)` → next tick, read
   `BGSRefAlias::GetReference()` on slot 0 → `SetActorValue(kAggression, 0)` + `EvaluatePackage()` →
   `SetStage(20)` → RUNNING.
5. Spike RUNNING: poll slot 0 every 5 s; log the attacker's distance to the player, whether it's dead, and its
   current package. On death or on `iAmbushMaxDurationSeconds`, go to CLEANUP.
6. Full CLEANUP per the design's ordering, including `Abort()`.
7. Every engine touch marshals through `MainThread::Run` from `Tick`, per `docs/THREADING_MODEL.md`.
8. Run `pwsh -File format.ps1`.

**Specifics:**

- `TESLevCharacter` derives from `TESBoundObject`, so it type-checks as `CreateReferenceAtLocation`'s
  `a_base`. Whether the engine *resolves* the leveled list at creation is exactly what this spike proves. If
  the spawned reference turns out to be an unresolved placeholder, switch to resolving the list via
  `TESLeveledList` and passing the resulting `TESNPC*`.
- Read the alias back on a **later tick**, never in the same tick as the dispatch — `VMDispatchOnQuest` only
  reports that the call was queued.
- Log the resolved actor's base FormID and display name after fill. That single log line answers the
  leveled-resolution question.

**Verify [CLAUDE]:**

- `pwsh -File build.ps1 build` succeeds.
- The dashboard's Dispatch tab lists `ambush` with its enable toggle on and cooldown populated.

**Verify [USER]:**

With `bDebugMode=true`, in exterior wilderness:

1. Force-dispatch `ambush` from the dashboard's Dispatch tab.
2. **Leveled resolution:** the log's post-fill line names a concrete bandit NPC (e.g. a specific
   `EncBandit…` record), not a leveled-list placeholder. Record which in this doc.
3. One attacker appears out of view at roughly the configured distance, on walkable ground — not embedded in
   terrain, not floating, not underwater.
4. It jogs toward you with its weapon drawn rather than standing still. (Standing still means the package
   didn't bind.)
5. **`AllowDead` behavior:** kill the attacker. Confirm the log's next RUNNING poll still reads a reference out
   of slot 0 and reports it as dead — rather than reading an empty alias. Record the answer.
6. CLEANUP runs: the corpse remains lootable, the quest reaches stage 200 and stops, and the cooldown appears
   on the Dispatch tab.
7. Save mid-encounter, quit to main menu, reload. The beat either resumes cleanly or falls to CLEANUP; it must
   not wedge in RUNNING.

---

### Step 6 — Full COMPOSE: group selection, N attackers, fill verification

- [X] Complete (Claude's half; user in-game verification outstanding)

**[CLAUDE]**

**Goal:** Expand the spike into the real COMPOSE. After this step the beat spawns the Director's chosen group
at the Director's chosen size, with clean failure on every path.

**Files:** `src/AmbushBeat.cpp`, `include/AmbushBeat.h`.

**Sub-tasks:**

1. `OnStart` parses and clamps `attacker_count` via `JsonUtils::ClampParameterInt`, and extracts
   `attacker_group` through `LLMTextSanitizer::Sanitize`. Spawn distance is read from settings, not from the
   parameters. Store to session state under the existing mutex; no engine access in `OnStart` per `IBeat`'s
   contract.
2. `SelectingPoints` re-validates the sanitized group id against `AmbushAttackerGroups::EligibleGroups()` as it
   stands **now**, not as it stood at prompt-build time. Unknown or no-longer-eligible id falls back to
   `bandits` with a `logger::warn`. Then `AmbushSpawnPoints::Find(player, distance, count)`.
3. `Spawning` calls `ComposeRoster` and creates one reference per attacker at its clustered position,
   dispatching `FillAttackerSlot(i, ref)` for each.
4. `VerifyingFill` reads back slots `0..N-1`. All filled → `Arming`. Any missing after a bounded number of
   retry ticks → delete every reference created this attempt, then CLEANUP with a specific `failure_reason`.
5. `Arming` sets aggression to 0 and calls `EvaluatePackage()` on each attacker, then `SetStage(20)`.
6. Persist the chosen group id into the `'NAMB'` record so a post-reload log line can still name it.
7. Every failure path logs a distinct `failure_reason` string and reaches CLEANUP without stamping the
   cooldown.
8. Run `pwsh -File format.ps1`.

**Specifics:**

- Partial-spawn cleanup is the important correctness case: if attacker 4 of 6 fails to create, the three
  already created must be deleted, not left standing in the world with no quest tracking them.
- Never fill slots beyond `N-1`. Leaving slots 5–8 empty is the whole point of the `Optional` flag.
- The clamped count can be lower than the Director asked for; log both the requested and the resolved value so
  the prompt's effect is traceable.

**Verify [CLAUDE]:**

- `pwsh -File build.ps1 build` succeeds.

**Verify [USER]:**

1. Force-dispatch several ambushes in Whiterun Hold. Confirm the attacker count varies with what the Director
   asked for, and that the log's requested-vs-resolved line agrees with the number of bodies that appear.
2. Force-dispatch in The Reach until `forsworn` is chosen. Confirm Forsworn appear rather than bandits, and
   that the group includes a shaman and an archer at counts of 3+.
3. Set `iAmbushMinSpawnDistanceUnits` unreachably high (e.g. `100000`), force-dispatch, and confirm the beat
   fails cleanly with a spawn-point `failure_reason`, does **not** stamp the cooldown, and leaves no stray
   actors.

---

### Step 7 — RUNNING and CLEANUP: engagement, completion, abandonment, self-validation

- [X] Complete (Claude's half; user in-game verification outstanding)

**[CLAUDE]**

**Goal:** The full running lifecycle — the engage handoff, all three completion routes, and the stale-state
recovery path.

**Files:** `src/AmbushBeat.cpp`.

**Sub-tasks:**

1. RUNNING poll walks slots `0..N-1` and classifies each attacker alive / dead / gone.
2. Engage handoff: any live attacker inside `iAmbushEngageDistanceUnits` that hasn't engaged gets
   `SetActorValue(kAggression, 2)` natively plus a VM-dispatched `EngageAttacker(i)`. Track engaged state
   per-slot so it fires once per attacker.
3. All-dead → stage 200 → CLEANUP, cooldown stamped.
4. Abandon-by-distance: every survivor beyond `iAmbushAbandonDistanceUnits` → CLEANUP, cooldown stamped.
5. Abandon-by-timeout: accumulate `unpausedElapsedSeconds` from the tick argument per
   `feedback_tick_driven_accumulators` — no wall-clock timer of its own — and abandon past
   `iAmbushMaxDurationSeconds`.
6. **Stale-state self-validation:** the first RUNNING tick after a load verifies `_ne_AmbushQuest` is at stage
   20 with at least one filled attacker slot. If not, log and go straight to CLEANUP.
7. CLEANUP implements the design's ordering exactly: collect from aliases → delete survivors only → leave
   corpses → stage 200 → Stop → Reset → stamp cooldown if COMPOSE succeeded.
8. Freeze under any non-`Normal` `TickMode`, matching the other beats.
9. Run `pwsh -File format.ps1`.

**Specifics:**

- Delete before `Stop()`. Stopping clears the aliases, and the aliases are what keep the references persistent.
- "Gone" (alias filled but the reference no longer resolves, e.g. another mod deleted it) counts as dead for
  completion purposes. It must not stall the all-dead check.
- Corpses are intentionally left behind — do not "tidy up" by deleting them.

**Verify [CLAUDE]:**

- `pwsh -File build.ps1 build` succeeds.

**Verify [USER]:**

1. **All-dead:** dispatch, kill every attacker, confirm completion, cooldown stamp, and lootable corpses.
2. **Abandon by distance:** dispatch, immediately fast-travel away. Confirm the beat abandons within one poll
   cycle, the survivors are deleted, and the cooldown is stamped.
3. **Abandon by timeout:** set `iAmbushMaxDurationSeconds=60`, dispatch, then stand out of reach. Confirm
   abandonment at ~60 s and that survivors are deleted rather than left wandering.
4. **Engage handoff:** watch attackers close the distance. They should approach with weapons drawn and become
   hostile at roughly `iAmbushEngageDistanceUnits`, not before.
5. **Save/reload mid-fight:** confirm the fight resumes, aliases still resolve, and the beat still completes.
6. **Abort:** dispatch, then hit the dashboard's Abort. Confirm synchronous teardown, no cooldown stamp, no
   orphaned actors, and that a subsequent dispatch runs cleanly.

---

### Step 8 — Prompt integration and beat description

- [X] Complete (Claude's half; user in-game verification outstanding)

**[CLAUDE]**

**Goal:** Teach the Director about the beat and its attacker menu so it fires organically rather than only by
force-dispatch.

**Files:** `src/BeatSystem.cpp`, `statics/SKSE/Plugins/SkyrimNet/prompts/narrative_engine_action_select.prompt`,
`src/AmbushBeat.cpp`, `README.md`.

**Sub-tasks:**

1. Extend `BuildBeatSelectPromptContext` with `ambush_attacker_candidates`, following the
   `letter_sender_candidates` / `visit_sender_candidates` pattern exactly. Populate from
   `AmbushAttackerGroups::EligibleGroups()` when `ambush` is in the candidate list; each entry
   `{id, display_name, flavor}`.
2. Collect the eligible groups in `ConsiderBeat` alongside the existing letter and visit collectors. No
   minimum-candidate gate is needed — `IsAvailable` already requires a non-empty eligible set.
3. Add the matching template block to `narrative_engine_action_select.prompt`. Follow the file's documented
   indentation discipline: every `{% ... %}` scope tag on its own line, indented by nesting depth, with
   `{%- ... -%}` trim markers.
4. Rewrite `Description()` to the long-form text: what the beat does, when it fits, when it doesn't, the
   parameter schema, and an explicit statement that `attacker_group` MUST come from
   `ambush_attacker_candidates`.
5. Restore the Ambush bullet in `README.md`, worded for the new behavior (varied attacker groups, not
   bandits-only).
6. Run `pwsh -File format.ps1`.

**Specifics:**

- The prompt is a `statics/` file — it only reaches the runtime mod folder via `build.ps1`
  (`feedback_statics_require_build`).
- Defense in depth: even a valid group id that isn't currently eligible must be rejected by `OnStart`'s
  re-validation. The prompt being clear about the constraint doesn't remove the need for the check.

**Verify [CLAUDE]:**

- `pwsh -File build.ps1 build` succeeds.
- The rendered prompt (debug-logged) contains the ambush block with the correct eligible groups for the
  player's current position.

---

### Step 9 — End-to-end in-game validation

- [ ] Complete

**[USER]**

**Goal:** Confirm the beat fires organically, behaves across game states, and doesn't regress the other beats.
This is the gating step for phase completion.

**Test steps** (running game, `bDebugMode=true`, `bEventHistoryEnabled=true`):

1. Play a session in open wilderness until `ambush` fires **organically** — not force-dispatched. Confirm the
   Director's `narrative_note` and `parameter_justification` read sensibly for the group and size it chose.
2. Confirm attackers spawn out of view. If you can see them materialize, note where you were standing and what
   the log's spawn-point diagnostic said.
3. Fight the encounter to completion. Confirm the cooldown appears and `ambush` is unavailable until it
   expires; advance time with the `t` console command and confirm it becomes available again.
4. Repeat in The Reach and confirm `forsworn` gets picked at least once, with appropriate appearance and gear.
5. Over the same runs, confirm the Director picks more than one province-wide group and that its
   `narrative_note` justifies each in a way that fits — a coven acting on a grudge, Thalmor taking an
   interest, vampires striking after dark. On a late-game save that has completed Diplomatic Immunity and the
   Dark Brotherhood questline, confirm `thalmor` and `penitus_oculatus` both enter the rotation. If a group is
   never picked, or is picked in situations that read as nonsense, that is `Flavor`-text tuning in the shipped
   file, not a code change.
6. On a civil-war-aligned save, confirm the opposing faction's group is offered and can be chosen.
7. On Solstheim, dispatch until both `solstheim_reavers` and `rieklings` have been chosen at least once, and
   confirm each reads as well as the mainland groups do.
8. Enter a city and an interior. Confirm `ambush` shows unavailable on the Dispatch tab in both, and available
   again on exit.
9. Trigger a fight with an unrelated hostile, then confirm no ambush fires while the player is in combat.
10. Run a 30+ minute session of normal play. Confirm:
    - No crashes, no stuck top-level beat state, no orphaned actors accumulating.
    - `npc_letter` and `npc_visit` still dispatch normally.
    - No `LetterPool` or `VisitState` regressions in the log.
11. **Upgrade path:** load a save taken on the pre-removal build with an ambush mid-flight, if one exists.
    Confirm the log shows the skipped unrecognized `'NBAM'` record, that the beat does not wedge in RUNNING,
    and that a fresh ambush dispatches cleanly afterward.
12. **Data-driven proof:** hand-author a sixteenth group in the deployed `AttackerGroups.ini` — `conjurers` is
    the intended exercise, drawn from `LCharWarlockConjurer` / `LCharWarlockBossConjurer` with no eligibility
    keys — using only the comments in the file as documentation. Restart, confirm the summary line reports
    16 of 16 loaded (or 13 of 16 without Dragonborn), and force-dispatch until the new
    group is chosen and spawns correctly. **No rebuild, no code change.** If anything about authoring that
    group required reading the C++ rather than the file's own comments, say so — the comments are the
    deliverable here as much as the parser is.

**Success criteria:**

- Fires organically at least three times, with different groups where the game state allows it.
- Attacker count tracks what the Director requested, within the configured clamps.
- All three completion routes observed at least once across the session.
- No orphaned actors after any completion route.
- No regressions in the other two beats.

If spawn positions feel wrong in practice (too close, too visible, unreachable), tune the sampling and ranking
in Step 4 and rerun this step. If the Director over- or under-picks the beat relative to the other two, tune
`Description()` in Step 8 and note the final wording here.

---

## Done condition

Phase 11 is complete when:

- All 9 implementation steps are checked off.
- Step 9's validation run passes without intervention.
- The four open engine questions are all resolved, with their answers recorded in Post-implementation and any
  engine-behavior findings written up under `docs/engine-findings/`.
- Alias fill failure is observable and recoverable: a forced fill failure produces a clean CLEANUP with a
  specific `failure_reason`, never a wedged beat.
- The Director's chosen `attacker_count` is honored, verifiable by counting bodies against the log.
- At least three distinct attacker groups have been observed firing in their eligible game states, including
  at least one hold-gated group.
- A group can be added to `AttackerGroups.ini` and fire in game with no code change and no rebuild.
- A malformed group in `AttackerGroups.ini` is skipped with a named, specific log reason while every other
  group still loads.
- No orphaned dynamic references remain after any completion, abandonment, or abort path.

---

## Post-implementation additions

*Populated after implementation completes, mirroring Phase 09's practice. The four open engine questions'
answers land here, along with any modules, settings, or design shifts that arrived beyond the numbered plan.*

### Open engine questions — status

**Q3 (Mutagen's output for a fill-rule-less `Optional` alias) — ANSWERED.** Each `Attacker0N` alias serializes
with exactly `ID`, `Name`, `Flags: [Optional, AllowDead]`, `PackageData: [000832]`, and `VoiceTypes: Null`. There
is no `CreateReferenceToObject`, no `ForcedReference`, and no `Conditions` block — Mutagen simply emits nothing
for the absent fill rule, which is the shape we wanted. A deserialize → serialize round-trip through Spriggit
returns the record byte-identical, so the CK's own save doesn't reintroduce a fill rule. Whether the *engine*
then treats it as empty rather than fill-failed is the half that still needs a running game.

**Q1 (leveled-list resolution) — ANSWERED: NO.** `CreateReferenceAtLocation` does **not** roll a
`TESLevCharacter` base. It type-checks (LVLN derives `TESBoundObject`), returns a valid handle, and produces a
reference whose base object is still the leveled list — so the reference never becomes an `Actor`. The documented
fallback is now the implementation: `ResolveLeveledCharacter` calls `TESLeveledList::CalculateCurrentFormList`
(the engine's own resolver, so list flags / chance-none / level filtering behave natively) and passes the
resulting `TESNPC*` as the base, recursing for nested lists with a depth cap.

Found on the first in-game test, and the way it presented is worth knowing:
`BGSRefAlias::GetReference()` returns the placeholder while `GetActorReference()` returns null, so the alias
reads as *filled* to every population check while every `Actor`-typed read of the same slot yields nothing. The
beat armed nothing, logged `armed 3 attacker(s); COMPOSE complete`, and completed the encounter on the first
RUNNING poll with all three slots classified as gone — five seconds, no warnings, no attackers. Written up under
`docs/engine-findings/` as `createreferenceatlocation-does-not-resolve-leveled-lists.md`.

**Q2 (`AllowDead` alias retention) — STILL OPEN.** It needs an attacker to actually die, which the Q1 bug
prevented. The RUNNING poll distinguishes "dead" (alias still resolves, actor `IsDead`) from "gone" (alias no
longer resolves) and treats both as down, so the beat completes either way; the log line says which happened.

**Q4 (navmesh reachability) — ANSWERED, with a caveat.** CommonLibSSE-NG exposes no callable navmesh query at
all; every navmesh header is data-layout only with zero `REL::Relocation` bindings. It *does* expose the raw
triangle data via `cell->GetRuntimeData().navMeshes`, so `AmbushSpawnPoints::IsOnNavmesh` answers **containment**
directly with a barycentric point-in-triangle test rather than falling back to the planned ground-height
raycast. **Connectivity** — "is that the same navmesh island the player is on?" — is deliberately not
implemented; it needs a triangle-graph BFS across portals and cells, and abandon-by-timeout covers the same
failure more cheaply. The post-spawn settle check specified alongside the raycast fallback **is** implemented,
as COMPOSE's `Settling` sub-phase — see below. Written up in
[`navmesh-queries-in-commonlibsse-ng.md`](../engine-findings/navmesh-queries-in-commonlibsse-ng.md).

One assumption inside that gate is still unverified: vertex coordinates are taken to be world-space. If they are
actually cell-local, the symptom is unmistakable — the spawn search logs `noNavmesh=16` at every radius and no
ambush ever spawns.

### Test 2: alias fills timed out — reference arguments don't survive the VM dispatch

With leveled resolution fixed, the spawn succeeded and every alias fill failed: `0/3 attacker aliases filled
after 20 ticks`, `failure_reason='alias_fill_timeout'`. The plugin log had nothing else to say, because
`VMDispatchOnQuest` is fire-and-forget and returns true for *queued*, not for *succeeded*. The cause was only
visible in `Papyrus.0.log`:

```text
Error: alias Attacker01 on quest _ne_AmbushQuest (FE0E7831):
       Cannot force the alias's reference to a None reference.
```

Passing the spawned `TESObjectREFR*` directly into `MakeFunctionArguments` produces an argument that is **not
`None`** on the script side but **unpacks to null** inside `ForceRefTo`'s native — so the script's own
`akRef == None` guard passed and the native failed on the next line. `FillAttackerSlot` now takes an `int`
FormID and resolves it itself via SKSE's `Game.GetFormEx` (not `Game.GetForm` — dynamically-created references
live at `0xFF......` and need the full 32-bit range). That removes the handle-packing step and moves resolution
to VM-execution time. Written up under `docs/engine-findings/` as `passing-references-to-papyrus-from-cpp.md`.

Note this needed **no Creation Kit work**: `.psc` files compile through `build.ps1` via `PapyrusCompiler.exe`,
and a function's parameter types aren't recorded in the quest's VMAD — only the script name and its properties
are, and neither changed.

A `spawned slot N ref=… base=… '<EditorID>'` line was added at the creation site, so the C++ side now records
what it handed over independently of whether the VM does anything with it.

### Test 4: it worked, then ate the corpses

The fourth run spawned three bandits, fought them, and completed correctly — and every body vanished the moment
the beat cleaned up, so there was nothing to loot.

Worth recording that this was **not** a stray delete. `DeleteCreatedRefs` skips dead actors, and the log proves
the guard held: it only logs when it actually deleted something, and the successful run's cleanup emitted the
cooldown line with no deletion line at all. The corpses were reaped by the engine as a consequence of
`Stop()` / `Reset()` releasing the aliases — the same alias-held persistence the original cleanup ordering
relied on for its "delete before stopping" rule. Leaving corpses and resetting the quest were mutually
exclusive as written.

Fixed by deferring the quest teardown to the next dispatch — see **Cleanup ordering** above for the full shape.

### Test 5: three encounters, and the beat freezing itself out of its own fight

Three encounters in three holds all worked end to end (bandits at Embershard, Forsworn in the Reach, vampires in
Haafingar). Three defects surfaced, and two of them turned out to be the same defect.

**Attackers arriving late and scattered — one root cause.** `Tick` froze on any non-`Normal` `TickMode`,
matching the social beats. But an ambush *creates* combat, so the beat's own spawn flipped the mode within a
second and froze its own COMPOSE mid-flight: 31 seconds in the Forsworn encounter, **59** in the vampire one.
Throughout that window the attackers existed but were unarmed — carrying their base aggression and running
their own combat AI instead of the approach package. That is both "they didn't come at me" and "they were
spread out", from a single cause.

`AmbushBeat::Tick` now freezes **only** on `Paused`. Combat and Dialogue proceed: once references exist in the
world, half-built state is worse than finishing setup at an awkward moment. This is a deliberate divergence
from the letter and visit beats, which are social beats where pausing for combat is correct.

Aggression is now also zeroed at **spawn** rather than at `Arming`, closing the window where a freshly-created
attacker fights on its own initiative. `Arming` re-applies it; doing it twice costs nothing.

**Spawning in water.** Skyrim's navmesh covers plenty of lake and river bed, so containment happily accepted a
spot under water and the attacker spawned swimming. There is now a water gate in the search, plus `IsInWater()`
folded into the post-spawn settle check as positive evidence.

Worth recording the API choice: `TESObjectCELL::GetWaterHeight` returns a **bool** for whether water exists at
all, while the `TES`-level overload returns a bare float with a sentinel for "no water". Using the latter would
have been a trap — plenty of Skyrim terrain sits at large negative Z (the vampire encounter's winning point was
at `z = -14020`), so a misread sentinel would have rejected every candidate in low-lying areas.

**Cluster spread.** `kClusterRadiusUnits` reduced 220 → 150. Note this was probably the *smaller* contributor:
most of the observed scatter is explained by the 31-59 second unarmed window above, during which the attackers
wandered under their own AI. Worth re-judging after a run with the freeze fixed.

### Test 6: the Director should never have had a say in spawn distance

`spawn_distance_units` was offered to the Director as an optional parameter. It returned 256, 128, and 256
across three encounters — two to four metres, i.e. materializing on top of the player. `ClampParameterInt`
raised each to the `iAmbushMinSpawnDistanceUnits` floor, so the parameter's only real effect was to override
the configured default of 2000 with the minimum of 1500 on every single dispatch. A knob whose entire
observable behavior was "always pick the worst legal value" is worse than no knob.

Removed: `OnStart` reads `iAmbushDefaultSpawnDistanceUnits` directly, `Description()` simply no longer lists
the parameter, and a stray `spawn_distance_units` in a response is ignored with a debug line so prompt drift
stays visible.

The prompt says nothing about the removal, deliberately. An initial version added "do not send any others — in
particular, where the attackers appear is not yours to choose", which is worse than silence: it spends tokens
describing a capability the schema never offered, and names the exact thing you don't want the model reaching
for. The right way to withhold an option from an LLM is to not list it.

The general lesson is worth keeping: an LLM parameter is only worth offering when the model has information the
code doesn't. It knows who should attack and roughly how many, because those are narrative judgements grounded
in the context it was given. It knows nothing about terrain, sightlines, or what 128 game units looks like.

### Test 7: spawn placement reprioritised

The original ranking preferred the **rear** hemisphere, on the theory that attackers should come from behind.
In play that reads wrong: the player never sees them coming, and an encounter that materialises at your back is
harder to distinguish from a bug than from an ambush. Reversed — the forward arc (ahead through square to
either side) is now the preferred placement, with the rear kept as an explicit fallback.

Three coupled changes:

- **Ranking** sorts forward-arc before rear, then by distance closeness. No preference *within* the forward
  arc: dead ahead and perpendicular are equally good.
- **Early exit** now requires a *forward* hit. Previously the search stopped at the first radius yielding any
  survivor, which under the new ordering would frequently lock in a rear fallback while a wider ring had cover
  in front. Survivors accumulate across radii so nothing is wasted.
- **Band widened** to 2000–5000 (from 1500–3500), with every radius clamped into it and the multiplier ladder
  extended to ×2 and ×2.5 to actually reach the top of the band. The narrowing ×0.75 step was dropped: with the
  minimum as a hard floor it only ever clamped back onto the first ring.

Knock-on: `iAmbushAbandonDistanceUnits` raised 6000 → 8000. At the old value an encounter spawning at the far
end of the new band would have had just 1000 units of slack and could abandon itself as soon as the player kept
walking.

The visibility gate is doing much more work under this ordering — positions in front of the player are usually
in view, so "in front but hidden" is a genuinely scarce combination. Expect a higher share of `visible=` in the
gate tally, and expect the rear fallback to be exercised in open terrain with no cover.

### Test 8: attackers spawning in plain view

Bandits spawned fully exposed in a spot that had usable cover available. The gate tally is the tell:

```text
sampled=32 noCell=0 interior=0 noGround=0 underwater=5 noNavmesh=15 visible=4 survived=8
```

Only 4 of the 12 candidates that reached the visibility gate were rejected. Eight passed as "hidden" while
standing in the open, so the gate was answering the wrong question rather than answering it too leniently.

Three causes, fixed together:

1. **`bhkPickData::rayInput.filterInfo` was left at its default `0`** (`kUnidentified`). The ray collided by
   whatever rules layer 0 carries instead of the sight rules, producing spurious blocked results — a spurious
   block reads as cover. Now set to `COL_LAYER::kLOS`. This is the likeliest primary cause, and it also
   silently affected `IsAnyPartVisibleFromCamera`, where the `HasLineOfSight` positive short-circuit had been
   masking it.
2. **A single vertical column of three rays** was too thin a sample to establish cover for a body, let alone a
   group. Replaced with a nine-ray silhouette widened to the cluster radius — see **The cover gate** above.
3. **Angular resolution too coarse to find cover.** At 2500 units, 16 spokes leave ~980 units of arc between
   samples while a boulder's shadow is a few hundred wide, so the search stepped over usable pockets. Raised
   to 32.

The old `IsPositionVisibleFromCamera` is gone rather than left alongside; it had exactly one caller and its
fail-toward-"hidden" direction was wrong for that caller.

**A cover-less fallback tier was added immediately afterwards.** The first cut of this change made an open
plain produce `survived=0` and a clean `no_spawn_point` failure — correct by the letter of "must be behind
cover", wrong in practice, because it meant whole regions where ambushes could never happen at all. Tier 3
above closes that: uncovered points are retained rather than discarded, and used only when nothing covered
exists anywhere. See **Spawn point selection**.

### Post-test hardening

The first in-game test exposed three diagnostic and control-flow holes that, together, turned a hard failure
into a run that logged nothing but success lines. All three are fixed, and they generalize past this phase:

- **The per-slot fill diagnostic was gated on the thing it was diagnosing.** It was written as
  `if (auto* actor = AttackerInSlot(i)) { log(...); }`, so when the Actor cast failed — the exact case that went
  wrong — it printed nothing. It now logs every filled slot unconditionally, reporting the raw ref, the base
  FormID, the base's `GetFormType()`, and an explicit `isActor` flag.
- **`Arming` logged the intended count rather than the achieved one.** `armed {expected}` was printed by a
  compose that armed zero attackers. It now reports `armed {n} of {expected}`, fails with `nothing_armed` when
  `n == 0`, and narrows `g_activeCount` to what it actually armed.
- **The settle check read "found nothing to check" as "nothing wrong."** `checked == 0` with `expected > 0` now
  fails with `no_attacker_resolved` instead of falling through to `Arming`.

`VerifyingFill` additionally hard-fails with `spawned_ref_not_actor` when slots are filled with references that
aren't actors, rather than letting a non-actor roster reach RUNNING.

### Arrivals beyond the numbered plan

- **A `Settling` COMPOSE sub-phase**, between `VerifyingFill` and `Arming`. The design put the post-spawn settle
  check in Step 5 as half of the raycast fallback; it is worth keeping even though navmesh containment replaced
  the raycast, because containment answers a question about a *point* and this answers one about an *actor*.
  After the aliases fill, the beat waits `kSettleWaitSeconds` (1.5 s, accumulated from the tick argument, never
  a timer of its own) and then compares each attacker's resting position against the position it was created at.
  More than `kMaxSettleDriftUnits` (300) of vertical drift, or a resting position that fails `IsOnNavmesh`,
  counts as unsettled — that is the actor that fell through the world, slid off a ledge the containment test
  couldn't see, or was ejected out of a rock.
  Unsettled attackers are relocated **once**, onto the search winner's position (the best-validated point we
  have), then re-checked after another settle window. The retry is latched so a position that keeps reading
  unsettled can't loop COMPOSE. After the retry, a *partial* failure proceeds to `Arming` with a warning — those
  actors are standing on the winning point, which passed every gate, so the check is likelier wrong than the
  ground is — while a *total* failure fails COMPOSE with `settle_failed`, since nobody settling anywhere is a
  systematic signal rather than bad luck.
  One API note worth keeping: `Actor` overrides `TESObjectREFR::SetPosition` with a second
  `a_updateCharController` parameter. It must be `true`, or the character controller stays behind and the actor
  walks back to where it was.
- **`CameraVisibility::IsPositionVisibleFromCamera(worldPos, probeHeightUnits)`** — a new overload. Step 4
  specified `IsAnyPartVisibleFromCamera` for the visibility gate, but that takes a `TESObjectREFR*` and the spawn
  search evaluates candidate positions *before* anything exists at them. The new overload probes a three-point
  vertical column (feet / torso / head) at the bare position instead of walking a skeleton, and shares the
  existing camera resolution and raycast helpers.
- **`AmbushAttackerGroups::EligibleGroupSummaries(MainThread::Token)`** — a plain-data flattening of the eligible
  set, added because `BuildBeatSelectPromptContext` runs on the *plugin* thread while eligibility evaluation is
  main-thread. Returning `{id, displayName, flavor}` strings rather than `const Group*` keeps engine-owned
  pointers from escaping to a worker thread, per `docs/THREADING_MODEL.md`.
- **The ambush menu is gathered inside `FireBeatSelectLLM`**, not in `BuildBeatSelectPrep` as the letter and
  visit collectors are. `FireBeatSelectLLM` already holds a `PluginThread::Token`, so one `MainThread::Run` there
  serves both `ConsiderBeat` and `ForceDispatchBeat`; collecting in the prep would have meant duplicating the
  marshalling, since `ForceDispatchBeat` takes no token at all.
- **`RequireGlobal` repeats AND rather than OR.** The design doc states this, but it is worth restating as the
  one exception to "repeating a key ORs its values": every other key names an entity on a single shared
  dimension, so repeats are alternatives, while each `RequireGlobal` is a whole predicate over a *different*
  variable. `Forbid*` keys are "none of these may match" regardless.
- **Additional settings clamps.** Beyond the specified attacker-count ceiling, `ReadIniInto` now also orders the
  min/default/max triples for both count and distance, and raises `iAmbushAbandonDistanceUnits` above
  `iAmbushMaxSpawnDistanceUnits` when a hand-edited INI inverts them — otherwise an ambush abandons itself on the
  tick after it spawns.
