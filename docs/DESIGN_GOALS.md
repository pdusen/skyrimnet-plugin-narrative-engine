# NarrativeEngine — Design Goals

A high-level design document. Describes *what* the mod is and *what it does*. Implementation choices (architecture,
modules, data structures, technology stack) are deliberately out of scope here and belong in later planning documents.

---

## Vision

Like Left 4 Dead's AI Director, NarrativeEngine observes the player's experience in real time and intervenes to shape
its narrative pacing. Skyrim's open world delivers emergent stories through systems, but it has no overarching dramatic
shape — moments of intensity and quiet happen wherever the player happens to walk. NarrativeEngine imposes shape on top
of that: a continuous, never-ending arc of tension and release, driven by an LLM that watches what's happening, decides
what should happen next, and reaches into the world to make it happen.

It does this *not* by directing a story (there is no plot to follow) but by **shaping the curve of intensity** the
player experiences over time.

---

## Core design principles

These are the foundational rules that constrain every other design decision. If a later choice violates one of these,
the choice is wrong, not the principle.

### Alpha Canon / Beta Canon

Events that originate from the vanilla game or any other mod the player has installed are **Alpha Canon** —
authoritative, immutable, never overridden. Events that NarrativeEngine itself generates are **Beta Canon** —
secondary, layered on top of Alpha Canon, always yielding to it.

In practice this means:

- The Director never countermands a vanilla quest, scripted scene, combat encounter, or AI behavior.
- The Director *reads* vanilla events as part of its situational awareness, and lets them count toward the current
  tension level.
- When vanilla content is producing the right tension by itself, the Director adds nothing; restraint is a first-class
  option.
- The Director may *react* to vanilla events (e.g. the player just finished a vanilla quest → a Falling Action beat may
  be appropriate), but it may not modify or invalidate them.

### One abstract tension curve, not authored arcs

There is exactly one global narrative cycle running at any time. It moves through the five phases of Freytag's Pyramid
— Exposition, Rising Action, Climax, Falling Action, Resolution — and then begins again, indefinitely.

The cycle is **abstract**: there is no per-cycle theme, throughline, or "story concept" the Director is committed to
maintaining. It is a tension *level* traveling through a fixed shape, not a story to tell. Each evaluation scores the
current moment's tension and decides whether to stay in the current phase or advance to the next.

This is a deliberate fit for Skyrim's player agency. The player will not stick to a throughline we hand them, so we
hand them none.

### Recent history is ground truth

Every Director decision is grounded in the actual recent gameplay history — what the player did, who they spoke with,
what fights happened, what events the world produced, how long the current phase has lasted, what the Director itself
recently decided. The Director does not invent context; it reasons over a real, persisted record of recent events.

"Recent" is a fluid concept. Sometimes it spans the last few minutes (for assessing the moment); sometimes it spans the
last several in-game hours (for assessing phase dwell time and avoiding repetition). The appropriate window depends on
the question being asked.

### The Director both injects and suppresses

The Director's authority is not limited to causing new events. It can also **suppress** — instruct other systems
(background simulations, action sources) to stand down for a period when the desired narrative effect requires calm.

This is essential to Falling Action and Resolution. Those phases need not just "low-tension events" but also *the
absence* of high-tension events. A Resolution phase ruined by an unrelated background-sim battle erupting nearby is a
failure.

### Real-time cadence, in-game effects

The Director evaluates at fixed **real-time** intervals, not game-time intervals. Narrative pacing is something the
player experiences at the keyboard in wall-clock time; pausing the game, fast-traveling, or sleeping should not advance
the curve. The events it produces and consumes are in-game (combat, dialogue, NPC behavior, world state), but the
cadence of decision-making is wall-clock.

---

## The narrative model

Freytag's Pyramid is interpreted here as a tension envelope:

| Phase          | Tension    | What the player should be experiencing                                        |
| -------------- | ---------- | ----------------------------------------------------------------------------- |
| Exposition     | Low        | Routine and ambient. World-building moments. Nothing pressing.                |
| Rising Action  | Climbing   | Pressure mounting. New threats or complications appearing. Stakes rising.     |
| Climax         | Peak       | The high-intensity payoff. Combat, confrontation, decision under duress.      |
| Falling Action | Descending | Immediate aftermath. Consequences unfolding. Follow-through.                  |
| Resolution     | Low again  | Reflection, closure, return to ambient. A quiet moment before the next cycle. |

The phases are **not equal in length**. Exposition and Resolution should naturally run longer; Climax should be brief
and intense. The Director uses heuristics for desired phase duration and is willing to force progression when a phase
has overstayed its narrative welcome — but it does so by *creating events that fit the next phase*, not by flipping an
internal flag in isolation.

The cycle is continuous: Resolution flows directly into the next Exposition. The player should never notice phase
boundaries as such; they should only notice the *feel* of the moment.

