# Phase 07b — Pivot to MCM Helper

Short follow-up to Phase 07. Same MCM page, different vehicle: replaces
the imperative SkyUI (`SKI_ConfigBase`) implementation with a declarative
MCM Helper (`MCM_ConfigBase`) implementation so hotkey rebinds persist
cross-save instead of per-save.

---

## Why

The original Phase 07 stored the hotkey binding as auto properties on
the `_ne_MCM` Papyrus script, which serialize into the vanilla `.ess`
save. That put the binding in save-scope: each character (and each save
slot) carried its own hotkey; a New Game reset to F7.

MCM Helper stores its values in `Data/MCM/Settings/<ModName>.ini` — one
file for the whole install, outside any save. That matches most players'
expectation ("I bound my hotkey once; I shouldn't have to do it again on
a new character").

The pivot became viable because the MCM SDK Papyrus sources
(`MCM_ConfigBase.psc` and its `SKI_ConfigBase` / `SKI_QuestBase` base
classes) turned out to ship in a separate Nexus download that wasn't
initially installed. Once that SDK was in place, MCM Helper became the
smaller and simpler design.

---

## What changed

### Papyrus (`esp/Source/Scripts/_ne_MCM.psc`)

Collapsed from ~180 lines of imperative UI code (state blocks, key
handlers, modifier bit manipulation, ModEvent packing) to ~20 lines.
The whole script is now:

```papyrus
Scriptname _ne_MCM extends MCM_ConfigBase

Event OnSettingChange(string a_ID)
    SendModEvent("_ne_DashboardHotkeyChanged")
EndEvent
```

MCM Helper drives the page from `config.json`; the only Papyrus work
left is signalling C++ that something changed.

### Declarative page (`statics/MCM/Config/NarrativeEngine/`)

- **`config.json`** — page layout. One page, two columns via
  `cursorFillMode: topToBottom` + `position: 1` on the right-column
  header. Left column: About (name / version / author / description as
  read-only `text` items). Right column: primary keymap + three
  modifier toggles.
- **`settings.ini`** — MCM Helper's default seed values. Matches the
  plugin INI defaults so first-open shows the same F7-no-modifiers
  binding the plugin INI has been enforcing.

MCM Helper's `keymap` control still only captures a single key (same
limitation as SkyUI's — confirmed in prior research), so modifier support
remains three separate `bHotkey{Shift,Ctrl,Alt}` bool INI keys under
`[Dashboard]`.

### C++ Settings (`include/Settings.h`, `src/Settings.cpp`)

- `ApplyMcmOverride()` restored (had been removed in the SkyUI pivot).
  Reads `Data/MCM/Settings/NarrativeEngine.ini`; reassembles the
  bitmask from the three bool keys. Now public so `MCMEventSink` can
  invoke it for live reloads.
- `UpdateDashboardHotkey()` removed. The previous "receive payload
  from ModEvent, write two fields" path no longer applies — MCM
  Helper writes the INI atomically before firing the event, so the
  sink can just call `ApplyMcmOverride()` to re-read.

### MCM ModEvent sink (`src/MCMEventSink.cpp`)

Simplified: no payload parsing. On every `_ne_DashboardHotkeyChanged`
event, marshal to main and call `Settings::ApplyMcmOverride()`. Log the
resulting binding.

### Build wiring

- `setup-mod-folder.ps1` — copies MCM SDK sources into
  `external/MCM/Source/Scripts/`. Purges the now-obsolete
  `external/SkyUI/` copy. Honors `$env:MCM_SDK_DIR` as an override.
- `CMakeLists.txt` and `NarrativeEngine.ppj.in` — swap
  `NE_PAPYRUS_IMPORT_SKYUI` for `NE_PAPYRUS_IMPORT_MCM`. The MCM SDK
  bundles the SkyUI base scripts too, so a single import path
  suffices.

### ESP work — user side

The Player alias + `SKI_PlayerLoadGameAlias` attachment added in the
original Phase 07 Step 3 is **no longer needed** and should be removed
from `_ne_MCMQuest` in the CK. It existed only to fire `OnGameReload`
so C++ could re-sync from Papyrus save data; MCM Helper's INI is now
authoritative and `Settings::Load` reads it directly at `kDataLoaded`.

The quest form itself stays, with the `_ne_MCM` script still attached.
No other CK changes.

---

## Verification (to redo)

Phase 07's Step 4 verification was performed against the SkyUI-imperative
implementation. The new MCM Helper implementation hasn't been in-game
verified yet. Suggested pass:

1. Boot Skyrim. MCM Helper is installed and picks up
   `Data/MCM/Config/NarrativeEngine/config.json` at startup. The MCM
   list shows "NarrativeEngine".
2. Open the page. Two columns: About on the left, Controls on the
   right (Dashboard Hotkey + three Require-modifier toggles).
3. Rebind. Log shows `MCMEventSink: dashboard hotkey rebound
   DXSC=<N> mods=<M>` promptly after each change.
4. Save, quit to desktop, relaunch, load. The rebinding survives —
   `Settings::Load` reads it from
   `Data/MCM/Settings/NarrativeEngine.ini` at boot.
5. Start a **New Game** (or load a save from a different character).
   The rebinding still applies — the INI is global.
6. Remove `Data/MCM/Settings/NarrativeEngine.ini` (simulate a fresh
   install). Boot; MCM page still opens with the plugin-INI defaults.

---

## What's now unused / removed

- Papyrus imperative MCM code (state blocks, `OnPageReset`,
  `OnKeyMapChangeST`, `ReadHeldModifiers`, `IsModifierKeyCode`,
  `HasModifier` / `SetModifier`, `SendHotkeyChangedEvent` with packed
  payload).
- `external/SkyUI/` SDK copy in the working tree.
- `NE_PAPYRUS_IMPORT_SKYUI` in CMake.
- `Settings::UpdateDashboardHotkey` (and its `UpdateDashboardHotkey`
  path through `MCMEventSink`).
- The Player alias + `SKI_PlayerLoadGameAlias` on `_ne_MCMQuest` (once
  the CK cleanup happens).
