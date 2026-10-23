# How SkyrimNet finds the profile file for a bio name

## TL;DR

Given an actor's bio name (see [skyrimnet-bio-template-name-assignment.md](skyrimnet-bio-template-name-assignment.md)),
`render_character_profile` renders the first of these that works:

```text
player:      characters/dynamic/player_special  if it resolves,  else  characters/player_special
everyone:    characters/dynamic/<name>          if it resolves,  else  characters/<name>
then, only if the actor is NOT flagged unique:
             characters/<tag>_generic           tag = the text inside [...] in the display name, if any
             characters/<name-key>_generic      name-key = the whole display name
then:        characters/default
```

Each template path is resolved through SkyrimNet's layered content store (Beta 25 and later). For every path, the
copy that wins is:

```text
1. the save layer's copy             saves/<save id>/   -- wins even over a pin
2. the pinned plugin's copy          content-registry.json "pins", if that plugin provides the file
3. the overlay's copy                overlay/
4. plugins in "priority" order       first entry highest
5. registered plugins with no priority entry, by case-folded id (in practice none: the registry loader appends a
   priority entry for every record that lacks one)
6. skyrimnet.base
```

Everything in this document was decoded from SkyrimNet Beta 26 RC4 (DLL dated 2026-10-03). Where it could be checked
against what SkyrimNet actually did, it was; see "Corroboration". Method:
[reading-the-skyrimnet-dll.md](reading-the-skyrimnet-dll.md).

## The render fallback chain

From the `render_character_profile` decorator (`DecoratorLibrary::Initialize`, lambda 3; arguments `render_mode`
and a UUID).

### 1. Dynamic, then static

- **The player** renders `characters/dynamic/player_special` if it resolves, otherwise `characters/player_special`.
- **Everyone else:** the bio name comes from `UUIDResolver`. When there is none, the decorator fails
  (`No bio template found for UUID`); that is the gate that keeps it from serving an unregistered actor. It then
  uses `characters/dynamic/<name>` **whenever that template resolves** (`Using dynamic bio for UUID`), and
  `characters/<name>` otherwise (`Using static bio`). There is no feature switch in this decision; the file's
  existence is the switch.

**Dynamic bios** are profiles the LLM writes for actors the player spends time with. They have the same block
format as a static bio and live at:

```text
saves/<save id>/prompts/characters/dynamic/<name>.dynamic.prompt
```

`CharacterProfileGenerator::WriteDynamicBio` writes them through `ContentStore::WriteFile`, and refuses when no
playthrough is loaded (`Not writing dynamic bio for '{}': no playthrough is loaded`). So they are per save, they
exist only for actors SkyrimNet has met, and they resolve through the same layer stack as any other path.

### 2. `_generic`, for actors that are not unique

If the render fails (`Failed to render bio template. Trying fallback options`), the decorator checks
`Entity::IsUnique`. That is the base NPC's `kUnique` flag (`TESActorBaseData` flags, bit 5), or true for one of
SkyrimNet's own virtual entities. **A unique actor skips straight to `default`.**

For a non-unique actor it tries up to two `_generic` templates, built from `Entity::GetDisplayName`. That is the
same name as the bio-name rule uses: `GetDisplayFullName`, falling back to the base NPC's name, repaired from
Windows-1252.

1. **The bracketed tag.** If the name contains a `[` with a `]` after it (searching from the start for each), take
   the text between them and trim `" \t\n\r\f\v"` from both ends. If that leaves anything:
   - look it up in the generic translation table (the same `slug(trim(...))` lookup as bio-name step 2), and use the
     English key unchanged if it is found;
   - otherwise use `slug(tag)`;

   and render `characters\<that>_generic`. "Boren Vallkirson [Courier]" tries `characters\courier_generic`. A
   result that renders non-empty is returned.
2. **The whole name.** The same again for the entire display name, with no bracket extraction: a generic-table hit
   is used unchanged, otherwise the slug. "Bandit" tries `characters\bandit_generic`.

