---
name: unit-test
description: Write a Catch2 unit test file for one C++ module in four gated stages — scaffold the harness, stub every branch, hoist the happy path, then implement each case. Takes the module as an argument (e.g. `/unit-test src/AliasWalkFilter.cpp`). Use when asked to add or write unit tests for an existing module.
---

# unit-test

Write a co-located Catch2 test file for the module named in the argument.

The work happens in **four stages with hard gates**. Each stage ends with a
build, and you do not begin the next stage until the current one's exit criteria
are met. Do not collapse stages, do not run ahead, and do not write assertions
before stage 4 — the staging exists because designing a suite's structure and
writing its assertions at the same time reliably produces deep, duplicated,
half-covering tests.

Read [`docs/DEVELOPMENT.md`](../../../docs/DEVELOPMENT.md) sections **Two test
executables**, **Mocking CommonLibSSE**, **Testing code that talks to the
engine**, and **Test style** before starting. Those are the conventions; this
file is the procedure that applies them.

## 0. Resolve the target and pick the test kind

The argument may be `Foo`, `src/Foo.cpp`, or `include/Foo.h`. Resolve it to the
pair `src/Foo.cpp` + `include/Foo.h`. If the module does not exist, stop and say
so rather than guessing at a near match.

Read the whole `.cpp` and the whole `.h` before deciding anything.

Then classify, by grepping the `.cpp` and its own headers for `RE::`, `SKSE::`,
`REL::`, `<RE/`, and `<SKSE/`:

| Finding | Test kind | File | Target |
| --- | --- | --- | --- |
| No engine references | **core** | `src/Foo.test.cpp` | `NarrativeEngineTests` |
| Any engine reference | **mocked-engine** | `src/Foo.engine.test.cpp` | `NarrativeEngineEngineMockTests` |

**Default to mocking. Do NOT refactor production code to remove an engine
dependency.** A module whose job is engine access is already the right shape,
and reshaping it to suit the harness is a change to shipping code made for the
test's convenience. Pushing a dependency out to the call boundary (the
`nowGameHours` parameter, the `ICosaveIO` port) is a legitimate design move, but
it is the user's call and not this skill's — if a module looks like it wants
one, propose it and wait for an answer. Otherwise mock.

State the classification and the file you are about to create, then proceed.

## 1. Scaffold — harness only, zero tests

Create the test file containing **only**:

- The include of the module under test, `<catch2/catch_test_macros.hpp>`, and
  whatever else the fixture needs.
- A file-header comment saying what the module is, why it is testable this way
  (pure, or mocked and how), and what is deliberately out of reach.
- An anonymous namespace holding `using` declarations, named constants, and any
  fake or helper the module obviously needs. For a mocked-engine test the
  `EngineMock` is constructed per `TEST_CASE`, not here.
- One `TEST_CASE` with the fixture declared and **no `SECTION`s**.

Wire the build:

- **Core**: add `"${CMAKE_SOURCE_DIR}/src/Foo.cpp"` to
  `NARRATIVEENGINE_CORE_SOURCES` in `CMakeLists.txt`. This *moves* the file out
  of the DLL's own source list into the shared static library — that is intended.
- **Mocked-engine**: add it to `NARRATIVEENGINE_MOCKED_SOURCES`. This does *not*
  remove it from the DLL; the file is compiled twice, once per link closure.

Then build:

```powershell
pwsh -File build.ps1 test
```

For a mocked-engine module the link will fail with `LNK2019` naming every engine
function the module calls that nothing stands in for. **That list is the work
item, not an error to route around.** Add a definition for each to
`testsupport/EngineMock.cpp`, backed by state on `EngineMock` in
`testsupport/EngineMock.h`, and repeat until it links. Rules for those mocks:

- Answer out of `EngineMock` state; never read through the pointer handed back.
  Engine "objects" are opaque storage with a valid address, not constructed
  instances.
- Give every singleton accessor a `present` flag so a test can make it return
  `nullptr`. The "singleton isn't up yet" path is usually among the most
  valuable branches in the module and is otherwise unreachable.
- Record interactions worth asserting on — a call count, or the arguments seen —
  when the module's correctness depends on *what* it asked the engine and not
  only on the answer it got.
- If a stub in `CommonLibSSERuntimeStubs.cpp` aborts at runtime, production code
  reached the address library. Add the missing mock; never weaken the stub.

**Exit criteria:** the file exists, both test executables build, `ctest` is
green, and the new `TEST_CASE` runs with zero assertions. Report the engine
functions you had to mock, then move to stage 2.

## 2. Branch inventory, then stubs

### 2a. Inventory

Re-read the `.cpp` and list **every** logical branch. Do not skip any:

- each arm of every `if` / `else if` / `else`, `?:`, and `switch` case,
  including the implicit fall-through or default,
- every early `return` and guard clause,
- every null, zero, empty-string, and empty-container check,
- every loop at zero, one, and many iterations, plus any `break` or `continue`
  that can fire mid-loop,
- every error, failure, or short-read path,
- every boundary comparison — pin down whether it is `<` or `<=`,
- for mocked-engine modules, every branch CommonLibSSE takes *inside inline
  header code* on the module's behalf, such as `REL::Module::IsVR()`.

Write the inventory out as a short table: branch → the case name that will cover
it. Every branch gets at least one case. Show the user this table.

### 2b. Structure the cases — breadth, not depth

This is the part that goes wrong when left to chance, so decide it deliberately
and before writing anything.

**Depth budget — four levels, and the fourth must be earned:**

