---
name: release
description: Publish a new GitHub release of NarrativeEngine. Verifies the working tree is clean, proposes a version and release notes based on commits since the last tag, packages the mod with package.ps1, tags and pushes, then creates a `gh` release with the zip attached and prints the URL.
---

# release

Cut a new NarrativeEngine GitHub release. Work through the steps below in order.
Pause for user input at each explicit approval gate — do NOT shortcut around
them. The user is the final authority on version and release-notes wording.

## 0. Verify GitHub CLI auth

Before touching anything else, run `gh auth status`. If it reports the user is
not logged in, or that the token lacks the required scopes (`repo` is enough
for creating releases), stop immediately and ask the user to run
`gh auth login` before re-invoking the skill.

Failing here costs nothing. Failing at step 9 after the tag is already pushed
leaves the repo in an awkward split state that requires manual cleanup, so
this check runs first on purpose.

## 1. Verify a clean working tree

Run `git status --porcelain`. If the output is non-empty, abort with a summary
of the dirty files and ask the user to commit or stash before continuing. Do
NOT stage or commit on their behalf.

Verify the current branch is `main` with `git branch --show-current`. If it
isn't, ask the user to confirm they intend to release from a non-main branch
before proceeding.

## 2. Read changes since the last release tag

Find the most recent release tag:

```powershell
git tag --list 'v*' --sort=-v:refname | Select-Object -First 1
```

- If a tag exists, the change window is `<tag>..HEAD`.
- If no tag exists (first release), the range is the entire history.

Read the changes at two levels — this is a hard requirement, not a suggestion,
because commit subjects alone routinely miss detail that has to appear in
release notes (save-compatibility warnings, renamed editor IDs / form IDs,
upgrade steps, changed prompt/manifest surface, etc.).

**Level 1 — full commit messages, not just subjects.** Read every commit's
entire message including its body:

```powershell
git log --no-merges --format='commit %h%n%s%n%n%b%n---' <range>
```

Authors put `IMPORTANT:` warnings, save-compat notes, and "users must do X
before updating" caveats in the body. `--pretty=format:'%h %s'` (subject-only)
hides all of that, and any of it that exists MUST appear in the draft in
step 4 — as a callout at the end of the feature subsection it belongs to,
or in the trailing "Notes" section when it belongs to no single feature.

**Level 2 — the actual diff, not just `--stat`.** Read `--stat` first for a
map of what changed, then inspect the actual diff content for player-facing
areas the commit messages didn't fully cover:

```powershell
git diff --stat <range>
git diff <range> -- <interesting paths>
```

Paths that are almost always worth inspecting when they appear in `--stat`:

- `esp/plugin/**` — renamed / renumbered forms, added quests, editor-ID
  changes. These are the diffs that surface save-compat concerns.
- `statics/SKSE/Plugins/SkyrimNet/prompts/**` — prompt changes affect LLM
  behavior; often not fully captured in the commit subject.
- `statics/MCM/**` and `statics/SKSE/Plugins/NarrativeEngine.ini` — user
  tunables added / removed / renamed.
- `statics/SKSE/Plugins/SkyrimNet/config/plugins/NarrativeEngine/manifest.yaml`
  — action / hook surface exposed to SkyrimNet.
- `dashboard/**` — dashboard tabs / controls the player interacts with.

Skim commit subjects and bodies for user-visible changes. Ignore purely
mechanical churn (formatter runs, doc-only edits with no player-facing effect)
when it isn't relevant to a user's install decision. But do NOT ignore
anything a commit body flags as important, and do NOT rely on commit messages
alone — cross-check against the actual diff.

## 3. Propose a version number

While the mod is in early alpha (see the README warning), the scheme is
`0.MINOR.PATCH`:

- Bump PATCH for bug fixes and small internal changes with no new player-facing
  surface.
- Bump MINOR for any new player-facing feature, subsystem, beat, dashboard tab,
  or noticeable behavior change.
- The first release starts at `0.1.0`.
- Once the mod exits alpha (README warning removed and `1.0.0` cut), switch to
  standard semver — bump MAJOR on breaking changes.