---

## The AI Director

### Per-evaluation responsibilities

On each evaluation tick, the Director:

1. **Reads** the recent event log, the current world/player state, the current phase, and how long the cycle has been
   in the current phase.
2. **Evaluates** (via LLM) the tension level of the present moment — informed by both what NarrativeEngine has generated
   *and* what vanilla content and other mods have produced.
3. **Determines** whether the current phase is still appropriate or whether to advance to the next phase.
4. **Acts** if advancement (or reinforcement) is warranted, by selecting one or more actions from its toolbox and
   dispatching them with appropriate parameters.
5. **Records** the evaluation — what it observed, what it concluded, what it chose to do, and *why* — into its own
   persistent decision log, so future evaluations have continuity.

The Director may also conclude that **no action is needed** — that vanilla content or the natural flow of events is
already producing the right shape — and refrain from acting. Doing nothing is a real choice, not a fallback.

### Decision space

Each evaluation produces, at minimum:

- A **tension score** for the current moment
- A **phase determination** (remain in current phase / advance to next)
- An **action selection** (which toolbox item to fire, with parameters) — or null
- A **suppression directive** (which background simulations, if any, should temporarily back off and for how long) —
  or null
- A **narrative note** (a short rationale explaining the chosen decision, for posterity in the decision log)

The decision log is itself an input to future evaluations. The Director should be able to see what it recently did and
avoid repetition, contradiction, or escalation loops.

### Context sensitivity

Action selection is itself an LLM evaluation. Given a chosen direction (raise / lower / hold tension), the Director
chooses **which** action from the toolbox best fits the present context: who the player is, where they are, what they
just did, who is nearby, what factions are active, what the recent event log shows, what background simulations have
brewing.

A bandit ambush in the middle of Whiterun's market is wrong. The same ambush on the road to Riften at dusk is right.
The Director has to make that distinction.

The Director must also be aware of the player's current activity (mid-vanilla-quest, in active combat, in a scripted
dialogue scene, deep in a dungeon, etc.) and avoid intruding on Alpha Canon moments. Some actions are inherently
"do-not-disturb-respecting"; others may be deferred until the player's current activity ends.

---

## The action toolbox

Actions are the levers the Director can pull to nudge the tension curve. The toolbox spans both directions and includes
both world events and the *absence* of world events.

### Tension-raising actions (for Rising Action, Climax)

- **Hostile encounters** — bandit ambush, faction-soldier waylay, beast attack, assassin
- **Stalking and surveillance** — a hostile NPC quietly tailing the player
- **Threatening dialogue** — an NPC delivering a warning, a confrontation, a demand
- **Looming background-sim events** — a politics tick has brewed an imminent battle, and the Director chooses now to
  manifest it near the player
- **Time pressure** — an NPC arrives with an urgent ask under a deadline
- **Bad news delivery** — a courier brings dire word that reframes stakes
- **Escalating presence** — hostile reinforcements arriving in a fight already underway

### Tension-lowering actions (for Falling Action, Resolution)

- **NPC reflection visits** — an NPC seeks out the player to reflect on, celebrate, or process recent events
- **Closure deliveries** — a courier delivers a letter, a reward, a thank-you, an acknowledgment
- **Respite offers** — a friendly NPC offers food, a drink, a song, shelter, an embrace
- **Faction stand-downs** — a faction quietly signals de-escalation in dialogue or behavior
- **Suppression windows** — explicit "no hostile encounters from any source for N real minutes"
- **Sim softening** — background simulations are instructed to skip their next tick or soften their output
- **Acknowledgment gossip** — rumors propagate about resolved events, the world catching up to what happened

### Either-direction actions (context-dependent)

- **Neutral messenger arrival** — an NPC arrives with news whose framing the Director chooses (escalating or
  de-escalating)
- **Witnessed NPC interaction** — two NPCs interact in front of the player, with content shaped to current need
- **Ambient shifts** — environmental or behavioral changes whose tone matches the desired direction

### Properties of the toolbox

- **Extensible.** New actions can be added without restructuring the Director.
- **Self-describing.** Each action declares its tension polarity (raise / lower / either), its preconditions, the
  context it needs, and any cooldowns or rate limits.
- **Composable.** Multiple actions may fire in concert when the situation supports it.
- **Honest about reach.** Some actions require an NPC the Director can dispatch; some require an unloaded outdoor cell;
  some require an active background sim with the right material. Actions whose preconditions aren't met are simply
  unavailable that tick.

---

## Background simulations

NarrativeEngine runs multiple long-running simulations in parallel with the Director, each modeling a domain of the
game world. The initial set is open — likely candidates include **politics** (faction relations, tensions, alliances),
**faction plots** (schemes, betrayals, conspiracies), and others can be added without architectural disruption.

### Primary role: fodder for the Director

