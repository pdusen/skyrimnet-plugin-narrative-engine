# Faction Plots — Feature Design

A background simulation in which unique NPCs pursue multi-step schemes: a **mastermind** adopts a concrete
objective, plans a sequence of **steps** toward it, delegates those steps to people who owe them something, and
adapts when a step goes wrong. Steps resolve over in-world days whether or not the player is anywhere nearby.
Most of what a plot produces is memory — on the mastermind, on whoever did the work, and on whoever it was done
to. Occasionally it produces a tangible change in the world. Occasionally it produces an errand for the player.

> **What this document is.** A **feature design** — what Faction Plots is and how it behaves — not a phase
> plan. The feature is too large for one phase; **Implementation staging** near the end proposes how it breaks
> into several, and each of those gets its own `docs/implementation/PHASE_NN_*.md` doc with numbered steps and
> verification criteria when its turn comes. This document is the contract those phase docs are written
> against, and it stays current as the feature evolves; the phase docs are point-in-time work plans.
>
> **Status: converging.** Cadence, plot budget, the delivery beat, and what "standing" means are settled and
> stated as such. What is still open is listed under **Open questions**, each tagged with the phase that forces
> it. Remaining numbers are given as shapes rather than values. Where the design leans on an existing
> subsystem, that is noted so the reuse claim can be checked.

---

## Why this feature exists

The ultimate-vision framing names three stakes tiers of background NPC life. Gossip (Phase 13) is tier 2.
This is **tier 3** — plotting and scheming — and it is the tier the whole life-sim framing was built toward. Tier 1
and 2 produce memories about ordinary life; tier 3 produces memories with *stakes*, held by NPCs who have reason
to lie about them.

Three things make it worth building:

1. **It is the highest-value memory source in the plugin.** A rumor that Maven Black-Briar is quietly buying up
   Riften's debt is worth more to an LLM-driven conversation than a hundred memories about meals and weather —
   and it is worth more still to the NPC who was ordered to do the buying and now has to explain a shortfall.
2. **It gives gossip something to carry.** Phase 13 spreads whatever high-notability memories exist; today those
   come from the player's own vicinity. Plots generate notable events across the province with no player nearby,
   which is precisely the input the propagation model was tuned for and has been starved of.
3. **It is the first system that can hand the player a task that already had a reason to exist.** Every beat so
   far invents its own pretext at compose time. A plot step is a pretext the world generated on its own, days
   before the player heard about it, with a person behind it who wanted it done for their own reasons.

---

## Scope

### In scope

- A **plot object model** — mastermind, objective, plan, ordered steps, adaptation on failure, terminal
  success/failure.
- A **casting system** that picks masterminds and step agents from the existing unique-NPC population, weighted
  by rank and resources, and enforces the one-role-per-NPC rule.
- **Step resolution** over in-world time: dispatch, continue, succeed, fail.
- **Memory writes** at every resolution point, for every party who would plausibly know.
- A **bounded set of world mutations** a plot is allowed to make (see "What a plot writes").
- **Player delegation**: a step the sim would rather hand to the player, offered to the Director as a beat
  candidate, expiring silently if the Director never fires it.
- A **player-facing errand quest** with a time limit and a standing penalty for refusal or failure.
- A **Plots tab** in the PrismaUI dashboard: the live state of every plot, its steps, and the mechanical inputs
  behind each outcome.

### Deferred (explicitly out)

- **Plots that target the player directly.** A plot may involve the player as *labour*; a plot whose objective
  is the player is a different and much sharper feature.
- **Inter-plot interaction.** Two masterminds with conflicting objectives is a great idea and a combinatorial
  hazard. First get one plot working end to end.
- **A journal record of plots.** Nothing this feature does writes a quest-log entry describing a scheme, and
  there is no in-fiction ledger of what the player has pieced together. What the player *knows* about a plot,
  they know because an NPC told them, which means it lives in the memory store like everything else. The
  dashboard's Plots tab (Part 10) is an out-of-fiction instrument and is explicitly not this.
- **Faction-level state.** No faction treasury, no territory, no standing between factions. The mastermind is a
  person with resources, not an institution running a sim.
- **Plot-driven combat.** No spawned assassins, no ambushes dispatched by a plot. Ambush is an existing beat and
  can stay independent until there is a reason to wire them together.

Everything in the **In scope** list is in scope for the *feature*, not for any one phase. The staging section
says which phase each part lands in.

---

## Design

### Part 1 — Objects

**Plot** — one scheme, one mastermind:

| Field        | Meaning                                                                                              |
| ------------ | ---------------------------------------------------------------------------------------------------- |
| `id`         | Stable identifier, persisted                                                                          |
| `mastermind` | FormID of a unique NPC. Exactly one, never a faction                                                  |
| `ambition`   | LLM-authored sentence: the larger thing this objective serves. Flavour, but it anchors adaptation |
| `objective`  | A typed objective drawn from the taxonomy below, plus a resolved target                               |
| `plan`       | Ordered list of steps, authored at plot birth, rewritten on failure                                   |
| `cursor`     | Which step is live                                                                                    |
| `history`    | Completed and failed steps, capped                                                                    |
| `state`      | `Active` / `Succeeded` / `Failed`                                                                     |

