# Phase 17 — Milestone 3: Plot Memories

Milestone 2 ([`PHASE_17_MILESTONE_2_RIGID_STEPS.md`](PHASE_17_MILESTONE_2_RIGID_STEPS.md)) made every step a verb with
fixed sentence templates and wrote memories from them at dispatch, at resolution and on giving up. Those memories cover
the agent completely and the mastermind only in part, and the people a step is done to remember nothing. This
milestone fills in the rest, so that everybody a plot touches can talk about it.

> **Doc status: planned; no step started.** Steps 1–2 complete the mastermind's side, Steps 3–5 give the people a
> step reaches memories of their own, and Step 6 is the in-game check. Phase 17's Steps 20–22 (world effects, gossip
> seeding and their validation) still follow this milestone.

---

## Why this milestone exists

What plots produce is memory, and an NPC can only talk about a plot they remember. Today:

| Who            | Event                                                       | Remembered                                                                                |
| -------------- | ----------------------------------------------------------- | ----------------------------------------------------------------------------------------- |
| **Mastermind** | Sending an agent, and who                                   | Yes: `mastermind_dispatched`, or `self_dispatched` when they act themselves               |
|                | The agent succeeded                                         | **No.** Milestone 2's open question 5                                                     |
|                | The agent failed, caught or out of time                     | Yes: `mastermind_told_failed`, `mastermind_told_timed_out`                                |
|                | Went on by another way after a failure                      | **No**                                                                                    |
|                | Gave up, or ran out of attempts                             | Yes: `mastermind_conceded`                                                                |
|                | Brought the scheme off                                      | **No**                                                                                    |
|                | Failed at the finish                                        | **Only** the step's "reported back" memory; nothing says the scheme is over               |
| **Agent**      | Being sent, or acting for themselves                        | Yes: `agent_dispatched`, `self_dispatched`                                                |
|                | Succeeded, ran out of time, was caught                      | Yes: `agent_succeeded`, `agent_timed_out`, `agent_failed`                                 |
| **Target**     | Anything                                                    | **No.** Milestone 2's open question 4. They are only listed as related on others' memories |

**A plot's creation is deliberately not remembered.** The mastermind's first dispatch memory already says what they
set out to do -- "I sent Aeta to steal a ruby from Hulda, as part of my plan to ruin Hulda" -- and a second memory
saying only that they decided to ruin Hulda would repeat it.

---

## Design

### Who a step reaches

Every person slot already carries an **awareness** (Milestone 2's design): `contact` (the attempt reached them,
whether it worked or not), `failure` (they know only if it goes wrong), or `never`. Today it decides only who a later
`silence` can count on knowing about the plot. It now also decides who remembers the step:

| Awareness | Fires on            | Whose                                                        |
| --------- | ------------------- | ------------------------------------------------------------ |
| `contact` | Success and failure | Somebody dealt with face to face                             |
| `failure` | Failure only        | Somebody worked against without being met                    |
| `success` | Success only        | **New.** Whoever a denunciation or exposure accuses          |
| `never`   | Nothing             | Nobody, once those two accused move from `never` to `success` |

By verb: `contact` is buy's seller, deliver's recipient, the mark of bribe, win_over, threaten and blackmail,
silence's witness, and the authority of denounce, expose and petition. `failure` is the mark of watch, ask_around,
steal, plant, spread_rumors, sabotage, dig_up and destroy, and falsify's subject.

A **timeout reaches nobody**, as before: the agent ran out of time before the attempt got that far.

**`success` is new** because a denunciation that works is done in the open. The jarl acts on it, and the accused
learns who accused them and before whom. One that fails -- the authority is not convinced -- never reaches the
person accused. That gives `denounce` and `expose` two people a step can reach -- the accused on success, the
authority either way -- so target templates belong to the **slot**, not the verb: the accused and the authority
remember the same denunciation in different words.

The crafting verbs have no person slot, and reach nobody.

### Three ways a step reaches somebody

- **It went wrong in front of them.** The step failed and their awareness fires on failure: a watched mark spotted
  the watcher, a guard refused the bribe, a jarl was not convinced. They know who the agent was, because they saw
  them. `target_failed`.
- **It was done to them openly.** The step succeeded and their awareness fires on success: they took the coin, were
  handed the parcel, were won over, were denounced before the jarl. They know who the agent was. `target_succeeded`.
- **It was done to them quietly, and left a mark.** The step succeeded, their awareness fires only on failure, and
  the verb leaves something behind they would notice. They know something happened and **not who did it**.
  `target_noticed`. Of the `failure` verbs, these leave a mark:

  | Verb            | What they notice                                   |
  | --------------- | -------------------------------------------------- |
  | `steal`         | The thing is gone                                  |
  | `destroy`       | The thing is ruined                                |
  | `sabotage`      | Their livelihood is suffering                      |
  | `spread_rumors` | People are saying ugly things about them           |

  and these leave nothing to notice: `watch` and `ask_around` learned something without touching them, `dig_up`
  found what was already there, `falsify` made a document nobody has used yet, and `plant` hid a thing that stays
  hidden until a later step -- a denunciation or rumors -- gives somebody a reason to look. That later step is what
  the mark then remembers.

