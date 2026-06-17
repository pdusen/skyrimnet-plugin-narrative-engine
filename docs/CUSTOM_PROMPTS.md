# Writing SkyrimNet custom prompts

`.prompt` files under `statics/SKSE/Plugins/SkyrimNet/prompts/` are Jinja templates that SkyrimNet renders at LLM
call time. The rendered Markdown is parsed into chat-API messages and sent to the configured LLM. Our SKSE side
invokes a named template with `SkyrimNetAPI::SendCustomPromptToLLM(name, variant, contextJson, callback)`;
SkyrimNet auto-discovers `Data/SKSE/Plugins/SkyrimNet/prompts/<name>.prompt` (no manifest registration needed),
renders it with the JSON we passed, and ships the result.

The build's `statics/` deploy mirrors `statics/SKSE/Plugins/SkyrimNet/prompts/` straight into the mod folder, so
new prompts are picked up on the next build.

## Required structure: split into `system` and `user`

Every NarrativeEngine prompt **must** use SkyrimNet's literal section markers to split into a system message and a
user message:

```jinja
[ system ]
... persistent instructions, role, task, output format, structured state ...
[ end system ]

[ user ]
... terse per-call kick that says "do the thing now" ...
[ end user ]
```

Why this matters:

- **Maps to the chat API.** Commercial LLM providers (OpenAI, Anthropic, OpenRouter) take role-tagged messages.
  `[ system ]` becomes a `role: system` message; `[ user ]` becomes a `role: user` message. Without the markers,
  SkyrimNet has to fall back on a default packaging that defeats both points below.
- **Caches the prompt prefix.** Providers cache stable prefixes (especially the system message). Putting durable
  instructions in `[ system ]` and the per-call payload in `[ user ]` lets the provider re-use cache between
  ticks. Mixing them invalidates the cache every call.
- **Matches the model's training.** Models are tuned to read instructions as system-role and act on requests as
  user-role. The system-vs-user split is part of how they were aligned; honoring it gives more reliable behavior.

### What goes in `[ system ]`

- **`## Task`** — one-sentence role + objective at the very top.
- **`## Output Format`** — required output shape, *before* anything else. Models follow the format better when
  they see it first and last.
- **Hard constraints** — what the model must / must not do.
- **`## Your Role` / style guidance** — tone, voice, principles.
- **The structured state** — `## Current state`, `## Recent events`, `## Available characters`, etc. Render the
  context JSON's relevant fields here.

### What goes in `[ user ]`

- A short imperative kick that triggers the model to act using the system above.
- A one-line restatement of the output contract at the very end (the model sees this last, just before
  generating). Bracketing the actual work with the format reminder maximizes adherence.

The user section should be **≤5 rendered lines** in almost every case. If it's growing, the content probably
belongs in `[ system ]`.

## What NOT to tell the LLM

**Do not describe what triggered this call.** The prompt should describe the *current state* and the *task*,
not the *cadence* or the *triggering event*:

- ✗ "This prompt fires every 30 seconds to..."
- ✗ "You are being asked because combat just ended..."
- ✗ "The mod's tick driver invoked you to..."
- ✓ "You read the current world state and judge..."
- ✓ "Current phase: RisingAction. Recent events: ..."

Reasons:

- **Reduces noise.** Trigger metadata isn't part of the decision the model has to make. Including it dilutes
  focus on the actual task.
- **Avoids leaking system shape.** Telling the model "this fires every 30s" invites it to reason about cadence
  and hallucinate cadence-aware behavior ("I said calm last tick, so I'll say tense this tick to vary it").
  Each call should be a fresh read of state.
- **Decouples prompt from triggering code.** If we change *when* the call fires later (event-driven, different
  interval, batched), we don't have to rewrite the prompt. Trigger semantics live in the SKSE plugin; the
  prompt describes state and task.

The model's job is to look at what's true *right now* and decide. Not to model how the request got to it.

## Useful conventions

Patterns worth copying from `gamemaster_action_selector.prompt`:

- **State the output format at the very top of `[ system ]`**, then end `[ user ]` with a one-line restatement
  of the same contract. Format-first, format-last.
- **Organize with `## Headings`.** The model treats them as semantic structure; sections become attention
  anchors.
- **Hide whole sections with `{% if capability %}`** rather than rendering placeholder text. The model never
  sees "no actions available" — the section just isn't there.
- **Use `---` (horizontal rule)** to separate logical phases (instructions → state → task).
- **Inline examples with ✓ / ✗**: clearer than describing the right shape in prose.
- **`CRITICAL:` prefix** for hard constraints, placed next to the data they constrain.
- **Compose component sub-templates** via `{{ render_template("components/foo") }}` when a fragment is reused
  across prompts. SkyrimNet ships shared components under `prompts/components/`.
- **SkyrimNet decorators**: `decnpc(uuid).name`, `is_action_enabled("X")`, `units_to_meters(...)`, `length(...)`,
  `existsIn(obj, "key")`, `papyrus_util(...)`. See
  [`prior-art/SKYRIMNET_PLUGIN_CONTRACT.md`](prior-art/SKYRIMNET_PLUGIN_CONTRACT.md) for the broader surface.

## File naming + invocation

- Path: `statics/SKSE/Plugins/SkyrimNet/prompts/<name>.prompt`. The `statics/` deploy mirrors it into the mod
  folder unchanged.
- Filename: lowercase `snake_case`, prefixed with `narrative_engine_` so it can't collide with another
  SkyrimNet plugin's prompts. SkyrimNet's auto-discovery is filename-based; no manifest entry needed.
  Suffix the filename with what the prompt *does* (`_story_eval`, `_action_select`, …) so multiple
  Narrative Engine prompts can coexist as siblings.
- `name` passed to `SendCustomPromptToLLM` is the filename *without* the `.prompt` extension. So
  `narrative_engine_story_eval.prompt` is invoked as
  `SendCustomPromptToLLM("narrative_engine_story_eval", ...)`.

## Minimal skeleton

```jinja
[ system ]
## Task
You are <role>. You <one-sentence task description>.

## Output Format
Return exactly <format description>. No markdown fences. No prose outside.
- `<example shape>` — when <condition>

## Current state
- Field: {{ field }}
- Other: {{ other }}

## Your role
- One bullet per principle.

## Recent context
{% if length(events) == 0 %}
None.
{% else %}
{% for evt in events %}- {{ evt.text }}
{% endfor %}
{% endif %}
[ end system ]

[ user ]
<One-line imperative kick.>
Return exactly <format>. No other text.
[ end user ]
```