**Step** — one unit of work:

| Field        | Meaning                                                                          |
| ------------ | -------------------------------------------------------------------------------- |
| `type`       | From the fixed taxonomy below                                                    |
| `target`     | A pre-resolved FormID (actor / location / item / faction), never a free string    |
| `agent`      | Who is doing it: a subordinate NPC, the mastermind, or the player                 |
| `state`      | `Planned` / `AwaitingPlayer` / `InProgress` / `Succeeded` / `Failed`              |
| `budget`     | Ticks the actor has before the step times out. Sized at dispatch — see Part 6     |
| `elapsed`    | Ticks spent so far, whether or not they produced progress                        |
| `threshold`  | Total progress the step requires                                                 |
| `progress`   | Progress accumulated so far                                                      |

**Step taxonomy — the manifest.** Fixed, small, and closed. It has to be closed because every type needs a
mechanical resolution, and it is handed to the LLM verbatim as the vocabulary it must plan in:

| Type        | Shape                                               | Deliverable | Conspicuous |
| ----------- | --------------------------------------------------- | ----------- | ----------- |
| `Locate`    | Find where a thing or person is                     | Yes         | No          |
| `Acquire`   | Obtain an object                                    | Yes         | Sometimes   |
| `Deliver`   | Move an object or message to someone                | Yes         | No          |
| `Surveil`   | Watch a person or place, report back                | Yes         | Yes         |
| `Suborn`    | Buy, recruit, or blackmail someone into cooperation | Later       | Yes         |
| `Discredit` | Damage a rival's standing or leverage               | Later       | Yes         |
| `Sabotage`  | Impair a thing, a shipment, an arrangement          | Later       | Yes         |
| `Conceal`   | Cover the tracks of a step already taken            | Later       | Sometimes   |

**Deliverable** is which types can be handed to the player, and it is a *starting* set rather than a property of
the type — see Part 8. **Conspicuous** is whether the step would unavoidably be noticed if the player were
standing there, which gates when it may resolve — see Part 6.

**The manifest is the objective vocabulary too.** The last step of a plan is the one that accomplishes the
objective, so an objective is just a step type at terminal difficulty against a high-value target — which means
the objective must come from the same manifest the steps do. The birth call is handed the manifest once and
asked for the objective *and* the ladder of steps beneath it in a single response: you cannot `Acquire` what
you have not `Located`, you cannot `Discredit` without `Surveil` first.

A useful consequence: adding a step type to the manifest automatically widens what plots can be *about*, with
no separate objective taxonomy to keep in sync.

**The objective is fixed for the life of the plot.** Adaptation rewrites the path, never the destination —
otherwise a plot quietly redefines its goal to whatever it can still reach and no plot ever fails.

**Occupancy.** One table, keyed by NPC FormID, holding at most one `{plot, role}`. Masterminding one plot and
executing a step for another are both occupancy, so the rule "a given unique NPC can only mastermind one plot at
a time or enact one step at a time" is a single uniqueness constraint rather than two.

### Part 2 — The tick

The sim advances on **in-world time**, like gossip and unlike the Director. A step takes in-world days; nothing
about it is meaningful in wall-clock terms.

**Cadence: one tick every 12 in-game hours.** Settable, but that is the design point — two ticks a day is slow
enough that a plot takes in-world weeks to run, and fast enough that a player who travels and sleeps normally
sees it move.

**Budget: up to 10 concurrent plots.** Also settable. A plot holds a slot from birth until it reaches a terminal
state, and frees it the moment it does.

Per tick, before any plot is advanced:

```text
budget free? → select a mastermind, generate a plot from their ambitions and memories, take the slot
```

Birth is therefore opportunistic rather than scheduled: the world runs at whatever plot density the completion
rate sustains, and a spell of long, grinding plots naturally suppresses new ones. Selection picks the
**mastermind first** (see Part 5), then generates the plot from *that person's* ambitions and memories — not
the reverse. A plot is an expression of who is running it.

Then, per plot, per tick:

```text
no live step   → plan not exhausted?  dispatch next step (cast, size budget + threshold, dispatch memories)
               → plan exhausted?      the objective was the last step: plot Succeeds
live step      → ++elapsed, always
               → conspicuous, and actor or target near the player?  no progress this tick
               → otherwise                                          roll progress, add to the total
               → mishap roll                                        caught? Fail now
               → progress >= threshold?                             Succeed
               → elapsed >= budget?                                 Fail: ran out of time
step Succeeded → write resolution memories, advance cursor
step Failed    → write resolution memories, then ADAPT
```

Note that `elapsed` advances on every tick including a held one. A conspicuous step blocked by the player's
presence is not paused; it is burning its budget while making no headway, which is what gives the player's
presence teeth without needing a rule of its own.