### Templates

Each verb gains one template beside Milestone 2's nine, each person slot whose awareness can fire gains up to three,
and the plot gains three:

| Template                    | Voice                                                               | Written                                         | To                                  |
| --------------------------- | ------------------------------------------------------------------- | ----------------------------------------------- | ----------------------------------- |
| `mastermind_told_succeeded` | The mastermind, on being told the step was done                     | When the step succeeds                          | The mastermind, unless self-agent   |
| `target_failed`             | The person reached, on the attempt going wrong in front of them     | When the step fails, awareness firing           | That person                         |
| `target_succeeded`          | The person reached, on it being done to them openly                 | When the step succeeds, awareness firing        | That person                         |
| `target_noticed`            | The person reached, on noticing what was done, not by whom          | When the step succeeds, on a verb that marks    | That person                         |
| `mastermind_adapted`        | The mastermind, on going on another way after a failure. Per plot   | When a re-plan is taken                         | The mastermind                      |
| `mastermind_succeeded`      | The mastermind, on bringing the scheme off. Per plot                | When the finish succeeds                        | The mastermind                      |
| `mastermind_failed`         | The mastermind, on the scheme failing at its finish. Per plot       | When the finish fails                           | The mastermind                      |

A slot carries only the target templates its awareness can fire: a `contact` slot has `target_failed` and
`target_succeeded`, a `failure` slot has `target_failed` and, if its verb leaves a mark, `target_noticed`, a `success`
slot has `target_succeeded`, and a `never` slot none. The verb suite asserts every slot carries exactly the ones it
can use.

Drafts, for review at the step that writes them:

```text
steal       mastermind_told_succeeded   Aeta reported back that they had stolen a ruby from Hulda.
steal       target_failed               I caught Aeta trying to steal a ruby from me.
steal       target_noticed              Someone stole a ruby from me.
bribe       target_failed               Aeta offered me coin to look the other way, and I refused it.
bribe       target_succeeded            Aeta paid me to look the other way.
win_over    target_succeeded            Aeta won me over to Thonar's side.
denounce    target_succeeded (mark)     Aeta denounced me to Igmund.
denounce    target_succeeded (auth.)    Aeta came to me denouncing Thongvor, and I believed them.
denounce    target_failed (authority)   Aeta came to me denouncing Thongvor, and I was not convinced.
spread_rum. target_noticed              Somebody has been spreading ugly talk about me around Markarth.
plot        mastermind_adapted          When that went wrong, I found another way to go on with my plan to ruin Hulda.
plot        mastermind_succeeded        I brought off my plan to ruin Hulda.
plot        mastermind_failed           My plan to ruin Hulda came to nothing.
```

Rules the suite holds every template to, beside Milestone 2's:

- **`target_noticed` names neither `{agent}` nor `{mastermind}`, and relates nobody.** The memory's related actors
  would otherwise tell SkyrimNet, and through it the NPC, exactly who they do not know did it.
- **The target templates name `{mastermind}` only where the verb's own sentence does.** Being won over to Thonar's
  side, or petitioned on his behalf, says whose side it is; being robbed by Aeta does not say who sent her.
- **The target templates never name `{scheme}`.** The person reached does not know the plan.
- **The plot templates name nothing but `{scheme}`**, as `mastermind_conceded` already does: the step memories kept
  before them say what happened.

### Importance and type

| Template                    | Importance | Type         | Why                                                                     |
| --------------------------- | ---------- | ------------ | ----------------------------------------------------------------------- |
| `mastermind_told_succeeded` | 0.5        | `KNOWLEDGE`  | Level with being told of a timeout; hearing about a step, not doing it  |
| `target_failed`             | 0.75       | `EXPERIENCE` | Level with the agent's failure: catching somebody is what gets talked of |
| `target_succeeded`          | 0.55       | `EXPERIENCE` | Level with the agent's success                                          |
| `target_noticed`            | 0.6        | `EXPERIENCE` | Being wronged by nobody one can name lingers                            |
| `mastermind_adapted`        | 0.5        | `EXPERIENCE` | Below the failure memory it follows                                     |
| `mastermind_succeeded`      | 0.7        | `EXPERIENCE` | The plan done is the most a mastermind has to show                      |
| `mastermind_failed`         | 0.65       | `EXPERIENCE` | Above giving up: it was lost, not let go                                |

Every new memory carries the plot tag, and a step memory takes the step's place as its location, as Milestone 2's do.
A target memory relates the agent, and the mastermind where it names them; `target_noticed` relates nobody. The plot
memories relate nobody.

**Only the living remember.** A keeper who has died by the time the memory is written keeps nothing, as a mastermind
who dies already does.

