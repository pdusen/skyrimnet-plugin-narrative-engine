# Phase 07 — MCM Configuration Page

Phase 01's MVP shipped a working dashboard behind a
hardcoded-default hotkey (`F7`) configured through the plugin
INI. **Step 17 of Phase 01 (the MCM page) was explicitly
deferred** because MCM Helper requires the consuming mod to
ship an `.esp` carrying a Quest with an attached
`MCM_ConfigBase`-derived Papyrus script, and Phase 01
deliberately kept the mod C++-only. Phases 04 and 05 have
since introduced the `.esp` file, the Papyrus source tree,
and the CK compile pipeline — the tools that made Step 17
uneconomic to build are now in place for their own reasons.

This phase brings up that MCM page in the shape Step 17
sketched: a **single page, two columns**. Left: static mod
credits and identifying information. Right: one control —
the dashboard hotkey rebinder, with modifier-combo support.
No other MCM options. No per-beat toggles, no tuning knobs,
no dev-mode surfaces.

The phase also folds in a small input-space cleanup that
surfaced during Phase 04–05 dev work but wasn't worth
touching in isolation: SKSE input events, SkyUI keymap
controls, and MCM Helper's keymap sink all speak **DirectX
scan codes**, while our current `HotkeySink` translates to
Windows VK codes via `MapVirtualKeyW` before comparing. That
translation is a needless round-trip and, more importantly,
it means an MCM-written value can't be compared directly to
a scan code coming out of a button event. Phase 07 aligns
the C++ side on scan-code space so MCM Helper's output flows
straight through.

---

## Why this phase exists

Concrete pain points, in order:

1. **The dashboard hotkey is unconfigurable in-game.** The
   only way to change it today is to hand-edit
   `Data/SKSE/Plugins/NarrativeEngine.ini` and restart the
   game. That's fine for us during development, but it's a
   dead end the first time anyone else tries the mod — the
   INI file lives inside MO2's virtualization and its exact
   location isn't discoverable from the game.
2. **The mod has no in-game identity.** A player who
   installs the plugin and boots the game has no way to see
   "yes, NarrativeEngine is running, and here's what version
   it is" without opening the log. The MCM page's left
   column solves that at negligible cost.
3. **The MCM plumbing is half-installed.** `Settings::Load`
   already reads `Data/MCM/Settings/NarrativeEngine.ini` as
   an override for the `[Dashboard]` section — the code path
   exists, it just never sees a file because no MCM page
   writes one. Finishing the loop is a small amount of work
   that makes an existing but currently-dead code path do
   something.
4. **Input-space mismatch waiting to happen.** The moment
   the MCM page starts writing `iHotkeyVK`, the C++ side
   will read a DirectX scan code and compare it against a
   VK code translated from another scan code. Cleaner to
   normalize the whole pipeline on scan-code space now —
   before there's a live MCM writer whose values would be
   silently misinterpreted.

---

## Scope

### In scope

- One new **`.esp` quest form** (`_ne_MCMQuest`) carrying an
  attached Papyrus script (`_ne_MCM`) that extends
  `MCM_ConfigBase` — the minimum MCM Helper requires to
  register a mod's MCM page.
- One new **MCM Helper `config.json`** describing the page:
  single sub-page, two columns, static text on the left,
  one `keymap` control on the right.
- One new **translation file**
  (`NarrativeEngine_ENGLISH.txt`) holding every `$NE_...`
  string the `config.json` references.
- A **C++ input-space normalization** step: rename the
  `dashboardHotkey*` fields to name their real content
  (DirectX scan code, SkyUI-convention modifier bitmask),
  rework `HotkeySink` to compare in scan-code space
  directly, update the plugin INI's `[Dashboard]` section
  and its defaults. Documented in **Core concepts →
  Input-space normalization** below.
- Verification that the MCM page appears in SkyUI, that a
  rebind persists across save/reload, and that removing MCM
  Helper falls back cleanly to the plugin INI.

