# `ScriptEventSourceHolder::AddEventSink<TESFastTravelEndEvent>` crashes on Skyrim VR

## TL;DR

`RE::ScriptEventSourceHolder` has one fewer base class on Skyrim VR than on SE/AE: it has **no
`BSTEventSource<TESFastTravelEndEvent>`**. CommonLibSSE-NG's accessor for that source returns `nullptr` on VR,
and the templated `holder->AddEventSink<T>(sink)` dereferences the accessor's result without checking. So on VR:

```cpp
holder->AddEventSink<RE::TESFastTravelEndEvent>(sink);   // instant access violation on VR
```

Route every fast-travel sink registration through `EngineUtils::AddFastTravelEndSink` /
`RemoveFastTravelEndSink` instead. They return `false` on VR (and when the holder singleton isn't up yet) so the
caller can record that the sink isn't live.

This is *only* a problem for `TESFastTravelEndEvent`. Every other event type in the holder exists on all three
runtimes, and `AddEventSink<T>` for those is a plain `static_cast<BSTEventSource<T>*>(this)` that can't be null.

## What it looks like when you get it wrong

A VR player crashes on the main menu — i.e. during `kDataLoaded`, before any save is loaded. CrashLoggerSSE
reports:

```text
Unhandled exception "EXCEPTION_ACCESS_VIOLATION" at NarrativeEngine.dll+0123658   mov r8d, [rsi]
Access Violation: Tried to read memory at 0x000000000048
    Base Register: rsi = 0x0000000000000048 (likely invalid)
```

The `0x48` is the giveaway: it's the offset of `BSTEventSource::sinks` (the array's size/data header) inside the
event source, read off a null `this`. Any "tried to read a small constant address" fault right after an
`AddEventSink` call is this bug.

The stack has the sink type and the registering function on it, which is how we located it:

```text
POSSIBLE RELEVANT OBJECTS:
    RDI: (NarrativeEngine::FineRoads::`anonymous namespace'::FastTravelEndSink*)
STACK:
    [RSP+20 ] (char*) "void __cdecl NarrativeEngine::FineRoads::Initialize(void)"
```

## Why the layout differs

`ScriptEventSourceHolder`'s base-class list in `RE/S/ScriptEventSourceHolder.h` ends with:

```cpp
#if !defined(ENABLE_SKYRIM_VR)
    public BSTEventSource<TESSwitchRaceCompleteEvent>,  // 11E0
    public BSTEventSource<TESFastTravelEndEvent>        // 1238
#else
    public BSTEventSource<TESSwitchRaceCompleteEvent>   // 11E0
#endif
```

`sizeof(ScriptEventSourceHolder)` is `0x1290` on SE/AE and `0x1238` on VR. VR has no fast travel in the first
place (movement is teleport/locomotion driven and the map menu doesn't offer it), so Bethesda never wired the
event up — there is nothing to register for, not just a moved offset.

Hence CommonLibSSE-NG's:

```cpp
inline BSTEventSource<TESFastTravelEndEvent>* AsTESFastTravelEndEventSource() noexcept
{
    if SKYRIM_REL_CONSTEXPR (REL::Module::IsVR()) {
        return nullptr;
    } else {
        return &REL::RelocateMember<BSTEventSource<TESFastTravelEndEvent>>(this, 0x1238, 0);
    }
}
```

The accessor is runtime-correct; the `AddEventSink<T>` / `RemoveEventSink<T>` / `SendEvent<T>` wrappers around it
are not null-safe. That asymmetry is the whole trap.

## The false lead

The obvious first read of the crash is "the singleton wasn't ready at `kDataLoaded`" — both call sites already
null-checked `ScriptEventSourceHolder::GetSingleton()` and logged an error on failure, so the null check *looked*
covered. It isn't the singleton that's null; the holder is perfectly valid. It's the per-event-type source inside
it that doesn't exist, and no amount of checking the holder catches that.

## Losing the event on VR

Both subsystems that wanted this event degrade cleanly without it:

- `FineRoads` used it only as one of several triggers to flag its cell grid for rescan. `TESCellFullyLoadedEvent`
  fires for the cells a fast travel lands you in anyway, and there's a periodic backstop rescan besides.
- `TravelEventLog` used it to tag a location change as a fast travel. Its poll-driven location diff still
  observes the move; it just can't label it. On a runtime with no fast travel, that label was never going to fire.

## Applies to

Any CommonLibSSE-NG accessor guarded by `REL::Module::IsVR()` that returns `nullptr`. Grepping the NG headers for
that pattern, the current set is `BSInputEventQueue`, `ButtonEvent`, `HUDMenu`, `MainMenu`, `MapMenu`,
`PlayerCharacter` (a handful of members), `ScriptEventSourceHolder`, `VRWandEvent`, and `WorldSpaceMenu`. Of
those, `ScriptEventSourceHolder` is the one whose null result reaches a dereference through a *convenience
template* rather than at the call site, which is why it's the one that bit us.