---

## Implementation plan

Every step is owned by `[CLAUDE]` or `[USER]` and verified on its own. The co-save is not touched: nothing here is
state, only memories written as things happen.

### Step 1 — The mastermind hears of a success

- [ ] Complete

**[CLAUDE]**

`mastermind_told_succeeded` on every verb, written to a mastermind who sent somebody else when that somebody succeeds.
A mastermind who did the step themselves keeps `agent_succeeded` alone, as now. Settles Milestone 2's open question 5.

**Verification:** the memory suite asserts, per verb, that a success writes `agent_succeeded` to the agent and
`mastermind_told_succeeded` to a mastermind who sent them, and only the first to a mastermind who acted themselves.

### Step 2 — The mastermind remembers how the scheme went

- [ ] Complete

**[CLAUDE]**

The three plot templates, written by the plot worker where it already decides each outcome:

- `mastermind_adapted` when a re-plan is taken after a failure, after the failure's own memory;
- `mastermind_succeeded` when the finish succeeds and the plot ends achieved;
- `mastermind_failed` when the finish fails and the plot ends, after the step's own failure memory.

`mastermind_conceded` stays as it is for a plot given up, by decision, by the cap or by a hopeless re-plan. A plot
whose mastermind is gone writes none of them.

**Verification:** the memory suite asserts each plot template goes to the mastermind alone, names only the scheme
and relates nobody; a plot-worker suite, or the memory suite over the outcomes that decide them, asserts a taken
re-plan writes `mastermind_adapted` and a conceded one does not, an achieved plot writes `mastermind_succeeded`, a
plot that fails at its finish writes `mastermind_failed` and not `mastermind_conceded`, and a plot ended by its
mastermind's death writes none.

### Step 3 — Awareness that fires on success

- [ ] Complete

**[CLAUDE]**

`Awareness::Success`, fired only when the step succeeds, and `denounce`'s and `expose`'s marks moved to it from `never`.
Target templates are carried per slot from here on, ready for Steps 4 and 5. Who a later `silence` may count as knowing
of the plot follows it, as it follows the others: somebody denounced before the jarl knows of the plot against them.

**Verification:** the verb suite asserts `denounce`'s and `expose`'s marks are `success`, and the candidates suite
asserts a successful denunciation makes its mark aware and a failed one does not.

### Step 4 — The person a step reaches remembers it

- [ ] Complete

**[CLAUDE]**

`target_failed` and `target_succeeded` on every person slot whose awareness can fire for them, written at resolution
to the person in that slot when it fires: on failure for `contact` and `failure`, on success for `contact` and `success`.
Nothing on a timeout. A keeper who is not alive keeps nothing, for these and every other plot memory.

**Verification:** the verb suite asserts each slot carries exactly the target templates its awareness can use, and
that they name `{mastermind}` only where the verb's sentence does and never `{scheme}`. The memory suite asserts, per
verb and per outcome, exactly who receives which template: a caught `steal` writes `target_failed` to its mark, a
successful `bribe` writes `target_succeeded` to its mark, a successful `denounce` writes it to the mark and the
authority, a failed one to the authority alone, a timeout to no target, and a dead target keeps nothing.

### Step 5 — A quiet success that leaves a mark is noticed

- [ ] Complete

**[CLAUDE]**

A per-verb flag for leaving a mark, set on `steal`, `destroy`, `sabotage` and `spread_rumors`, and `target_noticed`
on those four, written to the mark when the step succeeds. It names and relates neither the agent nor the
mastermind.

**Verification:** the verb suite asserts exactly the four verbs carry the flag and the template, and that the template
names neither `{agent}` nor `{mastermind}`. The memory suite asserts a successful `steal` writes `target_noticed` to
its mark with no related actors, and a successful `watch`, `dig_up` or `plant` writes nothing to theirs.

### Step 6 — In-game validation

- [ ] Complete

**[USER]**

With plots running over a session long enough for several to end:

1. Ask a mastermind whose agent succeeded what came of it. Do they know?
2. Ask a mastermind whose plot succeeded, one whose plot failed at its finish, and one who went on after a failure.
   Does each know how it stands?
3. Find a mark who caught an agent. Do they know who it was?
4. Find somebody bribed, won over or denounced. Do they know who came to them?
5. Find somebody robbed, or talked about. Do they know it happened, and not who did it?
6. Find somebody watched without noticing. Do they know nothing?

Record findings, and any wording or importance changes, under this step.

---

## Open questions

1. **Does a target learn who sent the agent?** The drafts give a target the agent's name and the mastermind's only
   where the verb's sentence says it. A caught agent might give up who sent them; whether that should be a chance, a
   rule by verb, or nothing is left for after Step 6 shows how the plain version plays.
2. **Is a planted item ever found without a later step?** As drafted, `plant` reaches its mark only through the
   denunciation or rumors that follow it. A plot that plants and then fails before that leaves the item unnoticed for
   good.
