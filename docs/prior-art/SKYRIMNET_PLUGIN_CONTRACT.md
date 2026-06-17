# The SkyrimNet Plugin Extension Surface

> **Most directly relevant document for NarrativeEngine.** NarrativeEngine will be a SkyrimNet plugin, so it has to
> satisfy the same contract IntelEngine satisfies. Everything below is reverse-engineered from IntelEngine's plugin
> assets, not from SkyrimNet's own (undocumented locally) source. Treat it as descriptive of *one working integration*,
> not as a normative spec.

---

## The on-disk tree

A SkyrimNet plugin ships its extension assets at a fixed location under the player's `Data/SKSE/Plugins/SkyrimNet/`
tree. IntelEngine's layout:

```text
SKSE/Plugins/SkyrimNet/
├── config/
│   ├── actions/                       # LLM-facing tool definitions
│   │   ├── cat_<category>.yaml        # Umbrella category entries the LLM picks first
│   │   └── <plugin>_<action>.yaml     # Per-action definitions
│   └── plugins/<PluginName>/          # Per-plugin config namespace
│       ├── manifest.yaml              # plugin metadata, LLM variants, MCM-like schema
│       ├── settings.sample.yaml       # default user settings to copy/override
│       └── <other>.sample.yaml        # any other user-editable data files
└── prompts/
    ├── <plugin>_<name>.prompt         # top-level Jinja2 prompt templates ("DM" prompts)
    └── submodules/
        └── character_bio/
            └── NNNN_<plugin>_<name>.prompt   # numbered bio fragments injected per NPC turn
```

All YAML and `.prompt` files are hot-loaded by SkyrimNet — the SKSE plugin doesn't have to register them in code. The
SKSE plugin's job is the *executable* side (Papyrus functions the actions call, decorators the eligibility rules and
prompts use, ModEvents the runtime fires).

---

## Action YAMLs

Each action is an LLM-callable tool. When SkyrimNet runs its dialogue cycle for an NPC, it presents all registered
actions (across all installed plugins) and the LLM picks one.

### Example: `intel_travel.yaml` (annotated)

```yaml
customCategory: intel_travel             # Which cat_*.yaml umbrella this belongs to
name: GoToLocation                       # Display name the LLM sees
description: >
  Leave immediately and travel to a specific place.

  TRIGGER WORDS that indicate travel intent:
  "go to", "head to", "walk to", "travel to", ...

  DO NOT USE THIS ACTION when the player asks you to deliver a message ...
  Use DeliverMessage instead. Examples that are NOT travel: ...

  DESTINATION must be one of these two categories:
  A) SEMANTIC KEYWORDS — use ONLY these exact words ...
  B) NAMED SKYRIM LOCATIONS — any real location that exists ...

  INVALID destinations (NEVER use these): ...
  WHEN NOT TO USE: ...

questEditorId: IntelEngine               # Which quest record holds the script
scriptName: IntelEngine_Travel           # Which Papyrus script class
executionFunctionName: GoToLocation      # Which function on that script to call

parameterMapping:
  - type: speaker                        # bound to the speaking NPC
    name: akNPC
    description: You (the NPC traveling)
  - type: dynamic                        # LLM fills in
    name: destination
    description: >
      Where to travel. Use ONLY: ...
  - type: dynamic
    name: speed
    description: >
      Travel speed: 0=walk, 1=jog, 2=run.
      Casual=0, normal=1, urgent=2
  - type: dynamic
    name: waitForPlayer
    description: >
      1 if the player asked or told you to go there.
      0 if you decided to go on your own.
  - type: static                         # hardcoded constant
    value: false
    name: isScheduled

eligibilityRules:                        # gates whether the action is even offered
  - conditions:
      - decoratorName: is_in_combat
        arguments: ["currentActor"]
        comparisonOperator: "=="
        expectedValue: false
      - decoratorName: is_busy
        arguments: ["currentActor"]
        comparisonOperator: "=="
        expectedValue: false
    logicalOperator: "AND"
    required: true

enabled: true                            # toggle without removing the file
defaultPriority: 5
tags: []
```

### Key contract details to internalize

