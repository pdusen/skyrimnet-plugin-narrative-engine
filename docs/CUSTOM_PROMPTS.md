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

## Referring to the player character

**Never call the player "the Dragonborn" or any other lore-anchored
title in a prompt.** Mods can replace the main quest, skip Helgen, or
otherwise produce a save where the player character isn't on the
Dragonborn path at all — for those saves, "Dragonborn" is a false
identity that pollutes every letter, memory write, dialogue line, and
piece of in-fiction content with a fact the world doesn't actually
support.

Use the SkyrimNet decorator `{{ player.name }}` instead. SkyrimNet
resolves it at render time to the live player character's actual
display name, whatever the player set during character creation:

- ✗ "...will have a courier deliver to the Dragonborn."
- ✗ "Pick an NPC who knows the Dragonborn and..."
- ✓ "...will have a courier deliver to {{ player.name }}."
- ✓ "Pick an NPC who knows {{ player.name }} and..."

`{{ player.name }}` renders to a proper noun (`Bob Dole`, `Lydia`,
whatever the player typed at character creation), so do NOT precede it
with an article or demonstrative. The patterns that worked with the
old "Dragonborn" title don't survive the substitution:

- ✗ "deliver to the {{ player.name }}" → "deliver to the Bob Dole"
- ✗ "warning the {{ player.name }} about" → "warning the Bob Dole about"
- ✓ "deliver to {{ player.name }}" → "deliver to Bob Dole"
- ✓ "warning {{ player.name }} about" → "warning Bob Dole about"

Watch for the article hiding on the previous line when the source
sentence wraps — the substitution leaves "the\n{{ player.name }}"
together and the rendered output reads the same way.

The same rule applies in any other context that might leak into LLM
output — semantic search queries, memory text we write via
`PublicAddMemory`, knowledge entries, decorator return values. If a
string is going to end up in front of either an LLM or the player, the
player's actual name belongs there, not a generic title.

The one place this rule doesn't apply is action / module descriptions
written as C++ string constants (e.g. `IAction::Description`) that get
embedded into a prompt as a raw JSON value. Inja doesn't re-render
nested template syntax inside string values by default, so
`{{ player.name }}` written inside a C++ literal would print
literally as those characters. In those cases, use neutral phrasing
("the player", "the player character") and let SkyrimNet-rendered
context elsewhere in the prompt carry the actual name.

Where to source the name from C++ when you need it in a search query
or memory write: `RE::PlayerCharacter::GetSingleton()->GetDisplayFullName()`,
called on the main thread.

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

## Don't tell the model what NOT to write

Negative content instructions ("don't mention X", "avoid Y", "don't make every
response about Z") frequently produce the opposite of what you want. The
mechanism is the same as "don't picture a pink elephant" — to comply with
the rule, the model first has to model the forbidden thing, and modeling it
raises its salience. The banned topic becomes the most active concept in
context just as generation starts.

We hit this concretely while writing a prompt asking for a letter in the
voice of a Whiterun working woman: we included "do NOT make every letter
about textiles or cloth" because earlier iterations all came back about
fabric shipments. Every subsequent generation was *still* about fabric
shipments. Removing the negation broke the textile fixation immediately.
(The replacement we tried — a long list of *desired* topics — ran into a
separate problem; see the next section. But the fabric anchor was gone.)

The fix: state what you DO want, framed as an abstract domain rather
than a list of specific examples.

- ✗ "Don't make every letter about textiles."
- ✓ "Pick a broad domain — money, work, gossip, a request, a grievance,
  social obligation — and invent a specific situation within it."

- ✗ "Avoid dark themes."
- ✓ "Keep the tone light — focus on small everyday matters rather than
  weighty themes."

If you catch yourself writing "don't" in a content guidance section, invert
it: write the positive form, but be careful not to swap one anchor for
another (read the next section before writing the replacement).

**Caveat: this applies to *content* steering, not to output-shape rules.**
"Output JSON only — no markdown fences" and "Return exactly the object,
no prose before or after" stay phrased as negations. Those are
once-at-generation-start structural checks the model honors fine; they
don't raise the salience of "write markdown fences" as a generation
target the way content negations do.

## Specific examples become attractors

Even when phrased positively, any list of specific concrete examples in
a content-guidance section tends to collapse the model's output
distribution onto one of those examples — or onto whatever concrete noun
fits the closest semantic neighborhood. The model treats specifics as
anchor points and generates near them rather than ranging across the
full implied domain. This is the same underlying mechanic as the
"don't picture a pink elephant" failure above, just on the positive
side: any concrete token in the prompt becomes an attention anchor that
gets surfaced in generation, regardless of whether it was framed as ✓
or ✗.

We hit this twice in a row writing the same letter-generation prompt:

1. The first character description listed Ysolda's goods as "hides,
   mead, wheat, salted meat, dyed wool, secondhand armor..." Every
   generation came back about textiles — "dyed wool" was the strongest
   anchor and the model followed it.
2. After stripping the goods list, we kept a long list of "plausible
   topics" in the content guidance: "an overdue debt, a stablehand who
   didn't show up, ..., a shipment of *any* kind of goods that arrived
   spoiled or late, ..." Every generation came back about salted hake —
   "shipment ... spoiled" was the strongest anchor, and the model
   converged on the same specific concrete instance call after call.

The fix: don't give concrete examples in content guidance. Name the
abstract domain(s) the response should land in, and explicitly tell the
model to invent the specific situation itself.

- ✗ "Plausible topics: a stablehand who didn't show up, a borrowed mule
  that hasn't returned, market gossip about a rival, weather wrecking
  a delivery..."
- ✓ "Pick a broad domain — money, work, news, a request, a grievance,
  gossip, social obligation — and invent a specific situation within
  it yourself. Do not pull a topic from any list; come up with one
  fresh."

- ✗ Character profile: "She handles whatever's in front of her: hides,
  mead, wheat, salted meat, dyed wool, secondhand armor, small
  carvings, alchemy ingredients, firewood..."
- ✓ Character profile: "She makes her living through a patchwork of
  small ventures."

The principle generalizes beyond topic lists. Anywhere a "for example"
might creep in — prop lists, recipient types, possible reactions,
sample phrasings — the same dynamic applies. If you list five specific
things, expect the next generation to be about one of those five things
or its closest semantic neighbor.

When you need a wide distribution of outputs, give the model an
ontology of domains, not a menu of specifics.

**Caveat: this is about *content* generation, not *output structure*.**
Specific examples of the required output *shape* (e.g. a sample JSON
object showing field names and types, a sample call-and-response
exchange showing turn format) are still good — those are demonstrating
the schema, not seeding the content distribution. The pink-elephant
dynamic only kicks in when the example competes with the model's
content-generation choice. Use ✓/✗ inline shape examples freely; just
don't bury the model in concrete topical examples and expect it to
generalize.

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
