# Name Resolution — IntelEngine Failure Modes

When NarrativeEngine eventually needs to resolve LLM-supplied names back to game entities (actors,
locations, references, items, markers), the IntelEngine resolver stack is the relevant prior art.
It has a working four-stage cascade *and* a documented track record of producing surprising wrong
answers. This document captures both — the strategy and the specific reasons it breaks — so we
don't ship a resolver that repeats the same mistakes.

The summary up front: **fuzzy name matching is brittle in ways that aren't obvious until they
fail in front of you, and the bugs are silent — the dispatcher just hands the action to the wrong
entity and the player blames the LLM.** Plan for it.

---

## IntelEngine's resolver topology

IntelEngine has three subsystems that resolve names back to game entities, plus a fourth for
runtime-relative destinations:

| Subsystem | Source | Returns | Index source |
|-----------|--------|---------|--------------|
| `NPCIndex::FindByName` | `src/NPCIndex.cpp:520` | `RE::Actor*` | TESNPC base-form `GetFullName()` at startup, plus runtime `IndexLoadedNPC` updates |
| `LocationResolver::ResolveCell` / `ResolveLocation` | `src/LocationResolver.cpp:118`, `:154` | `RE::TESObjectCELL*` / `RE::BGSLocation*` | All cells + BGSLocations at startup |
| `ItemIndex::FindByName` | `src/ItemIndex.cpp:207` | `RE::TESBoundObject*` | All weapons/armor/misc/spell tomes at startup |
| `LocationResolver::ResolveSemantic` | `src/LocationResolver.cpp:514` | `RE::TESObjectREFR*` | Live door scanning ("outside", "upstairs", …) |

Markers and references-by-name aren't fuzzy-resolved at all. NPC home beds use a
`FormID → (cellId, bedId)` map (`m_npcHomeIndex`), and semantic destinations resolve at runtime by
walking nearby doors. This is the **healthier** end of IntelEngine's design and we should pay
attention to which entity classes earned a fuzzy resolver vs. which didn't.

All three fuzzy resolvers share `StringUtils::FuzzyFind` and `StringUtils::TokenFuzzyFind` from
`src/StringUtils.h`. The two helpers are very different in quality, and which one each resolver
picked matters.

### The two fuzzy helpers

**`StringUtils::FuzzyFind` (`StringUtils.h:228`)** — Levenshtein-only. Iterates candidates,
computes edit distance, applies a `dist >= min(searchLen, candidateLen)` guard, returns the
lowest-distance match within `maxDistance`. **No article handling, no token logic, no
substring step inside this function.**

**`StringUtils::TokenFuzzyFind` (`StringUtils.h:265`)** — Three-phase cascade. Article-stripped
exact match first, then token-overlap scoring (each search word must appear as a whole word in
the candidate, score = matches / meaningful), then falls back to `FuzzyFind`. Strictly better
for natural-language place names like "the Bannered Mare."

The asymmetry that drives several of the failure modes below: **`NPCIndex::FindByName` does
*not* use `TokenFuzzyFind`** — it goes straight to `FuzzyFind`, then to its own *substring*
step (`NPCIndex.cpp:567-590`), then to a live-scan fallback that also uses substring. The
location resolvers use `TokenFuzzyFind`. The NPC resolver is the one with the bugs.

---

## Actor resolution failure modes

The two observed failures both come from `NPCIndex::FindByName`:

- The player is named "Sten." The LLM mentions "Sten" as a dispatch target. The resolver
  returns the unrelated NPC **Stenvarr** instead of the player.
- The game has a base-form NPC named **`<PLAYER_NAME>'s Shadow`** (a placeholder string in the
  TESNPC `fullName` field, resolved at display time). The LLM mentions the player by name. The
  resolver returns the Shadow NPC instead of the player.

### Tracing the Sten → Stenvarr bug

`NPCIndex::FindByName("Sten")` runs a four-stage cascade:

1. **Exact match in `m_npcIndex`** — the loaded-actor map keyed by `GetDisplayFullName()`. If
   the player is currently a loaded actor and was processed by `IndexLoadedNPC`, this *should*
   succeed. In practice this step is fragile — the player isn't always in
   `ProcessUtils::ForEachLoadedActor`'s iteration depending on timing, and any LLM-supplied
   name with extra whitespace, an article, or a slight spelling variation skips the exact path.

2. **`FuzzyFind` over `m_allNames`** — `m_allNames` is built **only from Phase 1 base-form
   indexing** (`NPCIndex.cpp:384`). `IndexLoadedNPC` updates `m_npcIndex` and `m_npcFormIds`
   but *never appends to `m_allNames`*. The player's base-form name is the default
   character-creation string (e.g., "Prisoner"), not their runtime name, so **"Sten" is
   structurally absent from the fuzzy candidate pool**. The pool contains "Stenvarr" though.

   `FuzzyFind("sten", ["stenvarr", …], maxDist=3)` computes distance 4. The guard
   `dist >= min(4, 8) → 4 >= 4` *rejects* Stenvarr. So this step correctly declines, too.

3. **Partial substring match (`NPCIndex.cpp:567-590`)** — iterates `m_allNames` again, this time
   returning the first candidate where `candidate.find(search) != npos` OR
   `search.find(candidate) != npos`. `"stenvarr".find("sten") == 0` → **MATCH**. The substring
   step has *zero* quality threshold and *zero* comparison against other candidates. First hit
   wins. **This is where the Sten → Stenvarr bug actually fires.**

4. Live-scan fallback. Never reached.

### Tracing the `<PLAYER_NAME>'s Shadow` bug

Same shape. The base-form TESNPC has `fullName = "<PLAYER_NAME>'s Shadow"`. When indexed via
`GetFullName()` during Phase 1, either the literal placeholder string ends up in `m_allNames`,
or the engine's text-replacement system substitutes the player's actual display name so the
indexed entry becomes `"<actual_player_name>'s shadow"`. Either way:

- Exact match: fails.
- `FuzzyFind`: Levenshtein distance from "sten" to "sten's shadow" is 9 (nine insertions). The
  `dist >= minLen` guard catches it (`9 >= 4`), so this step *correctly* declines.
- **Substring match: `"sten's shadow".find("sten") == 0` → MATCH. Returns the Shadow NPC.**

Same root cause, different victim. The substring step has no notion of "the candidate is
dramatically longer than the search term, this is probably wrong."

### What the resolver got right and what it got wrong

The Levenshtein step and its `dist >= minLen` guard were doing their job in both cases — they
declined the bad matches. The bug isn't in the fuzzy algorithm; it's in the *next* step that
fired specifically *because* the fuzzy step was disciplined enough to say "no."

The architectural mistake is the cascade philosophy itself: **each step is more permissive than
the last, with no quality threshold and no margin-over-alternatives check, because the cascade
is built around the assumption that returning *some* answer is better than returning no
answer.** For LLM-driven dispatch the opposite is true — a wrong answer dispatches the wrong
NPC silently, while a "not found" failure can be logged and the next LLM tick can retry.

The cascade also bakes in **no awareness of who is being searched for**. The player should
never be a candidate when the resolver is asked "find an NPC to dispatch as a messenger." A
single-purpose resolver can't enforce that filter; it doesn't know the question.

---

## Location and item resolution

`LocationResolver::ResolveCell` (`LocationResolver.cpp:118`) and `ResolveLocation`
(`LocationResolver.cpp:154`) use a two-step cascade:

1. Exact lowercase match in `m_cellIndex` / `m_locationIndex`.
2. `TokenFuzzyFind` against `m_allCellNames` / `m_allLocationNames`.