### Deferred (explicitly out)

- **More MCM controls.** No tuning of Director cadence, no
  per-beat enable toggles, no beat-cooldown knobs, no
  debug-mode toggle. Any of those are legitimate future
  additions, but each of them either duplicates the INI
  surface that already exists (per the CLAUDE.md warning
  about MCM globals for every setting) or opens a
  round-trip design question about MCM ↔ INI precedence
  that isn't worth answering for this phase.
- **Multi-page or tabbed MCM.** One page, two columns. If
  the phase runs long, the page can grow; it can't grow
  into multiple pages.
- **MCM-driven dashboard content.** The dashboard's Dispatch
  tab already lets the player toggle beats mid-session. No
  duplicate surface in MCM.
- **A "Reset to defaults" button.** The `keymap` control
  supports a per-binding reset gesture out of the box (long
  press). Nothing more is needed.
- **Translations beyond English.** One `_ENGLISH.txt` file.
  Anyone shipping a localized build later can drop in
  `_FRENCH.txt`, `_GERMAN.txt`, etc. — the schema doesn't
  care.

---

## Core concepts

### MCM Helper as the vehicle

**MCM Helper** (Nightsteed) is the SKSE plugin that lets an
`.esp`+Papyrus MCM page be described declaratively by a
JSON schema instead of built out imperatively in Papyrus.
The Papyrus footprint is a boilerplate script whose only job
is to extend `MCM_ConfigBase` and be attached to a quest
form; MCM Helper reads its `Data/MCM/Config/<ModName>/config.json`
at runtime and drives the page from the JSON. Live values
land in `Data/MCM/Settings/<ModName>.ini`, which the C++
side already reads via `Settings::ApplyMcmOverride`.

The alternative — an imperative Papyrus MCM script that
extends `SKI_ConfigBase` and calls
`AddKeyMapOptionST` / `AddTextOptionST` / etc. directly —
works but is harder to modify: every layout change is a
Papyrus rebuild, and the modifier-key handling in
particular becomes a per-mod re-implementation. MCM Helper
carries that logic for us.

MCM Helper is a **soft dependency**. If it's not installed,
the page doesn't appear, no INI is written under
`Data/MCM/Settings/`, and the plugin INI's `[Dashboard]`
values (or their baked-in defaults) stand. That fallback is
already wired.

### Two-column layout

MCM Helper's schema supports column layout via `groupBehavior`
markers on option entries, letting the page split its
option flow at a chosen point. Our page uses:

- **Left column** — a `groupBehavior: "click"` header
  followed by a `text` block containing the mod name,
  version, author, and a one-line description. No inputs.
- **Right column** — a `groupBehavior: "click"` header
  followed by a single `keymap` control whose bound value
  is the dashboard hotkey.

A single sub-page holds both. No tabs. No paging.

### The hotkey control and input-space normalization

**Today's shape (about to change).** SKSE's button events
carry a **DirectX scan code** in `btn->GetIDCode()`. The
current `HotkeySink` translates that scan code to a **Windows
VK code** via `MapVirtualKeyW(sc, MAPVK_VSC_TO_VK)` and
compares it against `Settings::Get().dashboardHotkeyVK`,
which the plugin INI stores in VK space (default `118` ==
`VK_F7`). Modifier state is read via `GetAsyncKeyState(VK_*)`
and compared against a bitmask stored in
`Settings::Get().dashboardHotkeyModifiers`, whose convention
is `1=Ctrl, 2=Shift, 4=Alt`.

**Why it's about to break.** MCM Helper's `keymap` control
stores the primary key as a **DirectX scan code** (Skyrim's
native input space; the same integer SKSE's button event
already carries) and stores its modifier bitmask under
SkyUI's convention: `1=Shift, 2=Ctrl, 4=Alt` (Shift and Ctrl
swapped from ours). If MCM writes to
`iHotkeyVK:Dashboard` we'd be putting a scan code into a
field the C++ side treats as a VK code; if MCM writes to
`iHotkeyModifiers:Dashboard` we'd have Ctrl-vs-Shift
inverted.