**Adapt** is the interesting one and the core requirement: a failed step is not a failed plot. The mastermind
gets one LLM call with the plan so far, what failed and why, and the remaining approaches, and returns either a
revised remaining plan or a concession that the objective is now unreachable. The concession must be a real
option the prompt offers, or the model will invent infinite alternatives and no plot will ever end. A hard cap
on total adaptations per plot backstops it regardless of what the model says.

**Ticks are stamped and never coalesced.** Sleeping through 24 in-world hours crosses two 12-hour boundaries
and runs **two ticks**, not one tick with twice as much to do — the second one seeing the world the first one
left behind. Each queued tick carries the game time it was *supposed* to fire at and reasons as of that moment,
which is what decouples when a tick runs from what it processes and lets the queue back up behind a slow LLM
call without the simulation drifting. This is `GossipTick`'s scheme and it transfers wholesale.

The backlog is what needs bounding, not the individual tick: past some number of outstanding ticks the
scheduler stops enqueuing and advances the schedule without doing the work, so a console time jump cannot queue
a year of simulation. Part 11 has the mechanics.

### Part 3 — Where the LLM sits, and where it must not

The discipline gossip established is the right one and this design inherits it: **the LLM authors, dice
resolve.** An LLM call happens at a small number of narrative junctions and never on a per-tick basis.

| Junction        | Call                                                    | How often                    |
| --------------- | ------------------------------------------------------- | ---------------------------- |
| Plot birth      | Read the mastermind's memories, pick objective + plan    | Once per plot                |
| Step dispatch   | Memory text for the mastermind and the step actor        | Once per step                |
| Step resolution | Memory text for both, plus the target if they noticed    | Once per step                |
| Adaptation      | Rewrite the remaining plan after a failure, or concede   | Once per failure             |
| Player offer    | Compose the ask in the requesting NPC's voice            | Once per offer that fires    |

Everything else — how much progress a tick yields, who gets cast, when a step times out — is mechanical. Nothing is
per-tick: a step spanning several ticks costs the same two calls as one that resolves immediately.

**The cost, honestly.** Two calls per step is the dominant term, so a five-step plot costs about eleven calls
rather than the one an earlier draft of this document claimed. Sketching it out: if a step spans a day or two
of in-world time, a five-step plot runs about a week and 10 concurrent plots produce on the order of **fifteen
calls per in-world day** — roughly one every few real minutes at default timescale, on top of the Director's
own cadence and gossip's.

That is affordable but it is not free, and it is worth stating rather than discovering. Two things keep it
bounded: the plot budget is a hard ceiling on concurrency, and step duration is the lever that trades memory
density against call volume. If the rate turns out to be too high in practice, lengthening steps costs nothing
narratively — plots are *supposed* to be slow.

**Plot birth reads the mastermind's memories.** This is what makes a plot theirs rather than a generic scheme
assigned to a name: the birth prompt is handed what that NPC actually knows, through the same SkyrimNet memory
query the rest of the plugin already uses. It also closes a loop with Phase 13 — a mastermind can scheme about
a rumor they heard, and the rumor they heard can be the outcome of somebody else's plot.

### Part 4 — Targets: the menu, never the guess

This is the failure mode most likely to sink the feature, and it is already documented:
`docs/prior-art/NAME_RESOLUTION_FAILURE_MODES.md` traces what happened in IntelEngine when an LLM named an
entity and a fuzzy resolver tried to find it.

**Rule: the LLM never names a target. It picks one from a list we hand it, by index or id, and every entry on
that list is already a resolved FormID.** The step manifest constrains the *verbs* the same way this constrains
the *nouns*; between them, a returned plan is a sequence of known types against known forms, and validating it
is a membership test rather than a resolution problem. The planning prompt receives candidate actors (from the existing
`GossipGraph` participant set, filtered by relevance to the mastermind), candidate locations, candidate
factions, and — for `Acquire` — a **curated item pool**, because "the LLM decided the objective is an artifact"
must resolve to a real `TESForm` and cannot be allowed to mean anything else.

If the model returns something not on the menu, the plot is rejected at birth rather than repaired. Rejecting is
cheap; a plot pointing at a target that does not exist poisons every memory it later writes.

### Part 5 — Casting

The population is the one `GossipGraph` already builds. There is no reason to index unique NPCs twice, and its
`Participant` records already carry residence, hold, and the faction and relationship edges casting needs.

**Mastermind weight** rises with the resources an NPC commands. The obvious signal is faction rank within a
faction that is large and prominent enough to matter — gossip already filters factions by size and name for its
own purposes, and that filtered set is a reasonable starting definition of "prominent." Independent NPCs stay
eligible at a low but non-zero weight, per the requirement; what independence costs them shows up in casting,
not in eligibility.

**Agent selection**, per step: prefer a subordinate — a lower-ranked member of a faction the mastermind ranks
highly in, preferably one with a personal edge to the mastermind. Fall back to a personal edge without shared
faction. Fall back last to the mastermind doing it themselves, which is how independents operate by default.
Who ends up doing the work should visibly reflect the mastermind's position: the Thieves Guild has options, a
Riverwood farmer has themselves.