- **`description` is prompt engineering.** The LLM reads it as instruction. IntelEngine's descriptions go hard on
  positive examples, negative examples, anti-patterns, "WHEN NOT TO USE", and explicit confusion warnings ("DO NOT USE
  this when X — use Y instead"). This is the primary lever to keep the LLM from misfiring an action.
- **`parameterMapping` types:**
  - `speaker` — auto-bound to the speaking NPC's actor reference
  - `dynamic` — LLM fills the value; each has its own description for the LLM
  - `static` — a hardcoded constant passed every time
- **`executionFunctionName`** is dispatched into Papyrus: SkyrimNet finds the quest record `questEditorId`, gets the
  script `scriptName` attached to it, and calls the function with the bound + LLM-supplied parameters. So **every action
  has a Papyrus function to receive it.**
- **`eligibilityRules` use decorators** — these are functions the SKSE plugin registers with SkyrimNet at startup.
  `is_busy`, `is_in_combat`, and any custom decorators (e.g. `intel_can_accept_task`) all evaluate during action
  presentation. If rules fail, the action is hidden from the LLM that turn.
- **`enabled: true/false`** lets the player toggle actions without uninstalling. IntelEngine's dashboard writes this
  flag back to disk.

### Category YAMLs

Categories are short umbrella tools the LLM picks *first* — a two-level hierarchy that keeps the immediate action menu
small. Example, `cat_travel.yaml`:

```yaml
customCategory: intel_travel
name: Travel
description: "Travel, movement, and tasks — SELECT THIS when {{ npc.name }} decides to:
  go somewhere, head to a location, go home, go outside/inside, lead someone to a
  destination, escort someone, go fetch a person and bring them back, stop a current
  task, cancel what they're doing, hurry up, slow down, or change travel speed."
enabled: true
```

Per-action YAMLs reference their umbrella via the matching `customCategory:` field. The `description:` is
Jinja2-rendered (note `{{ npc.name }}`).

### IntelEngine's action inventory (for reference)

| File                               | Action                     | Category            |
| ---------------------------------- | -------------------------- | ------------------- |
| `intel_travel.yaml`                | GoToLocation               | intel_travel        |
| `intel_fetchnpc.yaml`              | FetchPerson                | intel_travel        |
| `intel_escorttarget.yaml`          | EscortTarget               | intel_travel        |
| `intel_searchforactor.yaml`        | SearchForActor             | intel_travel        |
| `intel_changespeed.yaml`           | ChangeSpeed                | intel_travel        |
| `intel_canceltask.yaml`            | CancelCurrentTask          | intel_travel        |
| `intel_delivermessage.yaml`        | DeliverMessage             | intel_communication |
| `intel_report_player_conduct.yaml` | (player conduct reporting) | intel_communication |
| `intel_schedulemeeting.yaml`       | ScheduleMeeting            | intel_scheduling    |
| `intel_schedulefetch.yaml`         | ScheduleFetch              | intel_scheduling    |
| `intel_scheduledelivery.yaml`      | ScheduleDelivery           | intel_scheduling    |
| `cat_travel.yaml`                  | (category)                 |                     |
| `cat_communication.yaml`           | (category)                 |                     |
| `cat_scheduling.yaml`              | (category)                 |                     |

---

## Manifest

`config/plugins/<PluginName>/manifest.yaml` declares plugin metadata, LLM variants, and a schema of user-editable
settings that SkyrimNet renders into its own settings UI.

### Annotated example (IntelEngine's manifest, abridged)

```yaml
plugin:
  name: IntelEngine
  version: "3.5.0"
  description: "NPC task & story automation engine"
  icon: "\U0001F9E0"

variants:
  - name: intel_story_dm
    description: "Story Dungeon Master decisions — uses base OpenRouter config
      unless overridden below"

schema:
  fields:
    - name: "Dashboard Hotkey"
      path: "ui.dashboard_hotkey"        # dotted path into settings YAML
      type: "hotkey"                     # hotkey | int | float | string | (bool?)
      description: "Main key for dashboard toggle (VK code, e.g., 55 = 7). Set to -1 to disable."
      defaultValue: 55
      min: -1
      max: 255
      category: "UI"                     # tab/grouping in SkyrimNet's UI

    - name: "Faction Blocklist"
      path: "story.faction_blocklist"
      type: "string"
      description: "Comma-separated faction EditorIDs to exclude from story candidates.
        Append :rank to filter by max rank ..."
      defaultValue: ""
      category: "Story Engine"

    # ... LLM overrides have their own category:
    - name: "API Endpoint"
      path: "llm.endpoint"
      type: "string"
      description: "Custom API endpoint URL ..."
      defaultValue: ""
      category: "LLM Overrides"

    - name: "Model Override"
      path: "llm.model_name"
      type: "string"
      description: "Override model name for story DM. Leave empty to use the same
        model as dialogue."
      defaultValue: ""
      category: "LLM Overrides"

    # ... etc for Temperature Override, Max Tokens Override, Timeout Override
```

### What variants do

A **variant** is a named LLM configuration this plugin uses. IntelEngine declares one — `intel_story_dm` — because its
Story DM runs a different prompt than dialogue and the user may want a different (cheaper or smarter) model for it. The
user fills `llm.*` settings to override per variant; SkyrimNet routes the variant's prompts through that config. The
base OpenRouter / dialogue config is used when overrides are blank.

### What schema fields do

`schema.fields` declares which keys in `settings.yaml` are user-editable, with type, description, range, default, and
category. SkyrimNet renders these into its own settings UI. This is **parallel to (not a replacement for)** an SkyUI
MCM page — see "MCM duality" below.

---

## Sample settings & data files

The plugin ships `.sample.yaml` files. SkyrimNet's convention is that the user copies these to drop the `.sample.`
segment when they want to override defaults. IntelEngine ships:

- `settings.sample.yaml` — declares default values for everything declared in `manifest.yaml#schema.fields`
- `factions.sample.yaml` — IntelEngine-specific data file: 9 pre-configured factions, each with type, hold, Skyrim
  faction FormID, leaders, rivals, allies, army strength, war threshold, conflict style, soldier template, prison
  location, plus default pairwise relation scores

The plugin's SKSE DLL reads whichever file exists (`factions.yaml` if present, else `factions.sample.yaml`).

---

## Prompt templates

### Top-level prompts

Free-form Jinja2 templates living in `prompts/<name>.prompt`. The Papyrus side dispatches them by name to SkyrimNet —
e.g. `SendCustomPromptToLLM(context, "intel_story_dm")` — and SkyrimNet renders the template with the supplied context
plus its standard helpers (player, world state, world events, etc.).

IntelEngine's top-level prompts:

- **`intel_story_dm.prompt`** — Main Story DM. ~2000 lines; receives world state + candidate pool + dispatch
  history; returns one JSON dispatch decision under 700 chars.
- **`intel_story_npc_dm.prompt`** — NPC-to-NPC social DM. Receives location groups of loaded NPCs with shared
  memories; picks one interaction (argument/deal/romance/favor/gossip) grounded in real memories.
- **`intel_political_dm.prompt`** — Political DM. Receives faction relations, recent events, wars, player
  standings; picks one of 24 event types with relation delta.
- **`intel_schedule_safety_net.prompt`** — Schedule safety net. Detects when dialogue implied a scheduled
  action that the LLM didn't actually fire.

### Character bio submodule prompts

These are the most architecturally interesting fragments. They live at
`prompts/submodules/character_bio/NNNN_<plugin>_<name>.prompt` and are **injected into every NPC's bio every dialogue
turn**. The numeric prefix is the sort order in the rendered bio (lower = earlier).

IntelEngine's 7 submodules and where they slot:

| Prefix | File                                    | Content                         |
| ------ | --------------------------------------- | ------------------------------- |
| 0197   | `0197_intel_received_messages.prompt`   | Messages the NPC received       |
| 0198   | `0198_intel_schedule_awareness.prompt`  | Active scheduled meetings       |
| 0199   | `0199_intel_meeting_outcome.prompt`     | Past meeting outcomes           |
| 0200   | `0200_intel_gossip.prompt`              | Rumors heard/shared             |
| 0800   | `0800_intel_facts.prompt`               | Facts from story events         |
| 0801   | `0801_intel_task_awareness.prompt`      | Current/past tasks              |
| 0810   | `0810_intel_political_awareness.prompt` | Faction standing, recent events |

### Submodule template anatomy (annotated `0801_intel_task_awareness.prompt`)

```jinja
{# Gate: only render when SkyrimNet asks for "full" or "static" bio mode #}
{% if render_mode == "full" or render_mode == "static" %}
{# first_person flag: SkyrimNet renders bios in 1st or 3rd person depending on mode #}
{% set first_person = (render_mode == "transform" or render_mode == "full" or render_mode == "thoughts") %}
{% if actorUUID and isValidActor(actorUUID) %}

  {% if first_person %}
    {# Use a precomputed Papyrus-side string if available — avoids in-template formatting #}
    {% set renderedHistory = papyrus_util("GetStringValue", actorUUID, "Intel_TaskHistoryRendered", "") %}
    {% if renderedHistory != "" %}
      {{ renderedHistory }}
    {% endif %}
  {% else %}
    {# Third-person: build the history list inline from a Papyrus StringList #}
    {% set historyDescs = papyrus_util("GetStringList", actorUUID, "Intel_TaskHistory") %}
    {% set historyTimes = papyrus_util("GetFloatList", actorUUID, "Intel_TaskHistoryTime") %}
    {% if length(historyDescs) > 0 %}
      {% set currentTime = get_global_value("GameDaysPassed") %}
      ### Past Tasks
      {{ decnpc(actorUUID).name }}'s completed tasks:
      {% for _entry in historyDescs %}
        {% set idx = loop.index1 - 1 %}
        {% set desc = at(historyDescs, idx) %}
        {% set hoursPassed = (currentTime - at(historyTimes, idx)) * 24.0 %}
        - {{ desc }} ({% if hoursPassed < 0.1 %}just now
                    {% elif hoursPassed < 1.0 %}a few minutes ago
                    {% elif hoursPassed < 3.0 %}a short while ago
                    ...
                    {% else %}some time ago{% endif %})
      {% endfor %}
    {% endif %}
  {% endif %}

  {# Current task block: pulls task type, target, state, lingering flags ... #}
  {% set taskType = lower(papyrus_util("GetStringValue", actorUUID, "Intel_TaskType", "")) %}
  {% set busyReason = busy_reason(actorUUID) %}
  {% if taskType != "" %}
    {# ... renders "I am currently traveling to X" / "X is delivering a message to Y" ... #}
    {# Closes with: "To stop this task, use the **CancelCurrentTask** action.
       To change travel speed, use the **ChangeSpeed** action." #}
  {% elif busyReason != "" %}
    {# Fallback: busy from another subsystem #}
  {% endif %}

{% endif %}
{% endif %}
```

### Helpers available to submodule templates

- **`render_mode`** — One of `"full"`, `"static"`, `"transform"`, `"thoughts"` — different bio rendering
  passes.
- **`actorUUID`** — The NPC the bio is being rendered for.
- **`isValidActor(uuid)`** — Guard for missing/invalid actors.
- **`papyrus_util("GetStringValue"/"GetIntValue"/"GetFloatValue"/"GetStringList"/"GetFloatList", uuid, key, default?)`**
  — Read PapyrusUtil StorageUtil values keyed on the actor. **This is the bridge from Papyrus state into the
  bio template.**
- **`busy_reason(uuid)`** — The NPC's current busy reason (set by SkyrimNet decorator from any plugin).
- **`decnpc(uuid)`** — NPC decorator providing `.name`, etc.
- **`get_global_value("GameDaysPassed")`** — Skyrim global.
- **`length(list)`, `at(list, idx)`, `lower(s)`, etc.** — Standard Jinja-style helpers.
- **`{{ npc.name }}`, `{{ player.name }}`** — Top-level variables SkyrimNet injects.
- **Markdown hints like `**ActionName**`** — The LLM is steered to call the named action.

### Implication: Papyrus is the source of truth that bios pull from

State the LLM should "know" about an NPC is stored in Papyrus via PapyrusUtil's StorageUtil (per-actor keyed
strings/ints/floats/lists). The submodule pulls it at render time. **There is no direct C++ → bio path** — even the C++
SlotTracker has to push its state through Papyrus arrays/StorageUtil for the bio template to see it. That's why
IntelEngine's `SyncArraysFromSlotTracker()` exists: to mirror authoritative C++ state back into Papyrus on load.