**Resolution.** Normalize the C++ side on scan-code space
and on SkyUI modifier convention, because that's what the
runtime authorities we care about (SKSE input events, MCM
Helper) speak natively:

- Rename `dashboardHotkeyVK` → `dashboardHotkeyDXSC`
  (defaults to `65`, the scan code for F7). The INI key
  becomes `iHotkeyDXSC`.
- Rename `dashboardHotkeyModifiers` bitmask constants:
  `kModShift = 1; kModCtrl = 2; kModAlt = 4;` — same values
  reshuffled to which key they attach to, matching SkyUI /
  MCM Helper. The stored default (`0`) is unchanged.
- `HotkeySink::ProcessEvent` drops the `MapVirtualKeyW`
  call and compares `btn->GetIDCode()` directly against
  `dashboardHotkeyDXSC`.
- Modifier reads switch from `GetAsyncKeyState(VK_CONTROL)`
  etc. to the equivalent DirectInput scan-code checks on
  the sink's own held-keys state — or, more simply, keep
  the `GetAsyncKeyState` reads and remap them into the new
  convention as they land in the bitmask. `GetAsyncKeyState`
  is a Windows API and stays in VK space; only the bitmask
  the result is packed into changes convention.
- ESC-closes-dashboard branch keeps its `VK_ESCAPE`
  comparison — but on the scan-code side that's `1`
  (`DIK_ESCAPE`). Substitute the constant.
- The plugin INI's `[Dashboard]` block updates its default
  values, its key names, and its comments to match the new
  space. Values in flight from the old key names are dropped
  (there's no persistence — the INI is user-owned config
  text, and any misalignment from a stale INI edit will show
  up as a mis-firing hotkey, not a crash).

This is a two-hour C++ refactor. It doesn't change the
default hotkey (still F7 with no modifiers); it changes what
integers we compare and where they live.

### INI precedence and file locations

`Settings::Load` already implements the desired precedence.
Read order:

1. `Data/SKSE/Plugins/NarrativeEngine.ini` (plugin INI —
   dev / author defaults; the source of truth for every
   setting except `[Dashboard]` in the presence of MCM).
2. `Data/MCM/Settings/NarrativeEngine.ini` (MCM Helper's
   output — overrides `[Dashboard]` keys if the file
   exists).

Both files live under `Data/` and are subject to MO2 VFS.
The MCM output file only exists once a player interacts with
the MCM page — MCM Helper doesn't write it eagerly. On a
fresh install with the page opened but no rebind clicked,
the file may still not exist. That's fine: `ApplyMcmOverride`
already handles missing-file by leaving prior values in
place.

### Boilerplate Papyrus footprint

The minimum viable `_ne_MCM` script:

```papyrus
Scriptname _ne_MCM extends MCM_ConfigBase
```

That's the whole script. `MCM_ConfigBase` extends
`SKI_ConfigBase` and provides the hooks that let MCM Helper
drive the page. No properties, no events, no functions — the
boilerplate exists purely so the CK-attached script is
present and MCM Helper's registration finds the quest.

The `_ne_MCMQuest` form is a start-game-enabled quest with
priority `0`, no stages, no aliases. Its only role is to
carry the `_ne_MCM` script.

---

## Settings

**Renamed keys (semantic realignment; not a compatibility
break because these values are user-owned INI text with no
persisted state behind them):**

- `dashboardHotkeyVK` → `dashboardHotkeyDXSC`
- INI: `[Dashboard] iHotkeyVK` → `[Dashboard] iHotkeyDXSC`
- Default value: `118` (VK_F7) → `65` (DIK_F7)

**Semantic realignment on the modifier bitmask
(same field name, same INI key, reshuffled constants to
match SkyUI):**

- `kModShift = 1` (was `2`)
- `kModCtrl  = 2` (was `1`)
- `kModAlt   = 4` (unchanged)