`slug` is the bio-name slug: ASCII lowercase, then `' '`, `'`, `\` and `/` become `_`. Note that the generic key
carries **no `_<hex>` suffix**.

### 3. `default`

`characters\default` (`Failed to render generic template. Trying default`). Then the decorator gives up
(`Exhausted all attempts to render character profile template`).

### What this means for a reader of the files

- Every bio file on this install is plain blocks. Across all 1,509 base bios and every other layer's
  `prompts/characters/` (about 3,100 more files, dynamic ones included), the only template syntax is
  `{% block %}` / `{% endblock %}`: no `{{ }}`, no `extends`, `include`, `if` or `set`. Pulling block text out of the
  file yields the same words a render would. A content author *could* write template logic into a bio, so this is
  a property of today's content, not a guarantee.
- "Render fails" in practice means "no layer provides the template". A file that exists but renders to nothing
  would also fall through, but nothing like that ships.

## The content store

### The registry

`SKSE/Plugins/SkyrimNet/content-registry.json`, written by SkyrimNet (`ContentRegistry::SaveLocked`):

```json
{
  "schema_version": 1,
  "plugins": {
    "pdusen.skyrim-the-abridged-edition": { "enabled": true, "source": "external" },
    "skyrimnet.bios-cutting-room-floor": { "enabled": true, "enabled_explicit": true, "source": "hub" },
    "pdusen.skyrim-but-its-p": { "enabled": true, "missing": true, "source": "external" }
  },
  "priority": ["pdusen.skyrim-the-abridged-edition", "…", "galanx.intelengine"],
  "pins": { "prompts/characters/danica_pure-spring_69F.prompt": "pdusen.skyrim-the-abridged-edition" },
  "items": {}
}
```

- **`priority`** ranks plugin layers. **The first entry is the highest.**
- **`pins`** is the dashboard's version choice: a content path mapped to the plugin id chosen to provide it.
- **`items`** holds activation overrides, keyed by content path. They govern content-library *records* (actions,
  triggers, virtual entities, knowledge). No activation test was found in the template index that profile lookup
  goes through.
- **`source`** is `external` (a mod's own `external/<id>/`), or `hub` / `local` (a store copy under
  `library/<id>/`).

How `ContentRegistry::ApplyJsonLocked` treats bad input, from its log strings: a non-boolean `enabled` counts as
enabled; a missing or unusable `source` counts as `hub`; a plugin with no `priority` entry is appended at the
bottom; non-string, duplicate or unknown priority entries are dropped; ids that collide when case-folded are
dropped. An unreadable or unsupported file is quarantined and the registry starts empty.

### Layers

SkyrimNet's boot log names every layer and its kind:

```text
[ContentLayer] Scanned base layer 'skyrimnet.base' at …\SkyrimNet\library\skyrimnet.base: 1661 entries
[ContentLayer] Scanned library layer 'skyrimnet.bios-cutting-room-floor' at …\SkyrimNet\library\…: 12 entries
[ContentLayer] Scanned external layer 'pdusen.skyrim-the-abridged-edition' at …\SkyrimNet\external\…: 3063 entries
[ContentLayer] Scanned overlay layer 'overlay' at …\SkyrimNet\overlay: 5 entries
[ContentLayer] Scanned save layer '1778871594119-52513' at …\SkyrimNet\saves\1778871594119-52513: 2 entries
```

| Kind (enum) | Folder                    | Notes                                                                      |
| ----------- | ------------------------- | -------------------------------------------------------------------------- |
| base (0)    | `library/skyrimnet.base/` | SkyrimNet's shipped defaults                                               |
| library (1) | `library/<id>/`           | ignored unless the folder name is its manifest id and the registry owns it |
| external (2)| `external/<id>/`          | a mod's own content folder                                                 |
| overlay (3) | `overlay/`                | the player's dashboard edits                                               |
| save (4)    | `saves/<save id>/`        | this playthrough's own copies; **scoped to `prompts/characters/` only**    |

The save layer exists only while a save is active (`Active save is now '…'` forces a new snapshot), so an actor
resolves differently at the main menu than in game. The loose `prompts/characters/` of the betas before 25 is
**not** a layer: it is absent from the list above even though it still holds 3,113 files on this install.

### Building the stack (`Content::BuildLayerStack`)

The stack is an ordered list, assembled in four groups:

1. **Save and overlay.** Kinds 3 and 4, `stable_sort`ed so the save layer comes first.
2. **Plugins with a `priority` entry.** Each plugin layer is looked up in the registry:
   - a layer with **no registry record is not resolved at all** (`Layer '{}' has no registry record; not resolving
     it`);
   - a disabled (`enabled: false`) or `missing` plugin is flagged and left out;
   - the rest are `std::sort`ed by their index in `priority`, ascending, so `priority[0]` comes first.
3. **Registered plugins with no `priority` entry**, sorted by case-folded layer id.
4. **`skyrimnet.base`**, last (`No skyrimnet.base layer participates …` when it is absent).

One id present in two places resolves **once** (`Plugin '{}' v{} in '{}' is superseded by …`). The rule is
`sub_9f8ba0`, asking whether a later copy replaces the one already held:

1. **The higher manifest version wins.** A copy with no parseable version counts as 0.0.0.
2. **On a tie, location decides:** a `library/` copy beats an `external/` one, which beats anything else.
3. **Then the plain base folder beats `library/skyrimnet.base.update`.** That is a reserved folder SkyrimNet's
   content updater installs newer shipped defaults into. It carries the id `skyrimnet.base` too, so the newer of the
   two is the base layer.
4. **Then the copy discovered first stays.**

A library folder must carry its own name as its manifest id (compared case-insensitively). A reserved base folder
must carry `skyrimnet.base`, and no other folder may claim it. Folders failing either check are ignored, as is a
library folder with no registry record.

At boot SkyrimNet registers any external folders it finds before resolving (`… external registered`), and a new
record gets a priority entry appended at the bottom. So in practice group 3 is nearly always empty, and "no
registry record" only affects someone reading the registry before SkyrimNet has updated it.

### Choosing each path's winner (`Content::ResolveSnapshot`)

Every layer's entries are sorted by path, and the snapshot is built by a k-way merge over them. For each path, the
candidates are the layers that provide it, **in stack order**, and a selector picks one:

1. Look the path up in `pins`, exactly: paths are byte-compared, so `grelka_0b3.prompt` is not `grelka_0B3.prompt`.
   If there is a pin, scan the candidates for the layer whose id equals it after trimming whitespace, again exactly.
   Only the *registry* folds case, when it drops a second pin or record that differs from an earlier one only in
   case.
2. Default to the **first** candidate.
3. **If that first candidate is the save layer, it wins, whatever the pin says.** Otherwise, if the pin matched a
   candidate, that candidate wins.

The selector records which case applied: 0 = no pin, 1 = the pin named the top layer anyway, 2 = the pin overrode
the top layer, 3 = there is a pin but the pinned plugin does not provide the file, so stack order decided.

`ContentStore::ClearPinAfterWriteLocked` fits this. Every overlay write (`WriteFile`, `WriteFiles`) and save-layer
write (`WriteSaveFile`) clears any pin on the path it wrote, because otherwise the pin would keep the player's own
overlay edit from showing. A save-layer write would win anyway; clearing the pin there is just tidying up.

The result is a vector of resolved entries sorted by path, with an active flag on each. Template names map to
paths by adding `prompts/` and `.prompt` (`characters/default` is `prompts/characters/default.prompt`); a dynamic bio
is `.dynamic.prompt`. The `prompts/` prefix and the extension are matched ignoring ASCII case, but the rest of the key
keeps the case on disk. Where two paths collide on one template key, the lower-sorting path wins.

### Translation CSVs

`PromptNameTranslation` loads `prompts/translation/unique/*.csv` and `prompts/translation/generic/*.csv` by
binary-searching the resolved snapshot for each prefix and taking the **active** `.csv` entries **in path order**.
Consequences:

- Each filename resolves through the stack like any other path, so a plugin that ships a file with the **same name**
  as the base one (`00_SkyrimNet_unique.csv`) replaces that whole file.
- Files with **different names** from different layers are all loaded, in byte-wise path order, and a later row
  **replaces** an earlier one with the same key (`Overriding unique translation {} -> {} (was {}) from {}:{}`). So a
  plugin's `50_mymod.csv` overrides individual rows of `00_SkyrimNet_unique.csv`.

The reader is rapidcsv, with the header as row 0, `,` separators and `"` quoting, reading columns by position:

- **Skipped rows:** a row whose trimmed plugin cell is empty or starts with `#`, and a row whose key is empty after
  trimming.
- **Ids:** an id may carry `0x`, and must then be one to eight hex digits.
- **The unique mask:** a unique row's id is masked to twelve bits when its plugin is **loaded** as a light plugin
  (the data handler's `TESFile`, flag bit 9), and to twenty-four otherwise, including when the plugin is not loaded.
- **Generic ids** go to `TESDataHandler::LookupFormID` as written.

On this install only `skyrimnet.base` ships translation CSVs.

## Corroboration

`logs/openrouter_input*.log` records every rendered prompt, profiles included. For each bio that exists in more than
one layer with different text, a passage unique to each version was searched for in 17.6 M characters of those
logs:

| Layers holding the file                                                    | Rendered          | Cases |
| -------------------------------------------------------------------------- | ----------------- | ----- |
| external `skyrim-the-abridged-edition` + base                              | the external copy | 311   |
| external `skyrim-the-abridged-edition` + library `bios-cutting-room-floor` | the external copy | 4     |

The external layer is `priority[0]` and the library layer is ninth. Both results match the decoded rule ("first in
the stack wins", "ascending priority"), with no exceptions. The overlay, save-layer and pin rules could not be
corroborated this way: none of the overlay's five files is a bio, neither of the save layer's two dynamic bios
appeared in a logged prompt, and the single pin names the layer that would have won anyway. They rest on the
disassembly alone.