The PapyrusUtil key namespace used by IntelEngine for the task awareness submodule:

- `Intel_TaskHistory` (StringList), `Intel_TaskHistoryTime` (FloatList), `Intel_TaskHistoryRendered` (String)
- `Intel_TaskType`, `Intel_Target`, `Intel_State`, `Intel_MeetingLingering`, `Intel_TravelLingering`,
  `Intel_MeetingPlayerName`, `Intel_StoryNarration`

NarrativeEngine will need a similarly disciplined key-namespace convention.

---

## Decorators

Decorators are functions registered by the SKSE plugin with SkyrimNet at startup via the soft-linked SkyrimNet C API
(`SkyrimNetAPI.h`). They are then callable from:

- Action `eligibilityRules` (as the `decoratorName:` field)
- `.prompt` templates (as Jinja helpers — see `busy_reason(actorUUID)` above)

IntelEngine's registered decorators include:

| Decorator                    | Purpose                                                              |
| ---------------------------- | -------------------------------------------------------------------- |
| `intel_can_accept_task`      | True iff NPC has a free slot, isn't on cooldown, and isn't in combat |
| `intel_can_travel_to`        | True iff a destination is reachable                                  |
| `intel_can_find_npc`         | True iff a target NPC can be located                                 |
| `intel_can_resolve_semantic` | True iff semantic directions are available in the current cell       |
| `intel_validate_task`        | Full pre-flight validation of a proposed task                        |
| `intel_get_failure_reason`   | Returns a human-readable failure explanation the NPC can voice       |