State the current latest tag (or "no prior release"), the proposed new version,
and a one-sentence justification in normal chat text.

Then present the approval gate using the `AskUserQuestion` tool — NOT a
plain-text prompt. Structured selection is the correct UX for this decision,
and "Other" is auto-added by the harness so the user can still supply a custom
version. Use a single-select question shaped like this:

- **question:** `"Proposed version: vX.Y.Z. Accept, or bump differently?"`
- **header:** `"Version"`
- **options** (single-select, in this order):
  1. `"Accept vX.Y.Z (Recommended)"` — description: what the accepted bump
     means (e.g. "PATCH bump; no player-facing changes since vA.B.C").
  2. The next-larger bump on the same track — e.g. if you proposed PATCH,
     offer the corresponding MINOR (`v0.<MINOR+1>.0`); if you proposed MINOR,
     offer the next-higher MINOR. Description: one line on why the user might
     prefer it (e.g. "if you consider these changes big enough to warrant a
     MINOR").

Do NOT list "Other" yourself — the harness adds it. If the user picks Other and
types their own version string, accept it verbatim (per the "don't second-guess
user-provided identifiers" rule); do not silently normalize it.

Wait for their answer. Store the accepted version as `$Version` (bare, no `v`
prefix) — `package.ps1` and `gh release create` both want the bare form; the
`v` prefix is added only at the git-tag and archive-filename layers.

## 4. Draft release notes and iterate to approval

Draft in this shape:

```markdown
## Summary

<one or two sentences describing what this release is about>

## What's New

### <Feature domain>

- <bullet per user-facing change within this domain>

> **Note:** <callout tied to THIS feature — a requirement, a caveat, an
> expectation to set, an upgrade step it forces. Goes at the END of the
> feature's subsection, after its bullets.>

### <Next feature domain>

- <bullets>

## Fixes

- <one bullet per individual user-visible bug fix>

## For Developers

- <bullet per change aimed at someone working on the mod rather than
  playing it: internal subsystems, debug and diagnostic tooling, testing
  aids that ship off by default, fixes to any of those, build and
  packaging configuration, engine-level scaffolding.>

## Notes

<optional — release-wide caveats only: known issues, upgrade steps, or
requirements that belong to no single feature. Anything tied to one
feature belongs in that feature's subsection instead.>
```

Section order is fixed and MUST NOT be rearranged: Summary, What's New,
Fixes, For Developers, Notes. The ordering principle is "everything the
player notices comes before anything they don't" — so "For Developers" is
always the LAST content section, below "Fixes". Never place it above
fixes; a reader scanning the mod page for what changed for them has to get
through the plumbing to reach the bug fixes, which is exactly backwards.

Drop any section that would have zero bullets — don't ship an empty
"Fixes" or "For Developers" header just to keep the template.

"For Developers" is deliberately broad. It is not a "debug tools" section
— it is everything in the release whose audience is someone working on the
mod rather than someone playing it, whatever form that takes: a new
subsystem, a diagnostic dump, a fix to a debug-only path, a compiler flag,
a change to how the mod is packaged. If a change is real but no player
will ever notice it, this is where it goes.

### What's New: one subsection per feature domain

"What's New" is never a flat list. Group the release's new user-facing
surface into feature domains — the subsystem, mechanic, or area of the mod
a player would name if they were talking about it ("Gossip", "The
Director", "The Dashboard", "Beat Authoring") — and give each one an `###`
subsection. Order the domains by how much of the release they represent,
largest first.

A domain earns a subsection when a player can see or interact with it: new
beats, dashboard controls, tunables that shift Director behavior, event
sources feeding the LLM's context, MCM entries, major performance
improvements. A change that only a developer notices does NOT get a
subsection here — it belongs in "For Developers".

Within a subsection, lead with what the feature does for the player, then
the bullets that qualify it. A one-bullet domain is fine; do not pad it.
Do not create a "Miscellaneous" or "Other" domain — if a change fits no
domain, it is either its own small domain or it belongs in Fixes or
For Developers.

### Callouts go at the end of the feature they belong to

A callout is anything a reader has to know to use the feature correctly: a
hard requirement (a minimum SkyrimNet version, a companion mod), a
limitation, an expectation about pacing or visibility, an upgrade or
migration step, a save-compat warning. Every `IMPORTANT:` note, save-compat
caveat, and "users must do X before updating" line found in the commit
bodies in step 2 MUST land somewhere in the draft.

Place each callout as a blockquote at the END of its feature's subsection,
after that feature's bullets — not inline among them, and not collected
into a shared section at the bottom of the notes. A reader who only cares
about one feature reads its subsection and gets everything that applies to
it, including the caveats.

Only a caveat that belongs to no single feature — a global upgrade step, a
known issue spanning the whole mod — goes in the trailing "Notes" section.
If every caveat found a feature to attach to, omit "Notes" entirely.

### Fixes: one bullet per fix

Itemize. Each bullet is one individual user-visible bug fix, stating what
was wrong from the player's side and what now happens instead. Do not roll
several fixes into a "various fixes to X" bullet, and do not group fixes
under feature subheadings — "Fixes" is a flat list.

A fix that a player could never have observed is not a fix for this
purpose; it goes in "For Developers".

Framing rules:

- Player-facing framing under "What's New" and "Fixes", not engineer
  framing. "The Director now respects a minimum phase-dwell floor before
  advancing" beats "Refactor `PhaseTracker::EvaluateAdvance` signature."
  "For Developers" can be more technical, but still avoids raw file
  paths and symbol names when a behavior description works.
- Cite behaviors, not file paths or symbol names.
- Keep it tight. A short list beats a wall of prose.

Formatting rule (important — GitHub renders release notes with hard line
breaks): each bullet and each paragraph must be a **single unwrapped line**
in the source. Do NOT wrap prose at 80/100/120 columns inside a bullet or
paragraph — GitHub will render every soft newline as a `<br>`, breaking
sentences mid-thought on the published page. Only insert a newline where
you want the rendered output to actually break (between bullets, between
paragraphs, before/after headers). A multi-line blockquote callout is the
one exception in the template above; write it as a single `>` line too.

The draft you present to the user in chat can be wrapped for readability,
but the release notes you eventually write to disk in step 9 must be
unwrapped. Either draft unwrapped from the start, or unwrap before writing.

Present the draft in normal chat text, then gate approval using the
`AskUserQuestion` tool — NOT a plain-text "does this look good?" prompt. Use a
single-select question shaped like this:

- **question:** `"Approve these release notes, or cancel the release?"`
- **header:** `"Notes"`
- **options** (single-select, in this order):
  1. `"Approve as drafted (Recommended)"` — description: `"Ship these notes as-is."`
  2. `"Cancel the release"` — description: `"Abort the skill; no tag, package, or commit."`

The user provides revision requests through the harness-added "Other" slot —
do NOT list a "Request revisions" option yourself. Behavior by pick:

- Option 1 → explicit approval; proceed to step 5.
- Option 2 → abort the skill immediately with no side effects.
- Other with freeform text → treat as revision input (not approval) unless
  the text is unambiguously affirmative ("ship it", "approved", "looks
  good"). Apply the requested changes and re-present the updated draft via a
  fresh `AskUserQuestion` call. Repeat until they pick option 1 or option 2.

Do NOT proceed to packaging on ambiguous feedback.

## 5. Confirm authorization for the remaining operations

Before touching anything on the machine or on origin, get explicit user
authorization for the batch of operations the rest of the skill will perform.
This gate exists because the Claude Code auto-mode classifier will block
direct-to-main pushes (and similar high-blast-radius commands) unless the user
has said so in the current session — a mid-run block leaves the release
half-done (bump committed locally but not pushed, tag not pushed, GitHub
release not created), and untangling that is worse than asking once upfront.

Ask via `AskUserQuestion` with a single-select shape:

- **question:** `"Authorize the remaining release operations for vX.Y.Z?"`
- **header:** `"Authorize"`
- **options** (single-select, in this order):
  1. `"Authorize all remaining operations (Recommended)"` — description
     should enumerate exactly what will run: bump the two statics files,
     commit the bump and push to main, run `package.ps1`, tag `vX.Y.Z`, push
     the tag to origin, and create the GitHub release with the approved
     notes and the packaged zip attached.
  2. `"Cancel the release"` — description: `"Abort the skill; no bump, tag, or push."`

Enumerate the specific operations in the option-1 description so the
authorization is scoped and auditable — the user is agreeing to exactly this
list, not a blank check. If a later step needs an operation NOT in that list,
stop and ask separately.

If option 1, proceed to step 6. If option 2, abort the skill immediately with
no side effects. If Other with freeform text, treat it as revision input on
the operation list (not as approval) unless the text is unambiguously
affirmative ("do it", "authorized", "go ahead"). Re-present the
`AskUserQuestion` after any adjustment.

## 6. Bump the advertised version in tracked files and commit

Two files carry the mod's advertised version and must be updated to match the
accepted `$Version` before the packaged artifact is built (`package.ps1` runs
`build.ps1` which syncs these into the mod folder — the archive will bake in
whatever version is on disk at packaging time):

- `statics/SKSE/Plugins/SkyrimNet/config/plugins/NarrativeEngine/manifest.yaml`
  — update the `version: "..."` key in the top-level `plugin:` block.
- `statics/MCM/Config/NarrativeEngine/config.json` — update the
  `valueOptions.value` string on the version-display item under the `About`
  header (the row labeled `"NarrativeEngine"`). Only replace the leading
  `vX.Y.Z` token; preserve any trailing suffix (e.g. `(dev)`) verbatim.

Read both files first to confirm the current values, apply the edits, then
verify with a `git diff` that only the intended lines changed. If either edit
produces no diff (file was already at target version), stop and report — the
bump is a no-op and something is out of order.

Then commit and push the bump so the tag created in step 8 lands on a commit
that exists on origin:

```powershell
git add statics/SKSE/Plugins/SkyrimNet/config/plugins/NarrativeEngine/manifest.yaml `
        statics/MCM/Config/NarrativeEngine/config.json
git commit -m "chore(release): v$Version"
git push origin (git branch --show-current)
```

This step is exempt from step 1's "do not stage or commit on their behalf"
rule — the changes were made by the skill itself, not carried over from the
user's working tree.

If the push is rejected (e.g. branch protection, remote ahead), stop and
report. Do NOT force-push.

## 7. Package the mod

Run:

```powershell
pwsh -File package.ps1 -Version $Version
```

Wait for it to complete. On non-zero exit, report the failure and stop — do
not proceed to tagging. Confirm `out/NarrativeEngine-v<Version>.zip` exists on
disk before continuing.

## 8. Tag and push

Create an annotated tag and push it:

```powershell
git tag -a "v$Version" -m "NarrativeEngine v$Version"
git push origin "v$Version"
```

If either step fails (tag already exists, push rejected), stop and report — do
NOT force-push or delete existing tags without explicit user approval.

## 9. Create the GitHub release

Write the approved release notes to a temp file first so multi-line content
passes cleanly:

```powershell
$notesFile = New-TemporaryFile
Set-Content -LiteralPath $notesFile -Value $releaseNotes
gh release create "v$Version" `
    "out/NarrativeEngine-v$Version.zip" `
    --title "NarrativeEngine v$Version" `
    --notes-file $notesFile
Remove-Item -LiteralPath $notesFile
```

If `gh release create` fails after the tag has been pushed, report clearly that
the tag exists on origin but no release was created, and stop. The user can
either retry `gh release create` manually or delete the tag and re-run the
skill.

## 10. Report the release URL

Retrieve and print the URL as a clickable Markdown link:

```powershell
gh release view "v$Version" --json url --jq .url
```

Format the final output as:

> Release published: **[NarrativeEngine v$Version](<url>)**

Do NOT perform any post-release actions (editing the release, uploading
additional assets, drafting the next tag) unless the user asks.
