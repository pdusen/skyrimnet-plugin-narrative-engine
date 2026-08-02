# CreateReferenceAtLocation does not resolve leveled lists

## TL;DR

`TESDataHandler::CreateReferenceAtLocation` accepts a `TESLevCharacter`
as its `a_base` and returns a valid reference handle — and that
reference **never becomes an `Actor`**. The leveled list is not rolled;
you get a placeholder reference whose base object is still the LVLN.

Resolve the list yourself with
`TESLeveledList::CalculateCurrentFormList` and pass the resulting
`TESNPC*` as the base instead.

```cpp
RE::BSScrapArray<RE::CALCED_OBJECT> calced;
list->CalculateCurrentFormList(playerLevel, 1, calced, 0, /*a_usePlayerLevel=*/true);
// first entry whose form is a TESNPC; recurse if it's a nested TESLevCharacter
```

That's the engine's own resolver, so list flags, chance-none, and level
filtering behave the way the game would do them. Recurse for nested
lists, with a depth cap — a malformed mod-added list can be cyclic.

## Why it type-checks but doesn't work

`TESLevCharacter` derives from `TESBoundObject`, so it satisfies
`a_base` at compile time with no cast and no warning. The call succeeds,
the handle is valid, `handle.get()` gives you a real `TESObjectREFR`.
Everything looks right.

Papyrus's `PlaceAtMe` *does* resolve leveled bases, which is where the
wrong intuition comes from. `CreateReferenceAtLocation` is a lower-level
entry point and does not.

## The symptom, and why it is so hard to read

This is the part worth remembering. The failure does not look like a
failure:

- `BGSRefAlias::GetReference()` returns the reference — **non-null**.
- `BGSRefAlias::GetActorReference()` returns **null**.

So an alias force-filled with one of these placeholders reports as
*filled* to every "is the alias populated?" check, while every
`Actor`-typed read of the same slot silently yields nothing.

In `AmbushBeat` that produced a beat that looked like it rolled itself
back for no reason:

1. `VerifyingFill` counted filled slots via `GetReference()` → 3 of 3,
   passed.
2. The per-slot diagnostic log was written as
   `if (auto* actor = AttackerInSlot(i)) { log(...); }` — the cast
   failed, so it printed **nothing at all**. The one line that would
   have explained everything was silently skipped.
3. The settle check skipped null actors, ended with `checked == 0`,
   `unsettled == 0`, and read that as "nothing wrong".
4. `Arming` skipped null actors, armed nothing, and then logged
   `expected` rather than the count it had actually armed — printing
   `armed 3 attacker(s); COMPOSE complete`.
5. The first RUNNING poll classified all three slots as gone (no actor)
   and completed the encounter.

Total elapsed: five seconds, three "successful" log lines, no warnings,
no errors, and no attackers. The log was actively misleading.

## Lessons for diagnostics

Three logging patterns conspired here, all of them worth avoiding:

- **Never gate a diagnostic on the thing it is diagnosing.** Logging
  inside `if (cast succeeded)` means the failure case is invisible. Log
  the raw handle first, then report what it turned out to be —
  including the base form's `GetFormType()`, which names the problem
  outright.
- **Never log the intended count when the actual count is available.**
  `armed {expected}` was a lie the code had no reason to tell;
  `armed {n} of {expected}` would have shown `0 of 3`.
- **"Zero of the things I was looking for" is not "nothing wrong."**
  A validation pass that finds nothing to validate should fail loudly,
  not fall through.

`AmbushBeat` now hard-fails with a specific `failure_reason` at each of
those three points (`spawned_ref_not_actor`, `no_attacker_resolved`,
`nothing_armed`) rather than proceeding.

## Related

- [`starting-a-quest-from-cpp.md`](starting-a-quest-from-cpp.md) — the
  alias-fill side of the same subsystem.
- `PHASE_11_AMBUSH_BEAT.md`, open engine question 1.
