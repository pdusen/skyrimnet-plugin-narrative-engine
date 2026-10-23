# Phase 17 — Milestone 4: Profile Resolution

Plots need every schemer's character profile, and most schemers are people the player has never met. This milestone
replaces `CharacterBios`' guesswork with a resolver that picks the profile SkyrimNet itself would render for an NPC,
met or not: the same bio name, the same layer, the same fallbacks.

> **Doc status: Steps 1–5 complete; Step 6's in-game check is next.** Nothing here touches the co-save or the
> settings. It is the last obstacle to releasing the plot system.

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

**Resolved afresh at the start of every plot tick.** SkyrimNet re-resolves whenever its content changes: a dynamic bio
written during play, a dashboard edit, a new pin, an actor met for the first time. Ticks are hours of game time apart,
so redoing the whole resolution each time costs nothing that matters, and it means a plot never reads a profile more
than one tick older than the one SkyrimNet would render.

The work splits along what each part needs:

- **Engine facts, once per save, on the main thread.** `OnSessionStart` gathers:
  - per population member: the reference id (`GossipGraph::ActorRefFor`), `GetDisplayFullName()` on the reference,
    the base NPC's `TESFullName`, the base's `GetFile(0)` file name, and the base's `kUnique` flag;
  - per generic-table row: the row form's `TESFullName`, through the data handler.

  These are record data and do not change during a session. They are held stable for it and read lock-free by the
  plot worker, with the same lifecycle and guarantees as `PlotPopulation::Get()`. `OnRevert` drops them.
- **Everything else, every tick, on the plot worker.** `CharacterBios::Refresh(pt)`, the first thing `RunTick` does:
  - asks `SkyrimNetAPI::GetBioTemplateName` for each member, plus the save id, both of which change as the player
    meets people and changes saves;
  - re-reads the disk: `content-registry.json`, every layer's `manifest.json`, the relative paths under each layer's
    `prompts/`, the translation CSVs, and the winning bio files;
  - re-resolves every member, and replaces the held profiles wholesale.

**The plot worker owns the resolved profiles.** Every reader (`PlotTick`'s castability filter, `PlotBirth`'s prompt
sections and occupations) runs inside a tick, after that tick's refresh, so no reader can see a previous save's
profiles and none needs a lock.

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
- **Changes between ticks.** SkyrimNet applies a change at once; plots see it at the next tick.
- **A rename mid-session, before meeting.** Display names are read once per save. An unmet NPC renamed during the
  session is named by their old name until the next load.

---

## Implementation plan

Every step is owned by `[CLAUDE]` or `[USER]` and verified on its own, by Phase 17's rules (**How steps are verified**
and **How steps are executed**). Steps 1–4 are pure and add to `CharacterBioResolver` and its test file. Step 5 is
the only one that touches the engine.

Where a step checks itself against data on this machine (a save's `uuid_mappings` table, SkyrimNet's prompt logs), the
probe that does it is throwaway: written to the scratchpad, compiled against the core library, run, deleted, and its
result recorded under the step.

### Step 1 — The bio name

- [x] Complete

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

Done. `CharacterBioResolver` is a new core source with `BioName`, `Slug`, `Trim`, `RepairWindows1252`, `LocalId` and
`UniqueKey`, and an `ActorFacts` / `Tables` pair for its inputs. The Windows-1252 table is SkyrimNet's own, read out of
the DLL: standard Windows-1252, with the five undefined bytes mapped to the C1 control of the same value. A byte that
does not begin a well-formed UTF-8 sequence is re-encoded on its own and decoding resumes at the next, as SkyrimNet
does.

- **Unit tests:** four suites cover every case listed above, plus the base name standing in for an empty display name
  and the repair running before the slug. All 505 tests pass.
