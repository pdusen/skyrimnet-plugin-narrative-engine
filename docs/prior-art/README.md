# IntelEngine — Prior Art Reference

This directory contains everything Claude learned about **IntelEngine**, an open-sourced SkyrimNet plugin that
NarrativeEngine treats as prior art.

The source code lives outside this repo at:

- `C:\Projects\IntelEngine-NativePlugin\` — SKSE source tree (C++ + Papyrus)
- `C:\Projects\IntelEngine-GamePlugin\` — deployment / `Data/` payload (compiled artifacts + game-data assets)

These are **read-only** for NarrativeEngine work.

## How to use this folder

This is a **lookup library**, not a checklist. Treat each file as "if I've decided I need to solve problem X, here
is one author's working solution to a similar problem." Open the file *after* a NarrativeEngine design question is
on the table — never to browse for things to add to NarrativeEngine.

### The discipline

1. Decide what NarrativeEngine actually needs from first principles. (Phase scope, action requirements, design
   goals.)
2. *If* you've decided you need to solve a specific problem (a Papyrus-side dispatch, a way to track NPC busyness,
   an async LLM call shape, …), consult the relevant file below for one working approach.
3. **Even when IntelEngine's solution fits, ask whether a better one exists** before adopting. The
   [discipline in CLAUDE.md](../../CLAUDE.md#even-when-intelengine-fits-ask-if-theres-a-better-design) lists the
   axes — simplicity (is it more complicated than we need?), efficiency (is it doing more work than we need?),
   encapsulation (is it crossing the C++/Papyrus boundary more than we need?), maintainability (is it organized
   well for our smaller scope?). IntelEngine sized its solutions to a much broader scope; what was a reasonable
   cost there may be over-engineering here.
4. Adopt the approach if it's the best. Adapt if a simpler/better variant is obvious. Ignore if our problem isn't
   the same shape.

If you find yourself adding something to NarrativeEngine *because IntelEngine had one*, that's the bug — go back to
step 1 and re-justify it. If you find yourself mirroring IntelEngine's design wholesale, that's a separate bug —
go to step 3 and ask if you can do better.

### Treat IntelEngine's code with skepticism

IntelEngine was abandoned mid-development; it shipped working but it wasn't a polished or verified reference.
Specific field names, condition forms, magic numbers, edge-case handling — none of it should be trusted by
default. The CK and C++ details preserved in these docs are particularly suspect: we've already surfaced
fabricated CK field names ("Initially Cleared" alias flag) and a non-existent concept (AI Package "Priority")
that came from IntelEngine's own AI-generated design notes. When IntelEngine and an authoritative source (CK
Wiki, CommonLibSSE-NG headers, Papyrus reference, in-game observation) disagree, trust the authoritative source.
See the full treatment in
[CLAUDE.md](../../CLAUDE.md#treat-intelengines-code-with-skepticism-not-deference).

The two factual descriptions of IntelEngine (`SKYRIMNET_PLUGIN_CONTRACT.md` is partly an exception — see below) live
in `REPO_MAP.md`, `FEATURE_OVERVIEW.md`, `ARCHITECTURE.md`, `ESP_STRUCTURE.md`, and `DEPLOYMENT_LAYOUT.md`. They tell
you *what IntelEngine did* and *where to find it in the source*. They make no claim about what NarrativeEngine
should do.

The patterns / lessons doc (`PATTERNS_AND_LESSONS.md`) is the most-suggesting of the set; treat the patterns there
as "options to consider if the problem matches," not as principles to apply by default.

### Special case: `SKYRIMNET_PLUGIN_CONTRACT.md`

This is the one prior-art file whose content is closer to "things NarrativeEngine has to know about." The SkyrimNet
plugin extension surface (action YAMLs, manifest schema, prompt templates, submodule conventions) is a contract
imposed by SkyrimNet itself, not by IntelEngine — IntelEngine's example just happens to be the most complete one
we have. When the SkyrimNet integration is at stake, this file is closer to "this is what the contract looks like"
than to "this is what one author did with the contract."

## Index

[`REPO_MAP.md`](REPO_MAP.md) — Layout of the two IntelEngine repos and where to find any subsystem's source.

[`FEATURE_OVERVIEW.md`](FEATURE_OVERVIEW.md) — What IntelEngine *does* at the gameplay level (its choices, not
ours).

[`ARCHITECTURE.md`](ARCHITECTURE.md) — Preserved architecture analysis: layers, every SKSE C++ module, every
Papyrus script, data flow, threading, persistence, dependency graph — all describing **IntelEngine** as one
solved instance.

[`SKYRIMNET_PLUGIN_CONTRACT.md`](SKYRIMNET_PLUGIN_CONTRACT.md) — The SkyrimNet plugin extension surface —
closer to a contract than to one author's pattern; relevant whenever we're hooking SkyrimNet.

[`ESP_STRUCTURE.md`](ESP_STRUCTURE.md) — The CK setup IntelEngine's `.esp` provides (quest, alias slot
pattern, AI packages with speed variants, keywords, TaskFaction). Reference for "how might this be
structured?" — not a list of forms NarrativeEngine should create.

[`DEPLOYMENT_LAYOUT.md`](DEPLOYMENT_LAYOUT.md) — What ships in the IntelEngine `Data/` folder. Use as a model
for what *shape* of payload a SkyrimNet plugin tends to have.

[`PATTERNS_AND_LESSONS.md`](PATTERNS_AND_LESSONS.md) — Patterns that worked in IntelEngine's context
(three-phase async, fuzzy cascade, escalating recovery, soft dependency loading, dispatch ring buffer,
save-scum recovery, single source of truth). Read as candidates to consider, not defaults.