Values that were `0` or `4` in the old scheme translate
identically; `1` and `2` swap meaning. A user with a stale
`iHotkeyModifiers=1` in their plugin INI will see their
"required Ctrl" turn into "required Shift" after the
refactor. This affects exactly one person (the author, on
their dev machine, if they had modifiers configured); a
single INI edit resolves it.

**Unchanged:**

- Every other Settings field. This phase touches nothing
  outside `[Dashboard]`.

---

## Persistence

Nothing new in the SKSE cosave. MCM Helper handles its own
persistence to `Data/MCM/Settings/NarrativeEngine.ini`, and
the C++ side reads that INI at `Settings::Load` time on
plugin init. No new cosave record. No changes to any
existing cosave record.

The `_ne_MCMQuest` quest form is a start-game-enabled quest
with no stages and no aliases, so its Papyrus save footprint
is minimal (the quest itself is persistent by virtue of
being started; there's nothing on the script for the save to
carry).

---

## File map

New:

- `esp/Source/Scripts/_ne_MCM.psc` — the two-line boilerplate
  Papyrus script.
- `statics/MCM/Config/NarrativeEngine/config.json` — MCM
  Helper page schema.
- `statics/MCM/Config/NarrativeEngine/settings.ini` — MCM
  Helper default-seed INI (initial values written to
  `Data/MCM/Settings/NarrativeEngine.ini` on first page
  open).
- `statics/Interface/Translations/NarrativeEngine_ENGLISH.txt`
  — localization strings (`$NE_MCM_ModName`, etc.) referenced
  from `config.json`.

Reshaped (in place, no rename):

- `include/Settings.h` — rename `dashboardHotkeyVK` →
  `dashboardHotkeyDXSC`; reshuffle `kMod*` constants to
  SkyUI convention; update inline comments and file header.
- `src/Settings.cpp` — update INI reads for the renamed key
  in both `LoadPluginIni` and `ApplyMcmOverride`; update
  the resolved-hotkey log line.
- `src/DashboardUIManager.cpp` — rewrite `HotkeySink` to
  compare scan codes directly, remap `GetAsyncKeyState`
  results into the new bitmask convention, replace the
  `VK_ESCAPE` shortcut with `DIK_ESCAPE`. Update the init
  log to name the field `DXSC=` instead of `VK=`.
- `statics/SKSE/Plugins/NarrativeEngine.ini` — rename the
  `[Dashboard]` block's `iHotkeyVK` key to `iHotkeyDXSC`,
  update its default value from `118` to `65`, update the
  block comment to describe DirectX scan codes and the
  SkyUI-convention modifier bitmask.

CK / ESP work:

- `_ne_MCMQuest` — new start-game-enabled quest form, no
  stages, no aliases, attached `_ne_MCM` script.

Preserved unchanged:

- Every other C++ file (no other consumer of the renamed
  Settings fields exists).
- Every other Papyrus script and quest.
- The dashboard TS/React source.

---

## Implementation plan

Sequential. Steps 1 and 3 are entirely Claude's work; Step 2
is entirely the user's CK/Papyrus work; Step 4 is a joint
verification.

Step order:

1. **C++ input-space normalization** — the Settings /
   HotkeySink / INI refactor. Lands independently of MCM
   Helper and takes effect for the current INI-only
   configuration path. Verifiable without any CK or MCM
   work: the default F7 hotkey still opens the dashboard;
   changing `iHotkeyDXSC` in the plugin INI changes the
   bind.
2. **CK / ESP: MCM quest form + Papyrus script** — author
   `_ne_MCM.psc`, create `_ne_MCMQuest` in the CK, attach
   the script, compile. Nothing user-visible yet; MCM
   Helper needs the `config.json` from Step 3 to actually
   render a page.