- **The probe** compiled against `NarrativeEngineCore.lib` and read all 9,098 named rows from 17 save databases.
  - It reproduced **9,096**. The two misses are Sahpur's rows in the oldest database, as predicted.
  - The unique table came from the shipped CSV, read by the probe's own small reader until Step 2's builder exists.
    The generic table came from the 233 rows whose forms Spriggit could name.
  - The base plugin was the master that owns the id (load-order index 00 to 04) where there is one, and the stored
    `mod_file` otherwise. That is what makes Brand-Shei come out as `brandish_DDC`.

### Step 2 — The translation tables

- [x] Complete

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

Done. `BuildTables` takes the two file lists and two callbacks for the engine facts the loader needs:
`IsLightPlugin` and `FormName`. `ParseHexId` is public so the tests can pin its rules directly. The DLL settled three
details the plan left open:

- **The reader is rapidcsv** with the header as row 0, `,` as the separator and `"` quoting, so `ForEachRow` drops the
  first line and `SplitCsvLine` unquotes. A row is skipped when its trimmed plugin is empty or starts with `#`,
  SkyrimNet's comment rule.
- **An id** is trimmed, may carry `0x` or `0X`, and must then be one to eight hex digits.
- **A unique row's id is masked by how its plugin is loaded:** twelve bits for a light plugin, twenty-four otherwise,
  and twenty-four for a plugin that is not loaded. Generic ids go to the form lookup as written.

A row with fewer than three cells is skipped. rapidcsv would throw on it instead, and whether SkyrimNet then drops the
whole file was not decoded. No shipped file has such a row.

- **Unit tests:** `ParseHexId` and `BuildTables` cover each case listed above, plus the light mask, the header, and the
  argument order not mattering. 507 tests pass.
- **The probe**, rerun with both tables from `BuildTables`, again reproduced **9,096 of 9,098**, with the same two
  Sahpur rows missing. The shipped CSVs gave 2,228 unique rows in 2,227 keys (one row overrides another) and 233 named
  generic rows; 533 generic rows name forms outside the five masters, which Spriggit cannot name.

### Step 3 — The layer stack and each path's winner

- [x] Complete

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

Done. `ParseRegistry`, `CompareVersions`, `BuildStack` and `Winner` hold the stack. A `Layer` is plain data: kind,
folder, manifest id and version, and the set of relative paths it holds. Decoding the rest of the DLL corrected the
plan in two places and filled in a rule it had left open:

- **Paths and pin ids compare exactly, not case-folded.** The pin map and the snapshot are keyed by a byte-wise
  compare, and a pin's id is matched to a layer's after trimming only. Only the registry folds case, when it drops a
  record or pin that differs from an earlier one only in case. `Winner` therefore treats `grelka_0b3.prompt` and
  `grelka_0B3.prompt` as different paths, and normalises only `\` to `/`.
- **The unprioritized group is empty in practice.** SkyrimNet's registry loader appends a priority entry, in key
  order, for every record that lacks one. `ParseRegistry` does the same, so the stack has no separate group for
  them.
- **Two copies of one id:**
  - the higher version wins;
  - on a tie a `library/` copy beats an `external/` one;
  - then `library/skyrimnet.base` beats `library/skyrimnet.base.update`, the reserved folder SkyrimNet's content
    updater fills with newer shipped defaults;
  - then the copy discovered first stays.

  Versions compare by semantic-versioning precedence.

The reserved registry id that is dropped is taken to be `skyrimnet.base`, the one id with no record of its own. The
DLL's reserved-id check was not decoded far enough to confirm the list. The profile-lookup finding is updated with
all of the above.

- **Unit tests:** `ParseRegistry`, `CompareVersions`, and `BuildStack` with `Winner` cover each rule in the step and
  each correction. 510 tests pass.
- **The probe** merged the MO2 `Default` profile's enabled mods and `overwrite` into the tree the game sees, read
  each layer's manifest and the live registry, and built the stack.
  - The stack came out as the overlay, the registry's priority list, then `skyrimnet.base`. It correctly left out
    `pdusen.skyrim-but-its-p` (flagged `missing`) and `library/.archive` (no manifest naming it).
  - It picked the logged winner for **315 of 315** bios.

### Step 4 — The fallback chain

- [x] Complete

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

Done. `CharacterBioResolver::Resolve` returns a `Resolution`: its `Tier`, the winning content path, and the layer
providing it. `ActorFacts` gained `baseUnique`. Two details came out of the decorator rather than the plan:

- **An actor with no bio name resolves to `None`,** not to `default`. The decorator stops with "No bio template found"
  before its fallbacks, which only run when a named template fails to render. Today that is an actor named
  `"Unknown"` or with no name at all.
- **Which text each `_generic` key is slugged from.** The bracketed tag is trimmed before it is slugged; the whole
  name is slugged untrimmed, as SkyrimNet's second attempt does. Both are looked up in the generic table on their
  trimmed slug first. The name is the display name, else the base's, repaired as in Step 1.

**Unit tests** cover every case listed above, the fall from a tag to the whole name, and the `None` for no bio name.
511 tests pass. The first run crashed in the test file, not the resolver: one section built a stack over a temporary
vector of layers, whose pointers the header says must outlive the stack.

### Step 5 — Wire it in

- [x] Complete

**[CLAUDE]**

The split in the Design section:

- `CharacterBioLibrary` gathers the engine facts at `OnSessionStart` and drops them at `OnRevert`.
- `CharacterBios::Refresh(pt)`, called first in `PlotTick`'s `RunTick`, does the rest on the plot worker. It queries
  SkyrimNet's stored names, reads the listing, resolves every population member, reads each winning file's blocks
  through the sanitizer, and replaces the held profiles.
- `Has` becomes "resolved `Dynamic` or `Own`". `For` returns whatever tier resolved.
- `Catalog`, `Find`, the suffix index, `NameAffinity` and the `Outcome` enum are deleted.
- The header comment's "RECOGNITION, NOT DERIVATION" section is replaced by a pointer to the two engine findings.

The census becomes one log line per refresh:

- how many members came from a stored name and how many from a computed one;
- the count per tier, and per layer for `Own`;
- how many files carried anything but blocks.

At debug level, one line per member gives the name, the tier and the layer. It is written for every member on a
session's first refresh and, after that, only for members whose resolution changed since the previous tick, so a
session's log can be checked against what SkyrimNet sent without repeating 881 lines every tick.

**Verification:**

- `pwsh -File build.ps1 build` is clean, and every test passes.
- `PlotBirth` and `PlotTick` compile unchanged against the new `Has` and `For`, apart from the `Refresh` call.
- `Refresh` takes a `PlotThread::Token`, and no `CharacterBios` code path reads a file outside it.

Done.

- **`OnSessionStart` (main thread)** publishes one immutable `SessionFacts` snapshot through an
  `std::atomic<std::shared_ptr>`, so a tick in flight across a load keeps the copy it started with. The snapshot
  holds:
  - each population member's `ActorFacts`;
  - the set of plugins the data handler flags light;
  - the `TESFullName` of every row in every generic CSV any layer holds. Any of those files may win a later refresh.
- **`OnRevert`** clears the snapshot.
- **`Refresh(pt)` runs first in `RunTick`**, straight after `TakePendingState`. It:
  - asks SkyrimNet for the save id and each member's stored bio name;
  - scans every layer's `manifest.json` and its files under `prompts/`;
  - parses the registry, builds the stack, and builds the tables from each CSV's winning copy;
  - resolves every member, reading each winning file's blocks through `LLMTextSanitizer`;
  - replaces the worker-owned profiles.

`Has` is now `Dynamic` or `Own`. The suffix catalogue, `Find`, `NameAffinity`, `Normalise` and the `Outcome` enum are
gone, and the header points to the two engine findings instead.

The census is one info line per refresh:

- stored against computed names;
- the five tiers;
- `Own` counts by layer;
- the stack's size and the table rows;
- profiles holding template logic;
- how many resolutions changed.

Each member gets a debug line on a session's first refresh, and after that only when its name, tier, layer or path
changes.

**Verification:**

- `pwsh -File build.ps1 build` is clean, with no errors or warnings in any target, and all 511 tests pass.
- `PlotBirth` is unchanged. `PlotTick` gained only the include and the `Refresh` call.
- The mocked-engine target needed one new mock, `TESDataHandler::LookupForm`. It answers `nullptr`, because the
  mocked world loads no plugins.

**The last check holds only for the per-tick work.** `OnSessionStart` reads files too: it scans the layers and reads
the generic CSVs to learn which forms to name, because naming a form needs the main thread and the plot worker
cannot call onto it. The Design section asks for exactly that, so the check as written contradicted the design. The
files read there are the CSVs and the layer listing, never a profile.

### Step 6 — In-game validation

- [ ] Complete

**[USER]**

Open the game, load a save with plots enabled, and let several plot ticks run. A plot being born during them helps,
but is not required. Then quit. Nothing needs judging or recording; Step 7 reads everything from the logs.

**What the logs carry.** Everything below is written by default; the plugin's logger runs at trace level.

- **Once per session, at load:** a session-facts line counting members, light plugins and generic row names.
- **Once per session, at the first refresh:**
  - the save id, the layers on disk, the stack in order, and the pins;
  - a line for every member whose SkyrimNet-stored bio name differs from the computed one, with the facts that
    produced it.
- **Every refresh, so every tick:** the census line. It now also says how many stored names the rule computed
  identically.
- **Per member, on the first refresh and on any later change:** a tab-separated debug line holding:
  - the base and reference ids;
  - the tier, layer and path;
  - the stored and computed names;
  - the unique flag, the base and reference plugins, and the population, display and base names.

The resolver computes the name for every member, met or not, and checks it against SkyrimNet's stored name wherever
there is one. So every NPC the save has met is a test of the naming rule on real engine data. The stored name is
still the one used.

### Step 7 — The resolver against the logs, the disk and SkyrimNet

- [ ] Complete

**[CLAUDE]**

A throwaway harness over Step 6's session. Its inputs are `NarrativeEngine.log`, `NarrativeEngine_Plots.log`,
SkyrimNet's `openrouter_input.log`, the save's `uuid_mappings` table, and the MO2-merged content tree as Step 3's
probe built it.

**Verification:** each of these holds, or every exception is explained by a rule above and fixed, or written up here
and in the engine findings:

1. **The naming rule on real engine data.** For every member SkyrimNet has met, the computed name equals the stored
   one. A disagreement is checked against `uuid_mappings`. The `actor_name` stored there is the name at first
   meeting, so a rename since then is the documented exception, and anything else is a bug.
2. **The engine facts.** Every member with a reference id has a base plugin. Every unique flag matches the base
   record, checked through Spriggit for the masters. Every unique-table hit uses the base's plugin.
3. **The in-game stack against the disk.** The logged stack order equals the stack Step 3's probe builds offline from
   the same tree and registry. Each member's logged tier, layer and path equal what `Resolve` gives offline from the
   logged facts and that stack, so the in-game scan read the virtual file system correctly.
4. **The refresh.** There is a census line per tick, and the later ticks report only what genuinely changed between
   them.
5. **Dynamic bios.** Every member whose dynamic bio exists in the loaded save's layer resolves `Dynamic`, from the save
   layer.
6. **The prompts.** If a plot was born, its birth prompt in `openrouter_input.log` carries the sanitized blocks of
   each named person's resolved file.
7. **SkyrimNet's own renders.** Wherever SkyrimNet rendered the profile of a population member during the session,
   the text it sent comes from the same file the resolver chose. This check is opportunistic: it covers only the
   people SkyrimNet happened to render.

**What a load-and-tick session cannot show:** how a player's own overlay edit or pin ranks against the other layers,
since a session that makes neither has nothing to observe. Those rules rest on the disassembly and Step 3's unit tests.
They also have no instance on the development install other than one pin to the layer that wins anyway.
