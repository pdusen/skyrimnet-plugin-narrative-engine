# Passing references to Papyrus from C++

## TL;DR

Do **not** pass a `TESObjectREFR*` straight into `MakeFunctionArguments`
when VM-dispatching a Papyrus function. It compiles, it dispatches, and
the argument arrives on the script side as something that is **not
`None`** but **unpacks to null** inside any native that consumes it.

Pass the `FormID` as an `int` instead and resolve it in Papyrus with
SKSE's `Game.GetFormEx`:

```cpp
QuestUtils::VMDispatchOnQuest(quest, "MyScript", "MyFunc",
                              static_cast<std::int32_t>(ref->GetFormID()));
```

```papyrus
Function MyFunc(int aiFormID)
    ObjectReference akRef = Game.GetFormEx(aiFormID) as ObjectReference
    if akRef == None
        Debug.Trace("...did not resolve")
        return
    endif
    ; use akRef
EndFunction
```

`Game.GetFormEx` rather than `Game.GetForm`: the SKSE version handles
the full 32-bit FormID range, which is required here because
dynamically-created references live at `0xFF......`.

## Why the None guard doesn't save you

This is the part that makes it expensive to diagnose. The obvious
defensive code does not work:

```papyrus
Function FillAttackerSlot(int aiIndex, ObjectReference akRef)
    ReferenceAlias slot = GetAttackerSlot(aiIndex)
    if slot == None || akRef == None      ; <-- passes!
        return
    endif
    slot.ForceRefTo(akRef)                ; <-- fails here
EndFunction
```

Papyrus's `== None` tests whether the variable holds a null *handle*. A
handle that exists but resolves to nothing is not `None` by that test,
so the guard falls through and the native fails one line later:

```text
Error: alias Attacker01 on quest _ne_AmbushQuest (FE0E7831):
       Cannot force the alias's reference to a None reference.
 [alias Attacker01 ...].ReferenceAlias.ForceRefTo() - "<native>" Line ?
 [_ne_AmbushQuest (FE0E7831)]._ne_AmbushQuest.FillAttackerSlot() - "_ne_AmbushQuest.psc" Line 31
```

Read that stack carefully — it is genuinely informative once you know
what you're looking at. The alias resolved (it is named), the script
resolved, the function ran, the property binding worked, and the guard
was cleared. Everything is correct except the one argument that came
from C++.

## What the plugin log shows instead

Nothing useful, which is the trap. On the C++ side
`QuestUtils::VMDispatchOnQuest` returns **true** — it is
fire-and-forget and reports only that the call was *queued*, never that
it succeeded. So the beat dispatches three fills, gets three
`true`s, then polls the aliases and finds them empty, and the only
symptom is a fill-verification timeout with no cause attached.

If alias fills are timing out and the plugin log is silent about why,
**go read Papyrus.0.log**. That is where the actual error is.

## Scope

The suspect path is `PackValue` → `PackHandle` in `RE/P/PackUnpack.h`,
which packs a form pointer using
`static_cast<VMTypeID>(decay_pointer_t<U>::FORMTYPE)`. Whether the
failure is the pack itself, or the reference not yet being resolvable at
dispatch time (it was created moments earlier in the same main-thread
hop), was not isolated — the FormID transport sidesteps both, and
resolving inside the VM happens strictly later than packing does.

Note that `LetterPool.cpp` passes a `TESObjectREFR*` to
`WICourierScript.removeRefFromContainer` the same way, on its OnLoad
sweep path. That path predates this finding and has not been observed
failing, but it is the same shape and worth suspecting if that sweep
ever misbehaves.

## Related

- `createreferenceatlocation-does-not-resolve-leveled-lists.md` — the
  other half of the same spawn path, and another case of a
  non-null-but-useless object.
- [`starting-a-quest-from-cpp.md`](starting-a-quest-from-cpp.md)