An NPC who is dead, or who is already occupied, is not castable. If the **mastermind** dies, the plot fails
immediately — that is a genuinely good outcome and one the player can cause without ever knowing they did.

**Cooldowns, separately for each role.** Finishing a plot puts an NPC on a mastermind cooldown; finishing a
step puts them on a step-actor cooldown. Both are short — long enough to force a rotation through the eligible
population, not long enough to take anyone out of circulation for a meaningful stretch. They are separate
values because the roles recur at completely different rates: a plot ends every week or two, a step every day
or two, and one cooldown covering both would either lock masterminds out for far too long or fail to spread
step work at all.

Mechanically this is the occupancy table's job — the same row that says who is busy carries a per-role
"available again at" game-time stamp, so occupancy and cooldown are one lookup rather than two structures that
can disagree.

### Part 6 — Resolution

A step is not a coin flipped at a deadline. It is a **race between accumulating progress and a running
clock**, and both sides of that race are fixed when the step is dispatched.

**The budget** is how many ticks the actor gets, and it comes from *circumstance*: chiefly how far they must
travel to reach the target, plus whatever scale the step type carries inherently. It is deliberately **not** a
competence figure. A capable actor and a hopeless one sent on the same errand to the same place get the same
budget; what differs is what they do with it.

**The threshold** is how much work the step represents — a function of the step type and the target's
importance.

**Progress accrues per tick, on a roll modified by the actor.** Their relevant skills and attributes (already
on the `TESNPC`), and their suitability for this kind of step, shape how much ground they cover each tick. The
step **succeeds** the moment accumulated progress reaches the threshold, and **fails** if the budget runs out
first.

Three properties this buys that the single roll it replaces did not have:

- **Step duration stops being a number anyone has to pick.** It emerges: a skilled actor with a short trip
  finishes in two ticks, a poor one sent across the province may never finish at all. This is the answer to
  "how long is a step" — the question is deleted rather than tuned.
- **A step acquires an interior.** There is something true to say about it on every tick, which is what makes
  it inspectable on the dashboard now and dramatizable in front of the player later. A coin waiting to be
  flipped has no interior.
- **Near-misses become legible.** A step that reached 90% of its threshold and ran out of days is a genuinely
  different event from one that never got going, and that difference is exactly what adaptation should be
  reasoning over.

Two constraints on the arithmetic:

- **Clamp the per-tick roll at both ends.** It should never be zero on an unblocked tick, and never large
  enough to clear the threshold in one — the latter collapses the model back into the single roll it replaces.
- **Budget and threshold must be independently derived.** If the budget is computed from the threshold, every
  step has identical odds and the race is theatre. The budget answers "how long is the trip"; the threshold
  answers "how hard is the job"; they are allowed to be badly matched, and a badly matched pair is a step the
  mastermind should not have ordered.

**Failure must be typed, not just boolean.** "Ran out of time" and "was caught in the act" are different inputs
to adaptation and produce very different memories — being caught is what puts the mastermind's name in someone
else's mouth, which is what makes a plot leak into gossip. The outcome type also decides whether the **target**
gets a memory of the step at all: someone who was spied on badly noticed, someone who was spied on well did
not.

Timeout failure types itself from how far the actor got. Getting *caught* does not — it is orthogonal to
progress, and a proposal rather than a settled mechanic: **a second small per-tick roll for mishap**, weighted
up by the step's conspicuousness and down by the actor's competence, which ends the step immediately when it
lands. It is the only obvious way to preserve a distinction the design already depends on, but the shape is
worth arguing about before it is built (see Open questions).

**A conspicuous step will not resolve in front of the player.** Some steps could plausibly happen unobserved
anywhere; others — watching someone, leaning on someone, wrecking something — could not credibly resolve as a
die roll while the player is standing next to the people involved. The manifest marks which is which.

The short-term rule is a refusal, not a dramatization: **a conspicuous step makes no progress on any tick
where its actor or its target is in the player's loaded area.** The presence check is a plain per-actor
loaded-state read, which is safe off the main thread under the codebase's documented precedent — unique NPCs'
`Actor` objects are always resident and only their 3D unloads, which is the same property gossip relies on for
its life checks.

The progress model absorbs this without a special case. A held tick is simply a tick that yields nothing while
the budget still burns, so a step blocked long enough fails by **running out of time** like any other — no
separate "overtaken by events" path, no risk of a held step living forever. That is a deliberate consequence
rather than a fallback: a player who parks in Riften for a week disrupts schemes involving Riften NPCs, which
is a better outcome than either freezing the plot indefinitely or resolving it implausibly offscreen.

The eventual answer is to let a conspicuous step **play out in front of the player** as something they witness,
which is the natural meeting point between this system and the beat system. That is a later iteration; the
refusal rule is what makes it safe to defer.

### Part 7 — What a plot writes, and what it may actually change

**Memories are the primary output**, via `SkyrimNetAPI::AddMemory`, written at two points in every step's
life. Each point is **one LLM call that composes every party's memory together**, rather than a call per person
— the parties' accounts have to be consistent with each other to be worth anything, and composing them in one
response is what makes that automatic.

