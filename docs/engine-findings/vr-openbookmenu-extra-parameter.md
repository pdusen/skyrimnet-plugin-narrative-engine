# `BookMenu::OpenBookMenu` takes a 9th argument on Skyrim VR

## TL;DR

CommonLibSSE-NG declares `BookMenu::OpenBookMenu` with eight parameters:

```cpp
static void OpenBookMenu(const BSString& a_description, const ExtraDataList* a_extraList, TESObjectREFR* a_ref,
                         TESObjectBOOK* a_book, const NiPoint3& a_pos, const NiMatrix3& a_rot, float a_scale,
                         bool a_useDefaultPos);
```

That signature is correct for SE and AE. **On Skyrim VR the same function (address-library ID 50122) takes a ninth
parameter: `NiAVObject*`** — the book's already-loaded 3D node. VR hands the node in; SE/AE resolve it internally.
The VR function stores it into `BookMenu::RUNTIME_DATA::bookModel` (a `NiPointer<NiAVObject>`), which begins with an
`NiRefObject::IncRefCount` — the instruction `lock inc [rbx+0x08]` at `50122+0x19E`.

A detour written against the eight-parameter signature therefore *cannot* pass a book open through unharmed on VR.
Forwarding "every argument verbatim" still drops argument nine, so the original function ref-counts whatever the
detour's own stack frame left at `[rsp+0x48]` — a wild pointer, usually into some module's read-only data. Every book
open crashes, ours or not.

The fix is one detour per runtime, selected at install time:

```cpp
REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(50122, 51053) };
if (REL::Module::IsVR()) {
    InstallHook("BookMenu::OpenBookMenu (VR)", target.address(), &HookedOpenBookMenuVR, &g_origOpenBookMenuVR);
} else {
    InstallHook("BookMenu::OpenBookMenu", target.address(), &HookedOpenBookMenu, &g_origOpenBookMenu);
}
```

See `src/LetterPool.cpp` for the live version. Note this is a *hooking* problem specifically: calling
`RE::BookMenu::OpenBookMenu(...)` ourselves on VR would be wrong for the same reason, but we never do.

## What it looks like when you get it wrong

Two VR testers, unrelated load orders, identical crash — reproducible on activating any book in the world, and on
closing a book opened from the inventory:

```text
Unhandled exception "EXCEPTION_ACCESS_VIOLATION" at SkyrimVR.exe+088271E -> 50122+0x19E   lock inc [rbx+0x08]
Access Violation: Tried to write memory at 0x7FF72F156180
    Base Register: rbx = 0x00007FF72F156178

CALL STACK:
    [ 0][P] 0x7FF72E28271E   SkyrimVR.exe+088271E -> 50122+0x19E   lock inc [rbx+0x08]
    [ 1][P] 0x7FFFC06293E9   NarrativeEngine.dll+00D93E9
```

Tells that it is an argument problem and not a bad hook target:

- The fault is at `+0x19E`, deep inside the function — the prologue and the MinHook trampoline both ran fine.
- It reproduces from two unrelated call paths (`TESObjectREFR::Activate` in one log, the inventory book-menu path in
  the other), so it is the callee that is unhappy, not one caller.
- It reproduces for books that are **not** ours (`DA16TorporBook`, `CLWCamillaNote`), i.e. on the plain
  pass-through branch where the detour changes nothing.
- `rbx` points into some module's `.rdata` — in one log to another SKSE plugin's `__FILE__` literal
  (`"C:\Modding\SKSEDevelopment\Recipes\src\hooks.h"`). Read-only-data pointers with no relationship to the call are
  the signature of a stack slot the caller never wrote.

## Reading the argument off the stack dump

This is worth writing down because it is how the ninth argument was identified without a VR binary to disassemble.

Find the return address that leads back into the detour in the `STACK:` dump. That slot *is* the callee's entry
`rsp`, so arguments five through N are at `+0x28`, `+0x30`, `+0x38`, … above it. From the second log:

```text
[RSP+58 ] 0x7FFFC06293E9      NarrativeEngine.dll+00D93E9   <- return address; entry rsp
[RSP+80 ] 0xFA86CFF668        arg5  const NiPoint3&
[RSP+88 ] 0xFA86CFF690        arg6  const NiMatrix3&
[RSP+90 ] 0xFFFFFFFF3F800000  arg7  float a_scale (low 32 bits = 1.0f)
[RSP+98 ] 0x1                 arg8  bool a_useDefaultPos
[RSP+A0 ] 0x7FF72F156178      arg9  ==  rbx at the fault      <- never written by us
```

Then repeat one frame up, using the return address into the *engine* caller as that frame's entry `rsp`, to see what
the engine actually passed:

```text
[RSP+138] 0x7FF72DC3B0F2      SkyrimVR.exe+023B0F2          <- return address; entry rsp
[RSP+160] 0xFA86CFF668        arg5  (same NiPoint3)
[RSP+168] 0xFA86CFF690        arg6  (same NiMatrix3)
[RSP+170] 0x2583F800000       arg7  (low 32 bits = 1.0f)
[RSP+178] 0x259A59FE901       arg8  (low byte = 1)
[RSP+180] 0x25E50259A80       arg9  (BSFadeNode*) "book02lowpoly02.nif"
```

Arguments one through eight line up exactly with the SE/AE signature, and argument nine is a live `BSFadeNode` for the
book model being opened. Both crash logs agree, at the same frame offsets, with different books and different call
paths.

## False leads

- **"The address-library ID is wrong for VR."** It isn't. CrashLoggerSSE's own symbol name for `SkyrimVR.exe+88271E`
  is `BookMenu::OpenBookMenu`, and `50122` resolves to that function's base on VR.
- **"MinHook mis-copied the prologue."** It didn't. A broken trampoline faults at or near the function entry, not
  0x19E bytes in with a correct frame and correct arguments one through eight.
- **"Another SKSE plugin is hooking the same function."** Neither log's load order shares a book-related plugin, and
  the crash reproduces on the branch where the detour is a pure pass-through.

## Related

- [`hooking-engine-functions-with-minhook.md`](hooking-engine-functions-with-minhook.md) — why we detour with
  MinHook rather than `trampoline.write_branch`.
- [`scripteventsourceholder-addeventsink-crashes-on-vr.md`](scripteventsourceholder-addeventsink-crashes-on-vr.md) —
  the other place a CommonLibSSE-NG declaration silently doesn't hold on VR.