The first purpose of every background sim is to produce **narratively shaped material the Director can draw from**.
When the Director needs a tension-raising event and the politics sim has been brewing a Stormcloak/Imperial border
skirmish, the Director can manifest that brewing event near the player as its tension move. The sim supplies
plausibility, continuity, and lore-grounding; the Director supplies timing and pacing.

This means sims should be designed to produce events with **narrative texture** — named participants, relationships,
motives, stakes — not just numeric state changes. The Director's job is easier when the sim hands it something it can
frame as dialogue and consequence, rather than abstract values it has to reinterpret.

### Secondary role: independent world manifestation

Sims also run their own clocks and may occasionally produce **independent world changes** the player can stumble into
accidentally. The Dawnguard-versus-Volkihar example: a sim's running conflict produces a battle near Dawnstar at a
particular game-time, and a player who happens to be in the area finds themselves in the middle of it.

Independent manifestations are **bounded**:

- They happen at sim-determined cadence, not on every tick.
- They remain Beta Canon — they yield to vanilla content and to the Director's suppression directives.
- They should feel like a world living its own life, not like the Director constantly poking the player.

### Sim independence and Director coordination

Sims tick on their own schedule independently of the Director. The Director never *drives* a sim's internal state — it
only *reads* the sim's current state and *consumes* the events the sim has staged or is about to produce.

The Director's suppression authority is the one exception: it can instruct a sim to defer or skip an upcoming
manifestation when narrative pacing requires calm. Sims must honor suppression.

---

## Observability surface

The mod ships with a PrismaUI dashboard suite providing visibility into the Director and every background simulation.

### What the dashboards show

- **The AI Director** — current phase, time in phase, recent tension scores, the most recent evaluation's reasoning,
  recent action history, current suppression directives, the event log the Director is reasoning over.
- **Each background simulation** — current internal state, recent activity, upcoming staged events, status of any
  pending independent manifestations, current suppression status.
- **The shared recent-event log** — what the Director and the sims are reading from.

### Who they are for

The dashboards are **primarily a development and debugging surface** — for the author to observe whether the Director
is making sensible decisions, whether sims are producing useful fodder, whether the system is too aggressive or too
quiet, whether suppression is working as intended.

They are **secondarily for curious players** who want to peek behind the curtain. They are not intended to be a
normal-gameplay control surface.

### Constraints

- Dashboards are **read-only by default**.
- Any write surface (manual triggers, parameter overrides, sim resets, force-advance phase) is a **debug affordance**,
  not a player-facing control panel.
- Dashboards are entirely optional — if the PrismaUI dependency is absent, the mod functions without them.

---

## Non-goals

To constrain scope, these are explicitly out of design:

- **Authored stories or per-cycle themes.** The Director shapes a curve, not a narrative.
- **Overriding or replacing vanilla content.** Alpha Canon is absolute.
- **A player-facing tuning UI.** Dashboards are debug surfaces. Player-facing tuning is not a goal.
- **Replacing SkyrimNet's existing systems.** NarrativeEngine layers on top of what SkyrimNet already provides; it does
  not duplicate, replace, or compete with SkyrimNet's core dialogue, memory, or event systems.
- **A "win condition" or progression.** The cycle is endless; there is no narrative state the player can reach where
  the Director shuts off or the sims complete.
- **Multiplayer or shared world state.** Single-player Skyrim, single-save context.
- **Displacement of other SkyrimNet plugins.** Where another plugin (IntelEngine, if installed; others in the future)
  provides actions or sims that overlap with NarrativeEngine, coexistence is the goal, not displacement.
  Overlap-handling is a design problem to solve, not a justification to absorb the other plugin's responsibilities.

---

## Open questions to revisit during later planning

Concept-level questions that don't need to be resolved now but will shape future planning:

- **Cycle initiation.** On a fresh game (or first install on an existing save), what phase does the cycle start in and
  at what tension level?
- **Phase duration policy.** Are there preferred or capped phase durations, or does the LLM determine all phase
  boundaries from event content alone? How do we keep the curve from settling into a degenerate rhythm?
- **Collision handling.** When two background sims independently stage events that collide in time and place, who wins?
  Does the Director arbitrate, or does each sim self-defer?
- **Action repetition.** How does the toolbox avoid feeling repetitive when the same action type is the right call
  multiple times in a row?
- **Director memory horizon.** How far back does "recent event history" extend, and how does LLM context-window pressure
  constrain the working set?
- **Cross-plugin coexistence.** How does NarrativeEngine coexist with peer plugins (such as IntelEngine, if installed)
  that provide overlapping actions or overlapping sims? Coordinate, compete, or partition by domain?
- **Player-perceived agency over the curve.** Should the player's deliberate choices (declining a quest, leaving a
  fight, sleeping a full day) be treated as signals the Director respects, or as raw events to be reacted to?

These will be addressed as the design firms up.