**At dispatch**, for the mastermind and the step actor: the order being given, before either knows how it will
go. Giving them a memory of the *asking* is what lets an NPC be mid-task rather than only ever pre- or
post-task.

**At resolution**, success or failure, for the same two — plus, conditionally, the **target**. Whether the
target gets one is decided by the outcome type from Part 6: a badly-executed `Surveil` means the person being
watched noticed the watcher, a clean one means they never knew. That conditional is where most of the feature's
paranoia comes from, and it costs nothing extra because the call is already being made.

The mastermind's account and the actor's need not agree. A subordinate who failed has a reason to shade the
report, and a single composing call can write both sides of that divergence knowingly — which a per-person call
could not do.

All plot memories get their own tag, so that — exactly as gossip does with `GossipHarvest::kOwnOutputTag` — plot
output can be kept out of any candidate set it should not feed. It should, however, be explicitly *eligible for
gossip seeding*: a step that failed and was caught is the highest-notability thing this plugin can produce, and
gossip already exists to carry it. That integration is a stated design goal even if it lands later.

**World mutation is deliberately narrow and Alpha Canon binds it absolutely.** A background sim that can kill
named NPCs, empty containers, or move quest items is a save-corrupting liability and a direct violation of the
plugin's founding principle. The allowed set should start at roughly:

- Add an item from the curated pool to an NPC's inventory (the `Acquire` payoff).
- Adjust a relationship rank between two NPCs.
- Nothing else without a specific argument.

Notably, **`Discredit` and `Sabotage` resolve entirely in the memory layer**. Nobody dies. The world's *account*
of what happened changes, NPCs act on that account through SkyrimNet, and that is the whole effect. If a plot
should ever be able to genuinely remove an NPC, that is a beat the player witnesses, not a die roll resolved in
a hold the player has never visited.

### Part 8 — Player delegation

The requirement: some steps can be handed to the player, and whether that offer ever reaches them is the
**Director's** call, not the sim's.

**Eligibility**, computed when a step is dispatched. Both conditions are checkable from data the plugin already
has:

- The player is a member of the mastermind's faction at a lower rank than the mastermind, **or**
- the cast NPC agent has a personal relationship with the player.

Plus: the step type must be one of the currently player-deliverable ones.

**Which types those are is a staging decision, not a property of the type.** `Locate`, `Acquire`, `Deliver` and
`Surveil` have obvious completion conditions and are the natural first set. The rest are marked *Later* in the
manifest because their completion conditions are harder to detect, not because they would make bad errands —
`Suborn` in particular is an excellent errand ("go convince this person") and a genuinely hard thing to know
the player has done. Turning types on one at a time is the intended path.

**Rank weights the offer, it does not gate it.** A player standing high in the mastermind's faction is *more
likely* to be picked for a step than a new recruit, and closeness to the step's actor pulls the same way. But
no rank threshold locks the player out: a rank-1 Companion being handed something well above their station is
an interesting event, not a bug, and hard gates would make the whole surface predictable.

**The offer, and why it does not block the plot.** An eligible step enters `AwaitingPlayer` and registers an
offer with an in-world expiry — the fixer is holding the job open, hoping to catch you. The Director sees a beat
candidate. Then:

```text
Director fires the beat before expiry → the player is asked; the step becomes theirs
Director never fires it               → at expiry the step silently reverts to the NPC agent and resolves
                                        normally. No memory, no trace, no evidence an ask was contemplated
```

That second branch is the important one. The sim is never waiting on the Director and never degraded by its
silence — the offer is an opportunity the Director may decline to take, which is exactly how every other
tension-shaping decision in this plugin works.

**The beat.** A **new `IBeat`**, distinct from the visit beat but executing the same way one does: the
requester is designated, travels to the player, and opens a conversation. `IsAvailable` is true when the offer
pool is non-empty and the requester can plausibly reach the player.

It is a separate beat rather than a visit variant because the Director has to be able to reason about it as its
own thing — it carries an obligation the player can decline, which is a different narrative object from a
social call, and it needs its own `Description()` for the beat-select prompt to weigh. But it should reuse the
visit beat's approach-and-talk machinery rather than reimplement it; where that machinery is beat-specific
today, lifting the shared part out is part of the delegation phase's work, not a copy-paste.

**The quest.** This is the genuinely new surface area in the phase, and it should stay as small as it possibly
can: one reusable CK quest with a handful of aliases (giver, target actor, target item, target location),
objective text set at runtime from the step, and completion detected by polling — the same shape the visit beat
already uses for its own completion detection. Not a stage-and-dialogue quest per step type.

**Time limit and consequence.** The player's copy of the step carries **the same budget the NPC would have
had**, but its countdown starts when the player is *affirmatively given* the quest — not when the step was
dispatched, and not when the offer entered the pool. Time the offer spent waiting for the Director to fire is
not the player's to lose, and a player cannot be penalised against a clock that was running before they had
heard of the job.

