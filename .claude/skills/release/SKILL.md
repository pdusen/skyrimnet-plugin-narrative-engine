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

Failing here costs nothing. Failing at step 8 after the tag is already pushed
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

Read the changes with:

```powershell
git log --no-merges --pretty=format:'%h %s' <range>
git diff --stat <range>
```

Skim commit subjects for user-visible changes. Ignore purely mechanical churn
(formatter runs, doc-only edits with no player-facing effect) when it isn't
relevant to a user's install decision.

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

Draft in this shape (skip sections that would have zero bullets — don't ship
an empty "Fixes" header just to keep the template):

```markdown
## Summary

<one or two sentences describing what this release is about>

## What's New

- <bullet per user-visible change>

## Fixes

- <bullet per user-visible bug fix>

## Notes

<optional caveats — known issues, upgrade steps, etc.>
```

Framing rules:

- Player-facing framing, not engineer framing. "The Director now respects a
  minimum phase-dwell floor before advancing" beats "Refactor
  `PhaseTracker::EvaluateAdvance` signature."
- Cite behaviors, not file paths or symbol names.
- Keep it tight. A short list beats a wall of prose.

Formatting rule (important — GitHub renders release notes with hard line
breaks): each bullet and each paragraph must be a **single unwrapped line**
in the source. Do NOT wrap prose at 80/100/120 columns inside a bullet or
paragraph — GitHub will render every soft newline as a `<br>`, breaking
sentences mid-thought on the published page. Only insert a newline where
you want the rendered output to actually break (between bullets, between
paragraphs, before/after headers).

The draft you present to the user in chat can be wrapped for readability,
but the release notes you eventually write to disk in step 8 must be
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

## 5. Bump the advertised version in tracked files and commit

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

Then commit and push the bump so the tag created in step 7 lands on a commit
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

## 6. Package the mod

Run:

```powershell
pwsh -File package.ps1 -Version $Version
```

Wait for it to complete. On non-zero exit, report the failure and stop — do
not proceed to tagging. Confirm `out/NarrativeEngine-v<Version>.zip` exists on
disk before continuing.

## 7. Tag and push

Create an annotated tag and push it:

```powershell
git tag -a "v$Version" -m "NarrativeEngine v$Version"
git push origin "v$Version"
```

If either step fails (tag already exists, push rejected), stop and report — do
NOT force-push or delete existing tags without explicit user approval.

## 8. Create the GitHub release

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

## 9. Report the release URL

Retrieve and print the URL as a clickable Markdown link:

```powershell
gh release view "v$Version" --json url --jq .url
```

Format the final output as:

> Release published: **[NarrativeEngine v$Version](<url>)**

Do NOT perform any post-release actions (editing the release, uploading
additional assets, drafting the next tag) unless the user asks.
