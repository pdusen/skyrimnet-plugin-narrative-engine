# NarrativeEngine — Instructions for Claude

For general project information, conventions, build instructions, and the ESP/Papyrus workflow,
read [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) — everything that applies to any contributor
lives there. This file contains only the discipline that's specific to how Claude should approach
this project.

## Working method: phased implementation

NarrativeEngine is built in discrete phases, each captured in a planning doc under
`docs/implementation/PHASE_NN_*.md`. Each phase doc has numbered steps with explicit goals, files
to touch, and verification criteria; the doc records which steps are complete via per-step
checkboxes. Check the current phase doc before starting any new work to see what's in scope.

**Don't run ahead of the plan.** Implement the step that's currently being worked, not the ones
after it. Don't add infrastructure, abstractions, or features in anticipation of later phases —
the user iterates on the design phase by phase, and pre-built scaffolding has consistently been
wrong about what the next phase actually needs.

When a phase doc is itself under revision (the user is reshaping a step before committing to it),
treat the doc as the contract: edits to the doc come first, implementation follows the doc, and
the implementation should not silently include scope the doc doesn't reflect.

## LLM-returned strings: always sanitize

Every free-form string we accept back from an LLM MUST pass through
`NarrativeEngine::LLMTextSanitizer::Sanitize(...)` at the point of
extraction from the response JSON, before being stored, persisted to
co-save, written to any `RE::TESForm` field, displayed in UI, fed into
another LLM call, or used in any other downstream way.

The reason: LLMs routinely return smart quotes, em-dashes, ellipses,
NBSPs, accented Latin letters, and zero-width formatting characters
that look fine in chat but break visible-text use in Skyrim (missing
glyphs, silent truncation in ASCII-only engine fields, garbled co-save
payloads). The sanitizer is a single canonical pass that maps these to
ASCII equivalents or drops them, and trims surrounding whitespace.

When adding a new LLM-driven feature, wrap every `get<std::string>()`
call for a free-form field in `LLMTextSanitizer::Sanitize(...)` — do
not defer to a later normalization step. See
[`docs/LLM_RESPONSE_HANDLING.md`](docs/LLM_RESPONSE_HANDLING.md) for the
substitution table, the library-vs-manual rationale, and worked
examples of how to identify which response fields need sanitizing.

## Threading discipline: every plugin function takes a token

NarrativeEngine has a strict three-role thread model (Main / Plugin /
Foreign) with two unforgeable token types (`MainThread::Token`,
`PluginThread::Token`) that gate every plugin function. Every function
you add takes either a `MainThread::Token const&` (for engine-touching
wrappers) or a `PluginThread::Token const&` (for everything else).

Worker code that needs the main thread uses `MainThread::Run(pt, fn)`
to block for a result, or `MainThread::FireAndForget(pt, fn)` to
schedule fire-and-forget. Foreign-thread code (SkyrimNet callbacks,
engine event sinks) enters plugin-thread land via
`AsyncDispatch::EnqueueWork` — the sole plugin API that requires no
token from the caller.

The type system enforces the discipline: forbidden call patterns fail
to compile. See [`docs/THREADING_MODEL.md`](docs/THREADING_MODEL.md)
for the full rules, the wrapper pattern, the enforcement mechanisms,
and the "what NOT to do" cheat sheet.

## Always run `format.ps1` after adding or modifying files

After any batch of edits — code, docs, config, whatever — run `pwsh -File format.ps1` at the repo root
and resolve every finding before considering the task complete. The `.pre-commit-config.yaml` hooks
handle C++ (clang-format), Markdown (markdownlint --fix), CMake (gersemi), YAML/JSON (prettier),
PowerShell (Invoke-Formatter + PSScriptAnalyzer), and whitespace/EOL hygiene. Most fixes are applied
in place; if the run fails, re-read the output and fix the remaining findings yourself — do not hand
that work to the user.

Papyrus (`.psc`) is deliberately not autoformatted (no maintained formatter exists), so the hooks skip
it — Papyrus errors surface at CK compile time via `build.ps1 build` instead.

## What NOT to assume

- That a pattern IntelEngine uses is automatically appropriate for NarrativeEngine. The right
  way to consult IntelEngine is described below under "IntelEngine prior art — discipline."
- That a feature, action, subsystem, or piece of infrastructure beyond the current phase's scope
  is wanted yet. New surface area is deliberately staged phase by phase; speculative
  implementation has consistently been wrong about what the user actually wants next.
- That an unfamiliar file, directory, branch, or piece of state in the repo represents stale
  cruft to clean up. It might be the user's in-progress work — investigate (`git log`, ask)
  before deleting or overwriting.

## IntelEngine prior art — discipline

The factual reference (what IntelEngine is, where the repos and prior-art docs live) is in
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md#prior-art-reference-intelengine). The discipline below
governs how to USE that material.