3. **MCM Helper JSON + translation file** — author
   `config.json`, `settings.ini`, and
   `NarrativeEngine_ENGLISH.txt` under `statics/`. Rebuild
   so the files land in the runtime mod folder. With Step 2
   already done, the MCM page appears at Skyrim's next
   launch.
4. **Verify end-to-end** — walk the four verification
   scenarios below (page visible, rebind persists, MCM
   absent falls back, INI absent falls back).

---

### Step 1 — C++ input-space normalization

- [x] Complete

**[CLAUDE]**

**Goal:** Move the C++ hotkey pipeline from Windows-VK space
to DirectX-scan-code space, and align the modifier bitmask
with SkyUI's convention. Prepares the Settings surface for
MCM Helper's output to land directly.

**Files:**

- `include/Settings.h` — field rename + constant reshuffle.
- `src/Settings.cpp` — INI-read updates.
- `src/DashboardUIManager.cpp` — `HotkeySink` rewrite.
- `statics/SKSE/Plugins/NarrativeEngine.ini` — key rename,
  default change, comment refresh. (Under `statics/`;
  requires `build.ps1 build` to reach the runtime mod
  folder.)

**Sub-tasks:**

1. In `include/Settings.h`:
   - Rename `int dashboardHotkeyVK` → `int dashboardHotkeyDXSC`.
     Default value: `65` (`DIK_F7`).
   - Renumber the `kMod*` inline constants to
     `kModShift = 1; kModCtrl = 2; kModAlt = 4;`.
   - Update the block comments describing what these
     values mean; call out explicitly that this is
     SkyUI / MCM Helper convention.
   - Update the file header comment describing the
     precedence chain so the `[Dashboard]` note names
     `iHotkeyDXSC` instead of `iHotkeyVK`.
2. In `src/Settings.cpp`, both `LoadPluginIni` and
   `ApplyMcmOverride`:
   - Read `[Dashboard] iHotkeyDXSC` into
     `dashboardHotkeyDXSC`. Drop the old `iHotkeyVK`
     read entirely.
   - Update the resolved-hotkey log line to
     `Settings: dashboard hotkey DXSC=<N> mods=<M>`.
3. In `src/DashboardUIManager.cpp`'s `HotkeySink`:
   - Remove the `MapVirtualKeyW` translation. Read
     `const std::uint32_t dxsc = btn->GetIDCode();` and
     compare directly against
     `Settings::Get().dashboardHotkeyDXSC`.
   - Replace `VK_ESCAPE` with a `constexpr std::uint32_t
     kDIK_ESCAPE = 1;` and compare against `dxsc`.
   - Keep the modifier reads on `GetAsyncKeyState(VK_CONTROL
     / VK_SHIFT / VK_MENU)` — these are Windows API and
     natively speak VK — but assemble the result into the
     new bitmask convention: Shift bit set from
     `VK_SHIFT`, Ctrl bit from `VK_CONTROL`, Alt from
     `VK_MENU`.
   - Update the init log to name the field `DXSC=` instead
     of `VK=`.
4. In `statics/SKSE/Plugins/NarrativeEngine.ini`:
   - Rename the `[Dashboard]` block's `iHotkeyVK` key to
     `iHotkeyDXSC`. Change the default from `118` to `65`.
   - Rewrite the block comment to describe DirectX scan
     codes, not VK codes. Give a couple of common
     examples (F7=65, backslash-key=43, so on).
   - Rewrite the `iHotkeyModifiers` comment to show the
     new bitmask convention (`1=Shift, 2=Ctrl, 4=Alt`).
5. `pwsh -File format.ps1` to sweep formatting.

**Specifics:**

- The old plugin-INI key `iHotkeyVK` is not preserved as an
  alias. If the author has a hand-edited value there,
  they'll notice on the next boot that the default F7 is
  back and update the INI. This is a dev-branch refactor;
  no downstream users to break.
- No changes to the `PrismaUI`-side wiring — the dashboard
  is toggled by `ToggleVisibility` which is state-agnostic.
  Only the input-side comparison shape changes.