No substring step. No live-scan fallback. **The location resolvers don't carry the substring
trap that bites NPC resolution.** They're more disciplined — `TokenFuzzyFind`'s article-strip
and token-overlap phases handle the realistic failure cases ("Bannered Mare" vs. "the Bannered
Mare", "Whiterun Marketplace" vs. "Whiterun Market") cleanly without inviting prefix-substring
collisions.

`ItemIndex::FindByName` (`ItemIndex.cpp:207`) uses `FuzzyFind` (not `TokenFuzzyFind`) with a
hardcoded maxDist of 3. No substring step, no live scan. The maxDist is generous for short
item names — "Iron Sword" (10 chars) vs. "Iron Sord" or "Iron Sworn" both pass — but it doesn't
have the *unconditional-prefix-match* failure mode the NPC resolver has.

### Still-present hazards in location and item resolution

The non-NPC resolvers are safer but not immune. The remaining hazards to watch for:

- **Disambiguating duplicates.** Skyrim has multiple "Inn"-named cells, multiple "Mine"-named
  cells (Halted Stream Mine, Embershard Mine, Iron-Breaker Mine), multiple shrine cells per
  divine. The first-stored-wins behavior in `m_locationIndex.find == end → insert` means the
  exact-match step returns *some* answer for ambiguous names without surfacing the ambiguity.
- **Editor IDs and display names share namespace.** `BuildLocationIndex` inserts both the
  display name and the editor ID into `m_locationIndex` (`LocationResolver.cpp:90-96`). An
  editor ID like `WhiterunDragonsreach` doesn't collide with the display name "Dragonsreach"
  for exact matches, but if the LLM ever produces an editor-ID-shaped name it gets a free hit
  on the wrong tier of identifier.
- **TokenFuzzyFind on very short single-word names.** "Inn" alone (3 chars) tokenizes to one
  meaningful word; a candidate "Inn of the Bee" tokenizes to ["inn", "bee"], score = 1/1 = 1.0.
  Returns first-such-candidate. The guard rule in the underlying `FuzzyFind` doesn't apply here
  because the token path resolves before falling through.
- **Falls back to plain `FuzzyFind` if token matching fails.** `TokenFuzzyFind`'s phase 3 is
  the same Levenshtein-only logic the NPC resolver uses. Without a substring trap after it,
  this is fine — but it means the underlying `dist >= minLen` and global-`maxDistance`
  thresholds still need to be tuned for the worst-case short-name candidate, not just the
  expected case.

---

## Markers and references-by-name

IntelEngine notably *doesn't* fuzzy-resolve markers or runtime references:

- **NPC home beds** live in `m_npcHomeIndex` keyed by `FormID → (cellId, bedId)` (`LocationResolver.cpp:1057-1115`). Caller already has the FormID; no name lookup happens.
- **Semantic destinations** ("outside", "upstairs", "downstairs", "out of cell") resolve at
  runtime by enumerating doors near the actor and matching the relevant teleport target. No
  text fuzzy matching against a name pool.
- **Map markers** (REFRs with the `MapMarker` extra data) have no resolver in IntelEngine that
  we found.
- **Dungeon endpoints** sit in a `m_dungeonIndex` keyed by cell FormID, not by name.

This is the right instinct. A marker or runtime reference is identified by its FormID, and the
FormID is known to whoever placed the marker (the CK author, or the runtime code that called
`PlaceAtMe`). There's no natural-language input to disambiguate, so there's no reason to invite
the fuzzy-resolution failure modes into this layer at all.

**For NarrativeEngine: if we use teleport-to-marker patterns (the same dispatch shape ambush
uses), the marker is a FormID we either authored into the ESP or created at runtime. We should
never need to name-resolve a marker. If the design ever pushes us toward "the LLM names a
marker by string," that's the design we should change, not the resolver we should write.**

---

## Distilled failure-mode taxonomy

Six structural weaknesses combined to produce the observed IntelEngine bugs. The taxonomy is
worth memorizing because each is independently avoidable:

1. **Cascade-with-no-floor.** Each fallback is more permissive than the last, with no quality
   threshold and no "return nothing." When you reach the last step you return *something*
   because the cascade has nothing else to do.
2. **Unconditional substring matching.** The candidate just has to *contain* the search term
   anywhere. No relative-quality comparison ("is there a better-fitting candidate?"), no
   length-ratio check ("is the candidate dramatically longer than the search?"), no margin
   requirement ("does this beat the runner-up by N edits?").
3. **Missing entities not in the index.** Phase 1 indexes only base-form names. Phase 3
   refreshes `m_npcIndex`/`m_npcFormIds` but never updates `m_allNames`. The player, named
   followers, mod-added named refs, summoned creatures — anyone whose name only exists at
   runtime — is invisible to the fuzzy step. Exact match is their *only* path to being
   selected, and any LLM input that misses exact match silently routes elsewhere.
4. **Placeholder-containing names get indexed verbatim.** `<PLAYER_NAME>'s Shadow` ends up in
   the candidate pool with the placeholder either unresolved or resolved to the player's name.
   The substring step then traps every LLM mention of the player into the wrong actor.
5. **Absolute Levenshtein threshold, not relative.** A threshold of 3 lets 3 of 4 characters be
   wrong in a 4-character name. The `dist >= minLen` guard mitigates the worst cases but the
   threshold being absolute means short names are inherently more dangerous than long ones.
6. **No type or role filter at resolve time.** The resolver doesn't know whether the caller
   wants "an NPC who could be the messenger" (player ineligible, hostiles ineligible,
   followers contextually ineligible) or "a target to address" (player eligible). One answer,
   no semantics. Filtering at the *caller* after a wrong actor returns is too late.

---

## Principles for NarrativeEngine's resolver

These apply to *any* entity type — actors, locations, references, markers, items — not just
NPCs:

1. **Resolve by ID, not by name, when we can.** When we hand a candidate pool to the LLM, hand
   it `(id, display_name, …)` rows and instruct it to return the id. Name fuzzy-matching is a
   degraded fallback path, not the primary one. The candidate-pool-isolation pattern IntelEngine
   *also* uses (`m_dmCandidatePool`, `m_npcCandidatePool` in `NPCIndex.h:621-622`) is the
   better part of that codebase — lean on it.

2. **Special entities are first-class, not just rows.** The player isn't an NPC. Markers
   placed by the CK author aren't anonymous references. Locations the player has visited
   aren't equivalent to never-discovered ones. Carve special-case predicates
   (`IsPlayerReference`, `IsKnownMarker(formId)`, …) and check them *before* the fuzzy pool
   ever sees the query. Fuzzy is the path for ordinary entities; special entities have their
   own paths.

3. **Drop substring matching entirely.** It's the source of the worst NPC failure modes and it
   contributes nothing the token-overlap path doesn't handle better. If we want a "permissive
   final fallback," make it token-overlap with a strict score threshold (≥0.75) and a margin
   requirement against the runner-up.

4. **Filter the index of names that contain placeholder syntax.** Any TESNPC, location, or
   item whose `GetFullName()` contains `<` is excluded from the resolution pool. These are
   meant to be display-only strings; they should never be dispatch targets.

5. **Use a relative Levenshtein threshold, not absolute.** Roughly: `dist <= max(1, candidateLen / 4)`,
   with an additional rule that the result must beat the runner-up by at least 2 edits. Short
   names get strict matching; long names get the slack they need for typos; ambiguity between
   two similar candidates yields no match.

6. **Type-filter at resolve time, not after.** The resolver takes a filter parameter — "must
   be a non-hostile dispatchable NPC who is not the player and is currently loaded" — and
   applies it during candidate iteration. Filtering prunes the trap before substring or fuzzy
   even sees it.

7. **Return confidence, not just a result.** Caller decides whether low-confidence is
   acceptable. For Director dispatches, *fail closed*: low-confidence resolution → no dispatch
   this tick, log the bad name, let the next tick try again. Silent wrong-target dispatch is
   the worst failure mode possible because the player attributes the error to the LLM.

8. **Log every fuzzy resolution with margin.** Make it visible in the dashboard's decision log
   when a fuzzy resolution happened, what the search was, what the chosen result was, what
   distance/score it had, and what the runner-up was. The IntelEngine bugs went undiagnosed
   for a long time because the dispatcher silently routed to the wrong actor with no signal
   that anything unusual happened.

The practical takeaway: when the time comes to wire up an LLM-driven dispatch that picks a
sender NPC, a meeting location, or a specific reference, **we should expect to resolve the
LLM's response by id from a curated pool, with name-based resolution as the degraded
fallback.** If a Phase doc ever sketches "the LLM returns a name and we look it up," push back
on the design.
