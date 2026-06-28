# Hooking engine functions (use MinHook, not `trampoline.write_branch`)

## TL;DR

When you want to intercept a Skyrim engine function at its entry point —
e.g. trampoline-hook `TESDescription::GetDescription` so we can substitute
a book's body text — use **MinHook**, not SKSE's
`trampoline.write_branch<N>`.

```cpp
#include <MinHook.h>

using TargetFn_t = ReturnT (*)(Arg1, Arg2, ...);
TargetFn_t g_origFn = nullptr;

ReturnT HookedFn(Arg1 a1, Arg2 a2, ...)
{
    // Your interception logic.
    return g_origFn(a1, a2, ...);  // Fall through to original.
}

void Install()
{
    REL::Relocation<std::uintptr_t> target{ RE::Offset::Module::Function };
    const auto targetAddr = target.address();

    // MH_Initialize is idempotent under MH_ERROR_ALREADY_INITIALIZED, so
    // it composes with other plugins / our own future hooks.
    const auto initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
        logger::error("MH_Initialize failed (status={})", static_cast<int>(initStatus));
        return;
    }

    const auto createStatus = MH_CreateHook(
        reinterpret_cast<LPVOID>(targetAddr),
        reinterpret_cast<LPVOID>(&HookedFn),
        reinterpret_cast<LPVOID*>(&g_origFn));
    if (createStatus != MH_OK) { /* log + return */ }

    const auto enableStatus = MH_EnableHook(reinterpret_cast<LPVOID>(targetAddr));
    if (enableStatus != MH_OK) { /* log + return */ }
}
```

Verified working in `src/LetterSmokeTest.cpp` against
`TESDescription::GetDescription`. Same pattern SkyrimNet itself uses for
its AI-package hook — see `C:/Projects/chattelsys/src/PackageInjector.cpp`
and `C:/Projects/chattelsys/SkyrimNetAIPackageHooks.md`.

## Why not `SKSE::GetTrampoline().write_branch<5>(target, hook)`?

It looks like a function-entry detour but it isn't one. From the
CommonLibSSE-NG implementation (`SKSE/Trampoline.h` line 139):

```cpp
template <std::size_t N>
[[nodiscard]] std::uintptr_t write_branch(std::uintptr_t a_src,
                                           std::uintptr_t a_dst,
                                           std::uint8_t   a_data)
{
    const auto disp = reinterpret_cast<std::int32_t*>(a_src + N - 4);
    const auto nextOp = a_src + N;
    const auto func = nextOp + *disp;
    // ... writes 5-byte JMP at a_src ...
    return func;
}
```

It **reads the rel32 displacement at `a_src + N - 4`** to recover the
original destination of an *existing* 5-byte CALL or JMP instruction at
`a_src`, then writes a new 5-byte CALL/JMP, and returns the old
destination so the caller can chain.

In other words: `write_branch` is designed for **patching an existing
5-byte CALL/JMP site**, where the rel32 at `a_src + 1` is already a real
function offset. That's the right tool when you have a known call site you
want to redirect.

Pointed at a function's **prologue** (which contains arbitrary
instruction bytes, not a rel32), the displacement read produces garbage
and the returned "original function" pointer is junk. The hook write
succeeds, but storing the return value as your "original" pointer and
calling it later jumps into non-executable memory and crashes the game
with an access violation at exactly the garbage address.

This is the crash signature: **EXCEPTION_ACCESS_VIOLATION on
"Tried to execute memory at 0x<random>"**, where `0x<random>` is whatever
the prologue bytes happened to encode as a rel32 offset. If the address
in the crash matches the trampoline pointer you logged at install time,
you've fallen into this trap.

The 5-byte JMP itself works fine — the hook DOES fire — so partial
behavior is observable: substituting in the matched-FormID branch can
appear to work, and the bug only surfaces on the fall-through path that
calls the bogus "original."

## Why MinHook is the right tool

`MH_CreateHook` does what `write_branch` doesn't: it disassembles the
target function's prologue, copies enough complete instructions into an
allocated executable trampoline, appends an absolute JMP back to
`target + prologueLen`, and patches the function entry with a 5-byte
JMP to your hook. The pointer it gives back via the `LPVOID* original`
out-param is a real callable trampoline. Battle-tested, well-known, the
same library SkyrimNet uses.

## Build setup

```jsonc
// vcpkg.json
{
    "dependencies": [
        // ...existing deps...,
        "minhook"
    ]
}
```

```cmake
# CMakeLists.txt
find_package(minhook CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE minhook::minhook)
```

### Watch out for stale vcpkg baselines

The minhook port pulls a patch file from
`https://github.com/TsudaKageyu/minhook/commit/<sha>.patch` via
`vcpkg_download_distfile`, which hash-checks the download. GitHub
occasionally re-renders these patch files, which changes their bytes
and invalidates the port's recorded hash. A perfectly-valid baseline can
break overnight for this reason.

Symptom: `vcpkg install` fails with
`error: minhook-cmake-support.patch.<pid>.part: error: download from
https://... had an unexpected hash`.

Fix: bump `vcpkg-configuration.json`'s default-registry baseline to a
newer commit where the port has been updated. The known-working baseline
at the time of this writing is `f7805f1a46ba696714b74479ac358305cb087fd4`
(also what chattelsys uses).

The `commonlibsse-ng-fork` is pinned through a separate registry
(`Monitor221hz/modding-vcpkg-ports`) with its own baseline, so bumping
the default registry doesn't drag CommonLibSSE-NG along with it.

## When to use `write_branch` instead

`write_branch` (and its sibling `write_call`) are still the right tool if:

- You have a **specific call site** — a known 5-byte CALL/JMP instruction
  at a stable engine offset — that you want to redirect to your handler
  while keeping the rest of the function unhooked.
- You only want to intercept ONE caller of a function, not all callers.

For "intercept every call to function X anywhere it's invoked," reach
for MinHook.