While the player holds the step, it is theirs alone: the NPC agent is not also working it in parallel, and the
plot advances on the player's progress or not at all. Three outcomes:

| Outcome                          | Effect                                                                   |
| -------------------------------- | ------------------------------------------------------------------------ |
| Completed in time                | Step succeeds. Memories on player, requester, and mastermind. Standing up |
| Refused outright                 | Step reverts to the NPC agent. Modest standing loss with the requester    |
| Accepted, then failed or expired | Step fails, plot adapts. Larger standing loss — you took the job          |

Refusing should cost less than accepting and failing. That asymmetry is what makes the choice a choice.

**Standing is two things, always both.** A change to the **vanilla relationship rank** between the player and
the requester, *and* a **memory** on the requester recording what the player did. The rank is what makes other
systems — vanilla dialogue conditions, SkyrimNet's own disposition handling, other mods — react without knowing
this plugin exists; the memory is what lets the requester actually talk about it.

**The rank change is floored.** Declining errands must never drive an NPC to the bottom of the relationship
scale: a shopkeeper the player has turned down three times is annoyed, not a nemesis. The floor sits a rung or
two below neutral, and the penalty saturates there no matter how many offers are refused. The exact enum value
to clamp at should be read off CommonLibSSE's relationship-level enum at implementation time, not guessed here.

**Success raises rank too, and is capped the same way.** Otherwise the errand system becomes a grind for free
affinity — a ceiling is the same argument as the floor, pointed the other way.

**What the player is told** is however much the requester is willing to say, which is a real variable and not
a constant. Two independent things set it:

- **What the requester actually knows.** A subordinate handed a step three rungs down may know only the errand.
  Someone close to the mastermind may know the objective, and occasionally the ambition behind it. This falls
  out of the casting relationship — rank distance from the mastermind, and whether there is a personal tie —
  and it is a property of the *step's agent*, not of the plot.
- **How forthcoming they are.** Knowing and telling are different. A well-informed agent may still hold back
  from a player they barely know, and a talkative one may name the mastermind unprompted. Shaped by their
  standing with the player and their own disposition.

So the floor is the errand itself — fetch this, watch that, carry this there — and the ceiling is everything
the requester knows, up to and including "Maven wants the Jarl's debts and this ledger is how she gets them."
Both are legitimate outcomes of the same offer.

**Implementation shape: a disclosure budget, by omission.** The offer-composition prompt is handed the set of
facts this NPC may reveal, and facts outside that set are simply **absent from the prompt** rather than present
and marked secret. A model told "do not mention the mastermind's name" will mention the mastermind's name. What
it is never given, it cannot leak.

Whatever the requester does disclose should land in the player's memory of that conversation, so the player can
follow it up later with someone else — which is the entire reason this plugin writes memories instead of quest
journal entries. The *quest objective text* stays the errand regardless; disclosure lives in conversation, not
in the journal.

### Part 9 — Persistence and bounds

Its own SKSE co-save record. Bounded by construction rather than by hope, following Phase 13's stated policy:

- The concurrent-plot budget: **10**, as a setting. That is the pool of slots Part 2's birth rule draws
  against.
- A cap on steps retained in a plot's history.
- A reaping policy for terminal plots. A finished plot's *memories* persist in SkyrimNet regardless; the plot
  record itself has no reason to outlive it by long.

### Part 10 — The dashboard

The PrismaUI dashboard gets a **Plots tab**, beside Gossip, hidden from the tab bar when the feature is
switched off — the existing `TabBar` `hidden` convention, which drops a disabled subsystem's tab rather than
greying it out.

It reads the **published snapshot**, never the live state, for exactly the reason gossip does: a read taken
mid-tick shows a half-advanced simulation, with some plots stepped to the new game day and some not.

**It is a debugging instrument first.** A tab that renders only the fiction — who is plotting what against whom
— is pleasant and cannot diagnose anything. Every mechanical quantity that decided an outcome has to be on
screen:

- **Budget occupancy** (n / 10), and how long the free slots have been free.
- **Per plot:** mastermind, objective, ambition, plan with the cursor marked, current step and its agent,
  adaptations spent against the cap.
- **Per live step:** progress against threshold and elapsed against budget, as two bars side by side. The
  shape of the race is the whole diagnosis — a step at 20% progress and 80% of its budget is already lost, and
  that should be visible before it resolves rather than inferable afterwards.
- **Per resolved step:** the per-tick roll history that produced the outcome, plus the inputs that sized the
  budget and threshold in the first place (travel distance, step scale, actor competence, suitability). "It
  failed" is not a debuggable statement; "it failed at 0.62 of its threshold, having been held for four of its
  nine ticks" is.
- **Casting that did not happen:** which candidates were considered for a step and why each was passed over
  (occupied, dead, no tie to the mastermind). Casting is where a sim silently degrades into "the same six NPCs
  do everything," and that is invisible unless the rejects are shown.
- **Terminal plots**, retained until reaping. The question worth answering after the fact is almost always
  *why did that one fail*, and the answer is gone if the row disappears the moment the plot ends.
