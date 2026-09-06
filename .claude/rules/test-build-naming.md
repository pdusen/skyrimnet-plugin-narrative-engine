# Test builds: the current version, plus a tag saying what it is for

Applies to every packaged build handed to a tester rather than published as a release.

## The name

```text
NarrativeEngine-v<current version>-<tag>.zip
```

`package.ps1` builds that filename from whatever it is given, so the whole thing is one argument:

```powershell
pwsh -File package.ps1 -Version 0.5.0-gossip-persistence
```

The archive lands in `out/`, which is gitignored. Nothing about a test build is ever committed.

## The version is read, never chosen

Use the version the mod currently advertises. It lives in
`statics/SKSE/Plugins/SkyrimNet/config/plugins/NarrativeEngine/manifest.yaml`, as the `version` key
under the top-level `plugin:` block, and between releases it matches the latest `git tag`. Read it;
do not carry a number over from an earlier build in the same session.

**Never invent the next version.** A test build is a build *of* the current version — it is not a
preview of the next one, and the version number is not a serial. Two test builds a day apart are both
`v0.5.0`; what tells them apart is the tag. Incrementing to `0.5.1`, `0.5.2` and so on for successive
builds guesses at a release that has not happened, and the guess is usually wrong: the release skill
decides the real number later, with the user, from the commits that actually landed.

For the same reason, packaging a test build never touches the two files that carry the advertised
version. Bumping those is step 6 of the release skill and belongs to a real release.

## The tag

A few words on what the build is for — the fix under test, the subsystem, the reason it is going out.
It is the only thing distinguishing successive builds of one version, so reach for a tag that says
what changed before reaching for a number.

Lowercase, hyphen-separated, no spaces. Keep it short enough to survive being pasted into a chat
message.

**Look in `out/` before settling on a tag.** If the one you want is already on a file there, suffix an
incrementing number — `-2`, then `-3` — rather than padding the tag with a word that means nothing.
`package.ps1` deletes an existing archive of the same name without asking, so a repeated tag silently
replaces a build a tester may still be working through. The earlier build keeps its bare tag and is
never renamed; only the new one carries the number.

## Example

```text
NarrativeEngine-v0.5.0-gossip-persistence.zip     first build sent for the co-save wipe
NarrativeEngine-v0.5.0-gossip-persistence-2.zip   same fix, after the schedule anchor was added
NarrativeEngine-v0.5.0-letter-courier-repro.zip   unrelated build, same version, different subject
```