### IntelEngine's role in this project

IntelEngine is **one solved instance** of a similar-shape project — but it solved its own problems, not ours. Treat
the prior-art docs as a *reference for specific problems you've decided to solve*, not as a list of features to mirror
or an architecture to echo.

The right workflow when a design question comes up:

1. **Start from NarrativeEngine's actual needs.** What does our current scope (the feature in front of you, the phase
   you're in, the action being authored) genuinely require? Decide from first principles.
2. **Then check prior art, only if relevant.** If you've decided you need to solve problem X, the IntelEngine docs may
   show one working solution. If they do, adopt it where it fits, adapt where it almost fits, or ignore where our
   problem is different.
3. **Never let IntelEngine's structure dictate ours by default.** If you find yourself adding a form, a subsystem, a
   pattern, or a phase task "because IntelEngine has one," stop and re-justify it on NarrativeEngine's own terms.
   Anything that can't be justified that way doesn't belong here, even if IntelEngine had it.

Examples of the anti-pattern to avoid:

- "IntelEngine has speed-variant Travel packages, so we need them." → No. Phase 1 has no `ChangeSpeed` action; one
  walk-pace package is enough.
- "IntelEngine has 5 agent alias slots, so we need 5." → No. Pick the minimum the actual cooldown / dispatch math
  supports.
- "IntelEngine declares MCM globals for every setting, so we should too." → No. Our INI settings already cover the
  same surface; CK globals would be duplication.
- "IntelEngine has a faction-politics sim, so a sim is the right pattern for background simulation." → Only if our
  design needs that *shape*. Otherwise design from scratch.

The IntelEngine docs are useful precisely *because* they're a frozen snapshot of one author's choices — but those
choices answered IntelEngine's questions, not ours.

### Even when IntelEngine fits, ask if there's a better design

"Fits the problem" is not the same as "is the best solution." Even when an IntelEngine pattern matches a problem
NarrativeEngine actually has, ask whether a better design exists along these axes before adopting:

- **Simplicity** — is IntelEngine's solution more complicated than NarrativeEngine needs? IntelEngine's scope was
  much broader (faction politics, story DM, battle manager, in-game React dashboard, …) and its solutions were
  sized to that scope. What was a reasonable cost there may be over-engineering here.
- **Efficiency** — is IntelEngine doing more work than NarrativeEngine needs? Extra indexing passes, polling loops,
  state to track, LLM calls per cycle, save/load payload, and so on. Strip anything our actual feature surface
  doesn't justify.
- **Encapsulation** — is IntelEngine crossing the C++ / Papyrus boundary more than necessary? Boundary crossings
  (native bridge calls, ModEvents, StorageUtil reads/writes) are expensive to write, expensive to debug, and a
  recurring source of subtle bugs. Keep work on one side of the boundary unless there's a clear reason to cross.
  IntelEngine's ~5000-line `Papyrus.cpp` exposing ~200 natives is the cautionary example.
- **Maintainability** — is IntelEngine's solution organized in a way that's hard to understand or change? Sprawling
  single files, tightly-coupled subsystems, dense property surfaces on Papyrus scripts, and reliance on
  out-of-band conventions all add cost. A simpler, smaller, more localized design is almost always better when the
  feature surface allows for it.

Adopting wholesale without this check is the same anti-pattern as "we need it because IntelEngine has it" — just
in a different shape. The first time you find yourself thinking "IntelEngine does it this way, so I'll do the
same," stop and ask: can I do it better? If yes, do that instead and write down why.

### Treat IntelEngine's code with skepticism, not deference

IntelEngine shipped — but it was active development that was abandoned mid-cycle, not a polished or verified
reference. The original author would not claim it was bug-free. The code is full of subtle quirks, edge cases that
were never fully resolved, magic numbers chosen by feel, and patterns that worked well enough to ship without
being verifiably correct. We've already surfaced examples of this in the prior-art docs (fabricated CK field
names, non-existent AI Package "Priority" field, "Initially Cleared" alias flag that doesn't exist).

When adopting from IntelEngine, read it as **"here's how someone got far enough to ship,"** not **"here's the
verified-correct way."** Specific values, field names, condition forms, threshold tunings, and edge-case
handling should all be cross-checked against authoritative sources before being trusted:

- CK Wiki / Bethesda's Creation Kit documentation for any CK form details.
- CommonLibSSE-NG source / headers for any C++ engine API claim.
- Papyrus language reference for any script-language claim.
- The actual game (in-game testing, console commands, xEdit inspection of the live `IntelEngine.esp`) for any
  runtime-behavior claim.
- SkyrimNet's `PublicAPI.h` for any SkyrimNet integration claim.

If IntelEngine does something a certain way and the authoritative source contradicts it, the authoritative source
wins. If IntelEngine has unexplained magic in its code, treat that as a smell, not a precedent.
