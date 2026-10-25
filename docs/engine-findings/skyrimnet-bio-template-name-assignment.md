# How SkyrimNet names an actor's bio

## TL;DR

SkyrimNet does not look an actor's bio up. It **computes** the bio's name (its `bio_template_name`, the stem of
`prompts/characters/<stem>.prompt`) from the actor once, the first time it registers them, and stores the answer in
the save's `uuid_mappings` table. The calculation needs nothing SkyrimNet learns by registering the actor, so it can
be run for anyone, registered or not:

```text
id = the actor's REFERENCE form id

1. unique  = UniqueTable[(lower(plugin of the actor's BASE NPC), local(id) as {:06X})]
             -> "<unique>_<id & 0xFFF:03X>"
2. generic = GenericTable[slug(trim(name))]
             -> "<generic>_<id & 0xFFF:03X>"
3. id == 0x14                       -> "player_special"
   name empty, or exactly "Unknown" -> no bio
4.           -> "<slug(name)>_<id & 0xFFF:03X>"

name     = TESObjectREFR::GetDisplayFullName(), falling back to the base NPC's own name when that is empty,
           then repaired from Windows-1252 to UTF-8
slug(s)  = ASCII-lowercase s, then replace each ' '  '\''  '\\'  '/'  with '_'; every other byte is kept
local(i) = i & 0xFFF for a light-plugin form (high byte 0xFE), otherwise i & 0xFFFFFF
```

Checked against every `uuid_mappings` row in 17 save databases on the development machine: **9,094 of 9,098** stored
names reproduced exactly. The four misses are explained below.

Decoded from SkyrimNet Beta 26 RC4 (`skyrimnet-beta26-rc4.zip`, DLL dated 2026-10-03), function
`SkyrimNet::Skyrim::Utils::UUIDResolver::AddMapping`. How the DLL was read is in
[reading-the-skyrimnet-dll.md](reading-the-skyrimnet-dll.md); what SkyrimNet does with the name once it has it is in
[skyrimnet-character-profile-lookup.md](skyrimnet-character-profile-lookup.md).

## The two translation tables

Both are CSV files shipped as content, under `prompts/translation/` in a content layer. On this install only
`library/skyrimnet.base` provides them. Each filename resolves through the layer stack like any other path. Every
resolved `.csv` under the folder is then loaded in path order, and for the unique table a later row replaces an
earlier one with the same key. See "Translation CSVs" in
[skyrimnet-character-profile-lookup.md](skyrimnet-character-profile-lookup.md).

### Unique: `prompts/translation/unique/*.csv`

```text
"pluginName","refId","sanitizedEnglishKey"
"Skyrim.esm","019DDC","brandish"
```

2,228 rows in the base content. Keyed on one specific placed reference. This table is where every "irregular" bio
name comes from: Brand-Shei is `brandish`, Siddgeir is `jarl_siddgeir`, the Voice of Namira is `namira`, Bujold the
Intrepid is `bujold_the_unworthy`, Arniel Gane is `arniel_s_shade`, Balgruuf is `balgruuf_the_greater`. It also
**wins over the name**. A courier renamed by an NPC-names mod to "Boren Vallkirson [Courier]" is still `courier_FB7`.

How the key is built (`PromptNameTranslation::TryResolveUniqueBioKey` and the helpers it calls):

- **The plugin is the base NPC's, not the reference's.** For an actor it takes the base NPC's `sourceFiles[0]` file
  name and falls back to the reference's own only if the base has none. The name is lowercased. The observed
  difference is real: Brand-Shei's reference reports `unofficial skyrim special edition patch.esp` (that is what
  SkyrimNet stored as her `mod_file`), yet she resolved through the `Skyrim.esm` row.
- **The id is the reference's**, local part only, as six uppercase hex digits. The light-plugin mask is applied
  whenever the high byte is `0xFE`.
- Form id `0` and `0x14` never match.

To reproduce it, call what SkyrimNet calls: `actor->GetActorBase()->GetFile(0)` for the plugin, and the reference
form id for the id. Do not substitute "the plugin that defines the reference", even though for most actors it is
the same file.

### Generic: `prompts/translation/generic/*.csv`

```text
"pluginName","formId","sanitizedEnglishKey"
"Skyrim.esm","0745CD","deceased_refugee"
```

766 rows in the base content. The rows name **base NPC** forms, but the table is **not** looked up by form. At load
time (`PromptNameTranslation::ApplyGenericRowLocalizedKeys`) SkyrimNet resolves each row's form to its in-game,
localized name and indexes `slug(trim(that name))` to the English key. At registration the actor's name goes through
the same `slug(trim(...))` and is looked up.

So the table maps a **name** to a key, for every actor carrying that name, from any plugin. Every "Refugee" in the
game is `deceased_refugee_*`, including the ones JK's Skyrim places. On an English install it only changes the
result when a row's key differs from the slug of its own name. Its real purpose is non-English installs, where it
maps "Flüchtling" back to the English file.