1. `TEST_CASE` — one function, or one cohesive role of the class.
2. `SECTION("when <precondition>")` — a context.
3. `SECTION("and <further condition>")` *or* `SECTION("should <behaviour>")`.
4. `SECTION("should <behaviour>")` — only where level 3 was a genuine "and".

Anything that wants a fifth level is telling you it is a separate `TEST_CASE`.
Start one.

**Prefer breadth. Concretely:**

- Split into **more `TEST_CASE`s** before nesting deeper. One per public
  function is a good default; a module with six functions should not have one
  giant `TEST_CASE`.
- Two conditions that are *independent* are two sibling contexts, each setting
  its own state — not an outer context wrapping an inner one. Nest only when the
  inner condition is meaningless without the outer.
- Never write a context section whose only child is another context section.
  Collapse that chain into one section with a combined name. A context with a
  single *expectation* child is fine — that reads as "when X / should Y".
- If you are writing "and not X" as a sibling of "and X" three levels down,
  hoist both to level 2 as separate contexts.

**Leaf sections are single observable behaviours**, named `"should ..."`. Two
unrelated `REQUIRE`s in one leaf means two leaves. Several `REQUIRE`s that
together assert one fact — every field of a restored record, say — are one leaf.

### 2c. Write the stubs

Write every case, fully named and nested in its final shape, with an empty body
carrying a marker:

```cpp
SECTION("should drop the unresolvable sender")
{
    // TODO(stage 4): implement
}
```

No setup, no assertions, no `REQUIRE` anywhere yet.

**Exit criteria:** `pwsh -File build.ps1 test` is green, every branch from 2a
has a stub, and no section is deeper than the budget allows. Report the case
count and the maximum depth reached, then move to stage 3.

## 3. Hoist the happy path

Identify the **happy path**: the branch set that covers the most of the module —
normally "everything is available, the input is well-formed, the operation
succeeds". For a mocked-engine module it is usually "every singleton present,
the runtime is AE, and the state is unremarkable".

Move exactly that setup into the top-level `TEST_CASE` body, above the sections.
Catch2 re-runs everything above a `SECTION` for each leaf path beneath it, so
top-level setup behaves like a `beforeEach` and every case starts from the happy
path. Each case then overrides only its own delta, which is what keeps the
duplication out.

Rules:

- Hoist **only** what the majority of cases want. Setup that half the cases must
  undo is worse than no setup at all.
- Everything hoisted must be reconstructed per path — plain locals, destroyed at
  scope exit. No statics, no reset hooks, no `resetAllMocks` equivalent. RAII is
  the teardown.
- If two or three sibling contexts share setup that is *not* the happy path,
  give it a named helper — a lambda or small struct at `TEST_CASE` scope — and
  call it from each. Do not copy more than about two lines between siblings.
- Comment the hoisted block to say it is the happy path and that it re-runs per
  leaf.

**Exit criteria:** build green, stubs unchanged, top-level setup in place, and
you can name for each case what it will override. Move to stage 4.

## 4. Implement the cases

Work through the stubs **in file order, one at a time**. For each:

1. Add whatever local setup it needs that the top-level setup does not already
   provide, placed in the *shallowest* section all its descendants share, so
   siblings do not each repeat it.
2. Execute the code under test. Where several leaves share one call, put the
   call in their parent section and assert on the result in the leaves.
3. Write the assertions.
4. Delete the `TODO(stage 4)` marker.

Build after every few cases rather than only at the end, so a compile error
belongs to a small diff.

**Hard rules while implementing:**

- **A test must fail if the behaviour it names is removed.** The trap is a case
  that passes on falsy defaults: asserting `IsGamePaused()` is false while the
  mock's `gameIsPaused` is already false proves nothing, because deleting the
  null guard would keep it passing. Set the underlying state to the *opposite*
  of the expected answer, so the behaviour under test is the only thing that can
  produce it.
- **Assert observable behaviour, not implementation.** Anonymous-namespace
  helpers and private members are out of bounds; drive everything through the
  public surface. The branch inventory is how you *find* cases, not how you name
  or assert them.
- **Never change production code to make a test pass.** If a case cannot be
  written without one, stop and report it — the branch, why it is unreachable,
  and what the change would be. The user decides.
- If you find a real bug, say so and leave it. Do not fix it as a side effect,
  and do not write a test that enshrines it.
- Comment the non-obvious cases with *why the case matters* — the failure it
  catches, the bug it came from, the boundary it pins. Cases that are
  self-evident from their names need no commentary.

**Exit criteria:** no `TODO(stage 4)` markers remain in the file, and the suite
is green.

## 5. Verify and report

1. `pwsh -File format.ps1`. New files are untracked and the hooks only see
   tracked files, so run `git add -N <new files>` first — otherwise the run
   skips them and says so in its output.
2. `pwsh -File build.ps1 test` — every case green.
3. `pwsh -File build.ps1 build` — the SKSE plugin still builds. Mandatory
   whenever stage 1 edited `CMakeLists.txt` or any production file was touched.
4. **Mutation-check.** Pick the two or three assertions carrying the most
   weight, break the production behaviour each one names, and confirm the suite
   goes red naming that case. Then restore and re-run to green.

   Restoring has two traps. `git checkout --` restores the *committed* version,
   which discards any uncommitted work on that file — only safe when the file
   has no pending changes. Copying a backup over the original is safe but resets
   the mtime, so ninja sees a stale `.obj` as current, skips the rebuild, and the
   next run reports the mutated binary's result. After any restore, run
   `touch <file>` before rebuilding.

Report, briefly: the module and why it got the test kind it did; any engine
functions added to `EngineMock`; the case count, assertion count, and maximum
section depth; which branches you could not cover and why; the mutation checks
and their outcomes; and anything that looked like a bug but was left alone.