**Verify:**

- `pwsh -File build.ps1 build` succeeds cleanly.
- Boot Skyrim. Log shows
  `Settings: dashboard hotkey DXSC=65 mods=0`. Press F7;
  dashboard toggles.
- Change `iHotkeyDXSC=87` in the plugin INI (W key),
  rebuild, reboot. Log shows DXSC=87. Press F7 — nothing
  happens. Press W — dashboard toggles. (Pick a key that's
  not bound to movement or reset it before continuing.)
- Change `iHotkeyModifiers=1` in the plugin INI (SkyUI
  Shift convention), rebuild, reboot. Press F7 — nothing.
  Hold Shift and press F7 — dashboard toggles. Change to
  `iHotkeyModifiers=2` (Ctrl) and reverify.
- Revert INI changes.

---

### Step 2 — CK / ESP: MCM quest form + Papyrus script

- [ ] Complete

**[USER]**

**Goal:** Add the `.esp`-side machinery MCM Helper needs to
find our mod: one start-game-enabled quest form carrying an
attached Papyrus script that extends `MCM_ConfigBase`. The
script has no logic of its own — the boilerplate exists
only to satisfy MCM Helper's discovery contract.

**Files:**

- `esp/Source/Scripts/_ne_MCM.psc` — new. Two lines:
  scriptname declaration + `extends MCM_ConfigBase`.
- `_ne_MCMQuest` — new quest form in the CK.

**Sub-tasks:**

1. Author `esp/Source/Scripts/_ne_MCM.psc` with exactly:

    ```papyrus
    Scriptname _ne_MCM extends MCM_ConfigBase
    ```

    Save. The Papyrus compile pipeline (`build.ps1 build`)
    will pick it up on the next Claude-side build.

2. In the Creation Kit, open `NarrativeEngine.esp`.
3. Create a new Quest form. EditorID: `_ne_MCMQuest`. Set
    "Start Game Enabled" checked; priority `0`; no aliases;
    no stages; no dialogue.
4. On the Quest's "Scripts" tab, add the `_ne_MCM` script.
    Save the ESP.
5. Verify the CK's Papyrus compile of `_ne_MCM` succeeds
    against MCM Helper's `MCM_ConfigBase.psc` sources on
    disk. If the compile fails with a "cannot find
    MCM_ConfigBase" error, MCM Helper's source scripts
    aren't in the CK's script sources path — the fix is to
    add MCM Helper's `Scripts/Source` directory to the CK's
    script source paths (or copy the `.psc` files into the
    build tree), not to change our script.

**Verify:**

- CK save succeeds.
- `pwsh -File build.ps1 build` succeeds (Papyrus + C++
  both).
- Boot Skyrim without MCM Helper's page contribution (i.e.
  no `config.json` on disk yet — Step 3 hasn't run). SkyUI
  should not show a NarrativeEngine entry in the MCM list.
  This confirms that the quest-only footprint is inert
  until the `config.json` lands.

---

### Step 3 — MCM Helper JSON + translation file

- [ ] Complete

**[CLAUDE]**

**Goal:** Ship the declarative page description. With Steps
1 and 2 done, this is the file that turns "no MCM entry" into
"a fully working NarrativeEngine MCM page."

**Files:**

- `statics/MCM/Config/NarrativeEngine/config.json` — new.
  Page schema.
- `statics/MCM/Config/NarrativeEngine/settings.ini` — new.
  Seed values for MCM Helper.
- `statics/Interface/Translations/NarrativeEngine_ENGLISH.txt`
  — new. `$NE_MCM_*` localization strings.

**Sub-tasks:**