- **Delegation state:** offers currently in the pool, their expiry, and whether the Director has passed on them
  yet — the one place where "the Director never fired it" becomes visible rather than silent.

This tab is also the **primary verification surface for Phase A**. Reading it over accelerated in-world time is
how the resolution math, the birth rule and the casting distribution get judged before any content is attached.
It is not a nice-to-have that trails the implementation; it is how the implementation gets checked.

### Part 11 — Threading

Nothing new is invented here. This is the background-sim pattern the plugin already runs three times
(`AsyncDispatch`, `EvalDispatch`, `GossipDispatch`), and plots are the fourth instance of it.

**The plugin thread does the cadence check and nothing else.** A `Poll(const PluginThread::Token&)` entry point
hangs off `PollOnPluginThread` alongside the existing subsystem polls, and therefore runs on the driver's
existing 500 ms cadence with no timer of its own.

It takes **no elapsed-seconds argument**, because this feature's cadence is in-game time and nothing else: the
poll samples the game clock, compares it against the stamp of the last tick it enqueued, and enqueues one
stamped job per 12-hour in-world boundary crossed since. There is no real-seconds accumulator to keep and no
pause-awareness to get right — a paused game does not advance the game clock, so the comparison is simply never
satisfied while paused.

That also means the 500 ms poll rate is not a tuning parameter for this feature. It only bounds how promptly a
crossed boundary is noticed; sleeping eight hours advances the clock in one jump and the poll that observes it
enqueues every boundary that jump crossed. Microseconds of work, no locks on plot state, no possibility of
blocking — the only plot work that happens on the plugin thread at all.

**A dedicated serial worker runs the simulation.** Its own thread and its own FIFO queue, one job at a time in
enqueue order. The whole of a tick runs there start to finish: birth (including its LLM call), step dispatch,
resolution, adaptation (including its LLM call), memory writes, pruning, and the snapshot publish. Because the
only things enqueued on it are later plot work, **a job may block for as long as it needs to** — a synchronous
LLM round trip included — without any other subsystem noticing. That is the entire reason the pattern exists,
and it is why plot birth and adaptation do not need the async LLM path.

**Its own thread token.** A `PlotThread::Token`, minted only by that dispatcher, gating every function that
touches live plot state — the same argument `GossipThread::Token` makes, and the same payoff: the simulation
carries no mutex, and reaching it from `AsyncDispatch`, a SkyrimNet callback, or the main thread is a compile
error rather than a rule someone has to remember. The worker declares `ThreadRole::Plugin`; this is a narrower
capability within the plugin role, not a fourth role.

**Engine mutation hops to main.** The two world effects this feature is allowed (an item into an inventory, a
relationship-rank change) touch `TESForm` state and cannot happen on the worker. They go through
`MainThread::Run` / `MainThread::FireAndForget` like any other engine-touching work from a worker thread.

**Cancellation at operation boundaries, not just at the end.** Loading a save must cancel outstanding and
running plot jobs, and the job must check between operations rather than only before publishing. The reason is
the one `GossipDispatch` documents and it applies with more force here: a plot tick writes memories into
SkyrimNet's database, which lives outside our co-save and is **not** rolled back by loading an earlier game — a
tick that keeps running past a load keeps writing memories about a plot the loaded world has no record of. Plots
additionally mutate inventories and relationship ranks, so the same tick can leave engine state behind too.
Discarding results at the end would disown those writes without preventing them.

**Publish a snapshot at the end of a job, never during one.** The dashboard and the co-save read an immutable
published image, for the reason Part 10 gives: a read taken mid-tick shows some plots stepped and some not.

Every free-form string returned by an LLM goes through `LLMTextSanitizer::Sanitize` at the point of extraction
from the response JSON, before it is stored, persisted, or fed anywhere downstream.

#### The seam with the beat system

The beat half runs on `BeatSystem`'s own worker, not on the main thread: `ConsiderBeat` and `IsAvailable` are
plugin-thread, `IBeat::Tick` runs on the beat worker, and engine-touching work inside a beat marshals to main
itself. The errand beat plays by those rules like every other beat and needs no special handling.

What does need care is the **offer pool**, which the plot worker produces into and the beat path consumes from
— two different threads, and the pool cannot be gated by `PlotThread::Token` if the beat side has to read it.
The clean split:

- **Reads come from the published snapshot.** `IsAvailable` asks the last published image whether any offer is
  live. It is already required to be cheap and side-effect free, and a snapshot read is both.
- **The claim is enqueued back onto the plot worker.** When the Director actually fires the beat, taking the
  offer is a state transition on plot state, so it happens where that state lives rather than under a lock
  reached from outside. The beat learns whether it won the offer from the job's result.

That keeps every mutation of plot state on the one thread that owns it, and leaves the pool with no lock of its
own — the same shape the rest of the subsystem uses, rather than an exception carved out for the player-facing
half.

---

## Implementation staging

Four phases, each of which leaves the plugin in a working, shippable state and is verifiable on its own. The
ordering follows one rule: **the parts that produce the feature's actual value come before the parts that are
expensive to build.** Player delegation is the most visible half of this feature and also the only half that
needs new ESP surface, so it goes last — everything before it is C++ that can be observed from the log and the
dashboard.