These are not standalone — they share the same C++ index/resolver/validator infrastructure (NPCIndex, LocationResolver,
ActionValidator) so eligibility and prompt rendering get consistent answers.

---

## ModEvents from C++ → Papyrus

The C++ DLL fires a small set of ModEvents that SkyrimNet (and/or the plugin's own Papyrus) listens for. IntelEngine
uses at least:

- `dynamic_bio_update` — fired by DialogueTracker when an NPC's per-NPC dialogue count crosses a threshold; signals
  SkyrimNet to refresh the NPC's bio template

Documenting these as they are encountered in NarrativeEngine work is worth doing — they're how out-of-band state
changes propagate to the SkyrimNet runtime.

---

## MCM duality (worth flagging)

IntelEngine exposes settings through **two parallel surfaces**:

1. **SkyUI MCM** via `Interface/MCMHelper/IntelEngine/config.json` and `IntelEngine_MCM.psc` (extends `SKI_ConfigBase`).
   Drives per-task/per-feature toggles backed by Globals + StorageUtil. Required when the user is running SkyUI.
2. **SkyrimNet's manifest schema** via `manifest.yaml#schema.fields` rendered into SkyrimNet's own UI. Plugin-level
   settings (hotkeys, blocklists, LLM overrides).

The two overlap but aren't identical. The dashboard (PrismaUI overlay) gives a third surface that reads/writes both.
NarrativeEngine should decide up-front which settings live where, and whether to consolidate.

---

## What this means for NarrativeEngine

When NarrativeEngine reaches the integration-design stage, the contract above is the menu of integration points it can
use:

- **Actions** to expose new tools to the LLM
- **Categories** to organize them shallowly
- **Manifest variants** for per-use-case LLM configs
- **Top-level prompts** for any "DM"-style decision the plugin drives
- **Submodule prompts** for new persistent context to be injected into every NPC bio
- **Decorators** for shared eligibility + prompt helpers backed by C++ data
- **ModEvents** for out-of-band state notifications
- **StorageUtil key namespace** as the canonical bridge from Papyrus/C++ state to bio templates

For each point used, look at the matching IntelEngine asset to see the working precedent before designing
NarrativeEngine's version.