1. Author `config.json` with this shape (adjust the exact
   MCM Helper schema keys per the current MCM Helper wiki;
   the shape below reflects the schema at the time of
   writing):

    ```jsonc
    {
      "modName": "NarrativeEngine",
      "displayName": "$NE_MCM_ModName",
      "minMcmVersion": 1,
      "pages": [
        {
          "pageDisplayName": "",
          "content": [
            {
              "type": "header",
              "text": "$NE_MCM_About",
              "position": 0
            },
            {
              "type": "text",
              "text": "$NE_MCM_AboutBody",
              "position": 0
            },
            {
              "type": "header",
              "text": "$NE_MCM_Controls",
              "position": 1
            },
            {
              "type": "keymap",
              "text": "$NE_MCM_DashboardHotkey",
              "help": "$NE_MCM_DashboardHotkey_Help",
              "position": 1,
              "valueOptions": {
                "sourceType": "ModSettingInt",
                "sourceName": "iHotkeyDXSC:Dashboard",
                "modifierSourceName": "iHotkeyModifiers:Dashboard"
              }
            }
          ]
        }
      ]
    }
    ```

    Two-column layout: `position: 0` on the About header +
    text places them in the left column;
    `position: 1` on the Controls header + keymap places
    them in the right column. This matches MCM Helper's
    two-column convention.

2. Author `settings.ini` with the initial default values:

    ```ini
    [Dashboard]
    iHotkeyDXSC=65
    iHotkeyModifiers=0
    ```

    MCM Helper copies these into
    `Data/MCM/Settings/NarrativeEngine.ini` on first page
    open. They match the defaults baked into `Settings.h`
    so there's no visible change from first-run.

3. Author `NarrativeEngine_ENGLISH.txt` with the exact
    strings the `config.json` references:

    ```text
    $NE_MCM_ModName NarrativeEngine
    $NE_MCM_About About
    $NE_MCM_AboutBody An AI Director for Skyrim, layered on top of SkyrimNet.\n\nVersion: 0.1.0 (dev)\nAuthor: Patrick VanDusen\n
    $NE_MCM_Controls Controls
    $NE_MCM_DashboardHotkey Dashboard Hotkey
    $NE_MCM_DashboardHotkey_Help Press a key (and optional modifiers) to bind the in-game dashboard toggle. Long-press to clear.
    ```

    Skyrim's translation-file format uses tab separators
    between key and value; `\n` is honored as a line break
    in text options. Save the file as UTF-16 LE with BOM
    (Skyrim's translation loader is strict about the
    encoding — this is a common first-time gotcha).

4. `pwsh -File format.ps1`. Prettier will run on the JSON;
    the `.txt` and `.ini` may be skipped by the hook set,
    which is fine.

**Specifics:**

- The `position` field is the standard MCM Helper mechanism
  for the two-column layout: options with `position: 0`
  flow into the left column in declaration order; options
  with `position: 1` flow into the right column in
  declaration order. If the current MCM Helper schema uses
  a different key name (`column`, `side`, etc.), adjust
  the JSON — the semantics we want is "About goes left,
  Controls goes right, one page, no tabs."
- The `keymap` control's `valueOptions.modifierSourceName`
  is what MCM Helper uses to store the modifier bitmask
  separately from the main key. If the current schema
  doesn't support `modifierSourceName`, fall back to a
  three-checkbox layout for Ctrl/Shift/Alt as sketched in
  Phase 01 Step 17 — but verify the schema first;
  modifier support has been in MCM Helper for a while.
- The translation file's `$NE_MCM_AboutBody` string is
  where the mod-identifying information lives: name,
  version, author, one-line description. Update the version
  string whenever the mod bumps.

**Verify:**

- `pwsh -File build.ps1 build` succeeds; the JSON, INI, and
  translation file land under
  `.../mods/NarrativeEngine/MCM/Config/NarrativeEngine/`
  and `.../mods/NarrativeEngine/Interface/Translations/`.
- Boot Skyrim with MCM Helper installed. Wait ~30 seconds
  after the main menu for MCM to finish registering pages.
- Open MCM (via SkyUI). A "NarrativeEngine" entry appears
  in the mod list. Open it.
- Verify the two-column layout: left shows "About" +
  identifying text; right shows "Controls" + one row
  labeled "Dashboard Hotkey" with the current bind
  (default: `F7`) displayed.