The name indexed is the row form's `TESFullName` (its `FULL`, in the game's language), repaired from Windows-1252
like the actor's name. The form is looked up through the data handler from the row's plugin and local id. When two
rows produce the same name, the later row wins (`Overriding generic translation '{}' -> '{}' (was '{}')`), in the
same file-then-row order as the unique table. The validation below took English `FULL` names from the Spriggit
export, and that reproduced every row it could reach.

## The slug

`slug` is all the normalisation there is:

- ASCII `tolower` per byte, using the C locale, so bytes above 0x7F pass through untouched;
- `' '`, `'`, `\` and `/` become `_`;
- **everything else is kept**: hyphens (`parvana_al-rihad`), square brackets (`sahpur_[black-briar_mercenary]`),
  digits, and any UTF-8;
- the fallback path (step 4) does **not** trim, but the generic lookup (step 2) does.

The suffix is `{:03X}`, so three uppercase hex digits, zero padded (`grelka_0B3`). It is the low twelve bits of the
reference id. The base NPC's id is never used.

The name is `TESObjectREFR::GetDisplayFullName()` on the reference. That is CommonLibSSE-NG's
`RE::Offsets::TESObjectREFR::GetDisplayFullName`, SE id 19354, which SkyrimNet calls through a relocation thunk. A
rename the engine knows about (an `ExtraTextDisplayData` from a quest or a names mod) is therefore part of the key.
Before slugging, the name is repaired: bytes 0x80-0x9F are mapped through a Windows-1252 table and re-encoded as
UTF-8.

## Order and its one oddity

The order is unique, then generic, then the fallback, and the fallback is where the player and the "Unknown" name
are handled. So a player whose name is in the generic table gets that key at first registration.
`UUIDDriftConsolidator::UpdatePlayerBioTemplateName` exists to rewrite the player to `player_special` afterwards.
None of this matters to a plugin that never asks about the player.

## What the computation does not do

- **It does not check that a file exists.** The name is stored whether or not any layer provides that bio. Whether
  something renders is decided later, at render time (see
  [skyrimnet-character-profile-lookup.md](skyrimnet-character-profile-lookup.md)).
- **It runs once.** The result is stored in `uuid_mappings` and reused. Renaming the actor afterwards does not
  change it. `UUIDResolver::ResolveClassBioTemplate` also lets a re-registered actor "inherit" a former self's name
  (log: `uuid {:X} inherits bio template '{}' from former self {:X}`). That path was not decoded.

For an actor SkyrimNet has already registered, the stored name is the authoritative answer
(`PublicGetBioTemplateName`). For anyone else, the computation above is what SkyrimNet *will* store when it registers
them, provided the actor's display name has not changed in between.

**Registered is not the same as met.** What makes SkyrimNet register an actor was not decoded. It is far broader
than the player meeting them: the testing save's `uuid_mappings` holds about 1,800 rows, many more people than that
save has encountered.

## Validation

`uuid_mappings` holds `form_id`, `actor_name` (the name at registration), `bio_template_name`, and, in newer
schemas, `mod_file` and `local_form_id`. Every row across 17 databases in `SkyrimNet/data/` was run through the rule
above, using:

- the two CSVs as shipped in `library/skyrimnet.base`;
- `mod_file` + `local_form_id` for the unique lookup. Older schemas lack them, so for those rows the plugin was
  inferred from the load-order index (`00` to `04`, the five masters only);
- generic names from the Spriggit export. That reaches 233 of the 766 generic rows; the rest name mod plugins.

| Path    | Matched |
| ------- | ------- |
| unique  | 3,159   |
| generic | 343     |
| slug    | 5,559   |
| player  | 33      |

9,094 of 9,098 rows that carry a name. A further 1,087 rows store an empty name and were not examined. The four
misses:

- **Brand-Shei, 2 rows.** The test used the reference's `mod_file`, which is USSEP, so it missed the `Skyrim.esm`
  unique row. SkyrimNet itself used her base NPC's plugin and found it. This miss is what confirms the base-plugin
  rule.
- **Sahpur, 2 rows**, in the oldest database. The test predicted the unique key `black-briar_mercenary`, but
  SkyrimNet stored the slug. Most likely the row predates that table entry.

## What this replaces

`include/CharacterBios.h` (the "RECOGNITION, NOT DERIVATION" section) says that building `slug(name)_<hex>`
reproduces the real name "about 70% of the time". It lists hyphens, bracketed suffixes, zero padding and files
"named for someone else entirely" as reasons it cannot work. Every one of those is accounted for above: the first
three are what the real slug does, and the last is the unique table. The 70% came from a different slug and no
tables, so it says nothing about the rule SkyrimNet uses.
