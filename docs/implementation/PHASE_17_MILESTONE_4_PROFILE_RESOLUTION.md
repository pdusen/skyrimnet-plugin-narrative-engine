# Phase 17 — Milestone 4: Profile Resolution

Plots need every schemer's character profile, and most schemers are people the player has never met. This milestone
replaces `CharacterBios`' guesswork with a resolver that picks the profile SkyrimNet itself would render for an NPC,
met or not: the same bio name, the same layer, the same fallbacks.

> **Doc status: not started.** Nothing here touches the co-save or the settings. It is the last obstacle to releasing
> the plot system.

The rules this implements were decoded from SkyrimNet's DLL and are written up as engine findings. They are the
contract for every step below, and where this document and they disagree, they win:

- [`skyrimnet-bio-template-name-assignment.md`](../engine-findings/skyrimnet-bio-template-name-assignment.md): how
  SkyrimNet names an actor's bio.
- [`skyrimnet-character-profile-lookup.md`](../engine-findings/skyrimnet-character-profile-lookup.md): the fallback
  chain, the layer stack, pins, dynamic bios and the translation tables.

---

## Why this milestone exists

SkyrimNet only hands out a profile through a UUID, and it mints a UUID when the player meets someone. On a fresh save
it knew 88 of the 881 people in the plot population. Issue
[MinLL/SkyrimNet-GamePlugin#744](https://github.com/MinLL/SkyrimNet-GamePlugin/issues/744) asks for an API that
does not need one; it has had no answer.

`CharacterBios` works around that by reading the bio files itself, and gets the wrong answer in ways nothing reports:

| What it does today                                                    | What SkyrimNet does                                                                    |
| --------------------------------------------------------------------- | -------------------------------------------------------------------------------------- |
| Matches a file by the low 12 bits of the id and a loose name test     | Computes the bio name exactly: translation tables, then a fixed slug                   |
| Refuses ambiguous or suffix-only matches, so drops real profiles      | Never guesses, so never needs to refuse                                                |
| Ranks pins below the overlay                                          | A pin beats the overlay; only the save layer beats a pin                               |
| Puts external layers the registry does not list on top                | Does not resolve a layer with no registry record at all                                |
| Ignores dynamic bios                                                  | Uses an actor's dynamic bio whenever one exists                                        |
| Skips `_generic` files, and has no `default`                          | Falls back to `<tag>_generic`, `<name>_generic`, then `characters/default`             |
| Chooses between an `external/` and a `library/` copy by `source`      | Keeps the copy with the higher manifest version                                        |

The result is that the simulation and SkyrimNet's own dialogue can portray the same person from different text, and
some people the simulation could have used are left out.

---

## Design

### What "the same as SkyrimNet" means here

For an actor in the plot population, the resolver returns the file SkyrimNet's `render_character_profile` would
render for them **now**: in this save, with this content library, this registry and these pins. Two inputs come from
the engine, and everything after that is pure.

**The bio name.**

- *Met actors:* when `SkyrimNetAPI::GetBioTemplateName` returns a name, that name is used. SkyrimNet stored it at
  first meeting. It already reflects anything that has happened since (a rename after meeting, a re-registered actor
  inheriting a former self's name), and recomputing it could only disagree.
- *Unmet actors:* the name is computed by the decoded rule: unique table, then generic table, then slug. It is what
  SkyrimNet will store when they are met, provided their display name has not changed by then.

**The file.** The chain is `characters/dynamic/<name>`, `characters/<name>`, then, for an actor whose base is not
flagged unique, `characters/<tag>_generic` and `characters/<name-key>_generic`, then `characters/default`. Each path
is resolved through the layer stack:

```text
save > pinned plugin > overlay > priority plugins > unprioritized plugins by id > skyrimnet.base
```

The first path in the chain that resolves is the profile.

### What a lookup reports

Each resolved profile carries **which tier** it came from and **which layer** provided it:

| Tier      | Meaning                                                           |
| --------- | ----------------------------------------------------------------- |
| `Dynamic` | `characters/dynamic/<name>`: LLM-written, this save only          |
| `Own`     | `characters/<name>`: a profile written for this person            |
| `Generic` | a `_generic` template, matched by bracketed tag or by name        |
| `Default` | `characters/default`                                              |
| `None`    | not even `default` resolves. A broken install, logged loudly      |

**Castable means `Dynamic` or `Own`.** `PlotBirth::IsCastable` exists so that a mastermind is somebody the model can
portray in character, and a `_generic` or `default` profile would make every such schemer read like the same person,
which is the exact problem plots set out to fix. Every tier's text still goes into prompts: an agent whose profile is
generic is shown the generic text, because that is how SkyrimNet portrays them too.

### Where the work happens

The shape stays `CharacterBios`': **one resolution per save, held for the session, read lock-free by the plot
worker.** `OnSessionStart` on the main thread gathers what needs the engine and the disk, and resolves every
population member. `OnRevert` drops it. That is `PlotPopulation`'s arrangement, for the same reason.

The engine reads at session start, all on the main thread:

- per population member: the reference id (`GossipGraph::ActorRefFor`), `GetDisplayFullName()` on the reference, the
  base NPC's `TESFullName`, the base's `GetFile(0)` file name, and the base's `kUnique` flag;
- per generic-table row: the row form's `TESFullName`, through the data handler;
- `SkyrimNetAPI::GetBioTemplateName` for each member, and the save id.

The disk, read on the same pass:

- `content-registry.json`;
- every layer's `manifest.json`;
- the relative paths under each layer's `prompts/`;
- the translation CSVs;
- the winning bio files.

The rule itself, from bio name to winning file, is pure and lives in **`CharacterBioResolver`**, a new core source
with no engine dependency. It takes the facts gathered above as plain data, and the unit-test target exercises the
real object file.

### Text handling

Every block read out of a profile goes through `LLMTextSanitizer::Sanitize`, as `CLAUDE.md` requires of anything an
LLM wrote. Dynamic bios are LLM output outright, and an overlay or pinned bio may have come from the dashboard's
profile generator, so the rule is applied to every tier rather than chosen per file. The dynamic bio's extra
`interject_summary` block is ignored, like `appearance` and `speech_style`.

### What is deliberately not mirrored

- **Rendering.** Today's bios are plain blocks with no template logic (see the profile-lookup finding), so reading the
  blocks yields the words a render would. If content ever ships template logic in a bio, this breaks; the census
  below counts files with anything but blocks so that the day it happens is visible.
- **Mid-session changes.** SkyrimNet re-resolves when the content library changes, when a dynamic bio is written, and
  when a pin is set. This resolves once per load. See open question 1.

---

## Implementation plan

Every step is owned by `[CLAUDE]` or `[USER]` and verified on its own, by Phase 17's rules (**How steps are verified**
and **How steps are executed**). Steps 1–4 are pure and add to `CharacterBioResolver` and its test file. Step 5 is
the only one that touches the engine.

Where a step checks itself against data on this machine (a save's `uuid_mappings` table, SkyrimNet's prompt logs), the
probe that does it is throwaway: written to the scratchpad, compiled against the core library, run, deleted, and its
result recorded under the step.

### Step 1 — The bio name

- [ ] Complete

**[CLAUDE]**

`CharacterBioResolver::BioName(facts, tables)`, from the facts it needs: the reference id, the display name, the base
name, the base plugin's file name, and whether the actor is the player. It returns the name SkyrimNet assigns:

- the unique table, keyed by lowercased base plugin and the reference's local id as `{:06X}`, with the light-plugin
  mask for `0xFE`;
- then the generic table, on `slug(trim(name))`;
- then `player_special` for `0x14`, nothing for an empty name or `"Unknown"`;
- then `slug(name)`;
- each suffixed with `_{id & 0xFFF:03X}` except `player_special`.

It also needs the slug, the trim (`" \t\n\r\f\v"`), and the Windows-1252 repair. The repair keeps valid UTF-8 as it
is and re-encodes invalid bytes as Windows-1252.

The tables are passed in already built; Step 2 builds them.

**Verification:**

- **Unit tests** pin the slug on the cases the finding names: a hyphen kept, brackets kept, an apostrophe and a slash
  replaced, non-ASCII bytes untouched, zero padding, the light-plugin mask, and the player and `"Unknown"` cases. They
  also check that a unique row beats the generic table and that a generic row beats the slug.
- **A probe** feeds every `uuid_mappings` row on this machine through `BioName`, with tables from the shipped CSVs, and
  must reproduce the finding's result: every row matched but Sahpur's two. Brand-Shei's two are reproduced by giving the
  probe her base plugin, not the reference's.

### Step 2 — The translation tables

- [ ] Complete

**[CLAUDE]**

Building the unique and generic tables from CSV text, as SkyrimNet loads them:

- quoted three-column CSV with a header row;
- files applied in byte-wise path order, a later row replacing an earlier one with the same key;
- the unique key normalised as in Step 1;
- the generic table keyed by `slug(trim(FULL))` of each row's form, where the caller supplies the name for a
  (plugin, form id) pair, or nothing when the form does not resolve.

Which files are in play is decided by the layer stack (Step 3), so this step takes a list of (path, text) already
chosen.

**Verification:**

- **Unit tests:** a later file overrides one row of an earlier one and leaves the rest; a malformed row or a bad hex id
  is skipped without dropping the file; a generic row whose form does not resolve contributes nothing; two generic rows
  with the same name keep the later.
- **Step 1's probe**, rerun on tables built by this code rather than the probe's own CSV reader, must give the same
  result.

### Step 3 — The layer stack and each path's winner

- [ ] Complete

**[CLAUDE]**

Turning the registry, the layer listing and the manifests into the stack, and choosing a winner per path. Input: the
`content-registry.json` text, and per layer its kind, id, manifest version and the set of relative paths it holds.

- **The registry is read with SkyrimNet's tolerances.** A non-boolean `enabled` counts as true; a missing `source`
  counts as `hub`; ids that collide case-folded, and non-string, duplicate or unknown `priority` entries, are dropped.
  An unparseable file is an empty registry.
- **The stack:**
  - the save layer, then the overlay;
  - plugins with a priority entry, in priority order;
  - registered plugins without one, by case-folded id;
  - then `skyrimnet.base`.

  A plugin layer with no registry record, or one that is disabled or `missing`, is left out. A library folder whose
  name is not its manifest id is left out. One id in both `external/` and `library/` keeps the higher semver.
- **The winner for a path:** the first layer in the stack that holds it. If that layer is the save layer, it wins
  outright. Otherwise a pin naming a layer that holds the path wins. Paths and pin ids compare case-folded.

**Verification:**

- **Unit tests,** one per rule above, including a pin beating the overlay, the save layer beating a pin, a pin whose
  plugin lacks the file falling back to stack order, an unregistered external ignored, and an unprioritized plugin
  ranked below every prioritized one.
- **A probe** builds the stack from this machine's registry and layer folders and resolves the 315 bios that
  SkyrimNet's prompt logs showed winning. It must pick the logged winner every time.

### Step 4 — The fallback chain

- [ ] Complete

**[CLAUDE]**

`CharacterBioResolver::Resolve(facts, name, tables, stack)` returns the winning file, its tier and its layer. It walks
the chain in the Design section:

- dynamic, then own;
- for an actor whose base is not unique, `<tag>_generic` from the text between the first `[` and the first `]` after
  it, trimmed, then `<name-key>_generic` from the whole display name;
- then `default`.

Each `_generic` key is the generic-table entry for the text when there is one and its slug otherwise, with no hex
suffix. A dynamic bio is `prompts/characters/dynamic/<name>.dynamic.prompt`; every other template is
`prompts/<template>.prompt`.

**Verification:** unit tests cover:

- a dynamic bio beating the actor's own;
- an own file beating any generic;
- a unique actor going straight from own to `default`;
- `[Courier]` resolving `courier_generic`, an untagged "Bandit" resolving `bandit_generic`, and a generic-table hit
  used verbatim;
- `None` only when `default` itself is missing.

### Step 5 — Wire it in

- [ ] Complete

**[CLAUDE]**

`CharacterBioLibrary` gathers the facts and the listing at `OnSessionStart` as the Design section says, resolves every
population member, reads each winning file's blocks through the sanitizer, and holds them.

- `Has` becomes "resolved `Dynamic` or `Own`". `For` returns whatever tier resolved.
- `Catalog`, `Find`, the suffix index, `NameAffinity` and the `Outcome` enum are deleted.
- The header comment's "RECOGNITION, NOT DERIVATION" section is replaced by a pointer to the two engine findings.

The census line becomes one log line per session:

- how many members came from a stored name and how many from a computed one;
- the count per tier, and per layer for `Own`;
- how many files carried anything but blocks.

At debug level, one line per member gives the name, the tier and the layer, so a session's log can be checked against
what SkyrimNet sent.

**Verification:**

- `pwsh -File build.ps1 build` is clean, and every test passes.
- `PlotBirth` and `PlotTick` compile unchanged against the new `Has` and `For`.
- No `CharacterBios` code path reads a file outside `OnSessionStart`.

### Step 6 — In-game validation

- [ ] Complete

**[USER]**

On a save with plots running, the logs at debug, and `openrouter_input.log` kept:

1. Load, and read the census line. How many members are castable now, against the old census's "bio(s) loaded" figure on
   the same save?
2. In the dashboard, edit one NPC's bio in the overlay and pin a different plugin's version of another. Reload. Talk
   to both, and to one NPC who has a dynamic bio.
3. Let a plot be born and read its birth prompt. Do the masterminds read in character?

Record the census and anything surprising under this step.

### Step 7 — The resolver against what SkyrimNet sent

- [ ] Complete

**[CLAUDE]**

A throwaway harness over Step 6's logs compares each member's resolved file, from the debug lines, with the profile
text SkyrimNet put into `openrouter_input.log` for the same person.

**Verification:** every person who appears in both agrees, including the overlay edit, the pin and the dynamic bio
from Step 6. Each disagreement is either explained by a rule above and fixed, or written up here and in the engine
findings.

---

## Open questions

1. **Should the resolution refresh mid-session?** A dynamic bio written during play, a dashboard edit or a new pin
   shows in SkyrimNet's dialogue at once and in plots only after the next load. A cheap answer is to re-resolve at the
   start of a plot tick when the registry's or the save layer's modification time has moved. Left until Step 6 shows
   whether the lag is noticeable.
2. **What happens when SkyrimNet changes these rules?** They are decoded from Beta 26 RC4 and nothing announces a
   change. Step 7's harness is the check to rerun after a SkyrimNet update, and issue #744 remains the durable fix; if
   it lands, this resolver should give way to it.