- Click the hotkey row. Press a new key combination (e.g.
  Ctrl+Shift+`8`). The row updates to show the new bind.
- Exit MCM. Log shows a subsequent
  `Settings: dashboard hotkey DXSC=<N> mods=<M>` line
  reflecting the new bind. (The line is emitted on next
  `Settings::Load`, i.e. next game load — see Step 4's
  reload verification.)

---

### Step 4 — End-to-end verification

- [ ] Complete

**[CLAUDE + USER]**

**Goal:** Walk the four scenarios that together prove the
MCM page works and the fallback chain is intact.

**Sub-tasks:**

1. **Page visible + rebind persists across save/reload.**
   - Boot Skyrim with MCM Helper installed, MCM page from
     Step 3 in place.
   - Open MCM → NarrativeEngine. Verify layout (per Step
     3 verify).
   - Rebind the dashboard hotkey to Ctrl+`8`.
   - Save the game. Exit to main menu. Reload the save.
   - Log shows `Settings: dashboard hotkey DXSC=9 mods=2`
     (or the appropriate values for whatever combo was
     chosen).
   - Press Ctrl+`8`. Dashboard toggles. Press F7. Nothing
     happens (old default is now overridden).

2. **MCM Helper absent falls back to plugin INI.**
   - Disable MCM Helper in MO2. Boot Skyrim.
   - Log shows `Settings: no MCM override at
     Data/MCM/Settings/NarrativeEngine.ini` (or
     equivalent existing message from `ApplyMcmOverride`
     failure branch — verify the current log wording).
   - Log shows the plugin-INI-configured hotkey.
   - Press the plugin-INI-configured hotkey. Dashboard
     toggles. The MCM entry is absent from SkyUI's list.
   - Re-enable MCM Helper for subsequent steps.

3. **Fresh install, MCM not yet opened.** *(Simulates a
   first-time user's experience.)*
   - Delete
     `Data/MCM/Settings/NarrativeEngine.ini` (or the
     equivalent MO2-Overwrite location where MCM Helper
     writes it).
   - Boot Skyrim. Log shows the fallback line (no MCM
     override applied); hotkey resolves to the plugin
     INI's `iHotkeyDXSC` value.
   - Press F7 (or the INI's configured hotkey). Dashboard
     toggles.
   - Open MCM. The page appears with default bindings from
     the `settings.ini` seed. No user rebind needed for
     the page to be functional.

4. **Grep sweep.**
   - `grep -RIn "dashboardHotkeyVK\|iHotkeyVK\|VK_ESCAPE\|MapVirtualKeyW"
     src include statics` returns no matches (all removed
     in Step 1). Phase 01 Step 17's mention of `iHotkeyVK`
     in `PHASE_01_MVP.md` is expected historical and
     preserved.

**Verify:** all four sub-tasks pass; both checkboxes in
Steps 1–3 flip to complete.

---

## Done condition

- All four steps above have their checkboxes marked
  complete.
- The MCM page appears in SkyUI when MCM Helper is
  installed; its layout is two columns as designed; the
  keymap control accepts rebinds including modifier combos.
- A rebind persists across save/reload via MCM Helper's INI
  output.
- Removing MCM Helper falls back to the plugin INI cleanly;
  removing the plugin INI too falls back to the baked-in
  defaults (F7, no modifiers).
- `pwsh -File build.ps1 build` succeeds cleanly.
- `pwsh -File format.ps1` succeeds cleanly.
- No source or config file references `dashboardHotkeyVK`,
  `iHotkeyVK`, or `MapVirtualKeyW` (matches in
  `docs/implementation/PHASE_01_MVP.md` are expected and
  preserved as historical).
- The `_ne_MCMQuest` quest is present in the ESP with its
  attached `_ne_MCM` script; the script compiles cleanly
  in the CK against MCM Helper's `MCM_ConfigBase`.