The Phase 13 precedent is deliberate here. Gossip built a headless validation harness first and only then
attached content, and that ordering is what caught a propagation model that tuning could not have fixed.

### Phase A — the simulation, headless

The plot and step model, the **dedicated worker and its thread token** (Part 11), the plugin-thread cadence
check, casting from `GossipGraph`, the occupancy table with its per-role cooldowns, the progress-race
resolution model, the conspicuous-step presence gate, the plot budget and birth rule, co-save persistence, the
log, and **the dashboard's Plots tab** (Part 10). **Objectives and plans are stubs** — drawn from a
hardcoded table, not authored. No LLM, no memory writes, no world mutation.

The worker comes first even though nothing yet blocks on an LLM call: retrofitting a token discipline onto
state that was written without one means touching every function that reads it.

The tab is not deferred to a later polish pass. It is the instrument this phase is verified with, so it ships
with the machinery it observes.

*Verifiable by:* running the sim over accelerated in-world time and watching the Plots tab and the log. Do
plots start, advance, adapt and terminate at sane rates? Does the budget stay saturated without starving? Does
the occupancy rule hold? Are step outcomes distributed the way the resolution math intended? Is casting
spreading across the population or converging on the same handful of NPCs? Every one of those is answerable
without a single LLM call, and all of them are cheaper to fix here than after content is attached.

### Phase B — LLM authoring

The plot-birth prompt (memory-grounded, menu-constrained targets), the adaptation prompt, the target menus,
`LLMTextSanitizer` on every returned string, and rejection-at-birth for off-menu answers. Stubs from Phase A
are replaced. Still no memory writes and no world mutation.

*Verifiable by:* reading generated plots. Do they read as things *that specific NPC* would want? Does the
target menu hold — has anything off-menu ever been accepted? Does adaptation actually concede when it should,
or does it spiral until the cap catches it?

### Phase C — memories and world effects

The dispatch-time and resolution-time memory calls (Part 7), the conditional target memory, the plot-output
tag, gossip-seeding eligibility, the curated item pool, and relationship-rank changes with their floor and
ceiling. This is also where the LLM call volume becomes real, so it is the first phase where the rate in
Part 3 can actually be measured rather than estimated.

This is the phase where the feature first pays off with no player participation at all: NPCs across the
province now hold memories about schemes, and gossip carries the loud ones. It is worth playing on its own.

*Verifiable by:* talking to an NPC who was cast in a plot and seeing whether they can discuss it coherently;
watching a caught failure propagate through gossip.

### Phase D — player delegation

The offer pool, eligibility, expiry-and-revert, the new `IBeat` (and lifting the shared approach-and-talk
machinery out of the visit beat), the reusable CK errand quest with its aliases and runtime objective text,
completion polling, the disclosure budget on the offer prompt, and the standing consequences on the player
side. Player-deliverable step types ship as the `Locate` / `Acquire` / `Deliver` / `Surveil` set; the rest are
follow-on work, not part of this phase.

The only phase with new ESP surface, and the one whose verification needs actual play. It should not start
until A–C are stable, because a bug in the sim reached through a quest is far harder to diagnose than the same
bug in a log.

*Verifiable by:* being offered an errand, completing it, refusing one, and taking one and letting it expire —
and confirming the plot behind it responded correctly in all four cases.

### Later, unscheduled

MCM exposure for the settings this feature adds, and whatever tuning the first real play sessions demand. Not
a phase until there is something to tune.

---

## Open questions

These are the decisions the design deliberately does not make yet.

1. *(Phase A)* **Is the conspicuousness split right?** The manifest's `Conspicuous` column is a first guess.
   `Acquire` and `Conceal` are marked *Sometimes* because they depend entirely on the target — lifting a ledger
   from an empty study is not the same act as lifting it off a belt. That may need to be a per-step property
   decided at dispatch rather than a per-type constant.
2. *(Phase A)* **How does a step get caught?** Part 6 proposes a second per-tick mishap roll, weighted by
   conspicuousness and competence, because typed failure is load-bearing — it decides whether the target gets a
   memory and whether the plot leaks into gossip — and progress alone cannot express it. The shape is not
   settled: it could equally be a property of *how* the progress roll failed, or something only conspicuous
   steps are exposed to at all.
3. *(Later)* **What does a conspicuous step look like when it plays out in front of the player?** The refusal
   rule in Part 6 defers this cleanly, but it is the most interesting thing this feature could eventually do
   and the design should not accidentally foreclose it. The progress model helps here: a step already has an
   interior to dramatize, rather than a single hidden roll.

---

## What this feature is not

Not a faction politics simulation. There is no institution modelled anywhere in this design — only people with
resources, ambitions, and subordinates. IntelEngine had a faction-politics sim and it is the wrong shape for
this: what NarrativeEngine needs out of tier 3 is *memories with stakes*, and a person scheming produces those
far more cheaply than a simulated institution does.
