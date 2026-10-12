# Phase 17 — Milestone 2: Rigid Steps

Milestone 1 (the original [`PHASE_17_FACTION_PLOTS.md`](PHASE_17_FACTION_PLOTS.md), Steps 1–17) built a plot
simulation whose masterminds, ambitions and schemes are good and whose steps are not. This milestone replaces how
a step is chosen, described and remembered. The mastermind draw, the ambition and scheme prose, the casting
ladder, the progress race and the co-save all stay.

> **Doc status: Steps 1–40 complete; Steps 41–43 follow up Step 9's ninth attempt.** Steps 10–12 plan
> backwards from the goal, Steps 13–16 tie the plan to the vocabulary and bound the preparing, Steps 17–19 keep the
> player out, route denunciations by authority and give the scheme back its purpose, Step 20 makes adapting find
> another way, Steps 21–25 mend what the fifth attempt found, Steps 26–28 add five verbs and the records they act on,
> Steps 29–31 add five crafting verbs, Steps 32–34 make a scheme finish what it starts on the person it names, and
> Steps 35–37 probe and then replace how a finish's people are read, Steps 38–40 make a refused role say why and
> what would do, and Steps 41–43 rank authority and make preparing worth it -- each added after an attempt at Step 9.
> Steps 18 and 19 of the Phase 17 doc are superseded by this milestone; its Steps 20–22 still apply and follow it.

---

## Why this milestone exists

An audit of the six recorded twenty-tick runs (2026-09-05: 67 plots, 138 dispatches, 88 resolutions) found that
the schemes fit their masterminds and the steps under them did not hold together. Every defect traces to one
cause: **a step is free prose, written before anyone is cast to carry it out, and resolved afterwards by
searching for whoever the prose seems to describe.**

| Observed                                                                                | Why                                                         |
| --------------------------------------------------------------------------------------- | ----------------------------------------------------------- |
| "Nirya searches the Arcanaeum…" carried out by Brelyna; "Idolaf watches…" by Lars       | The description names a doer; casting picks another         |
| Korir "convinces his steward, Harrald" — no such person; the actor was Malur Seloth     | Free text can name anybody                                  |
| "Has *the hunter* confirm…" — step 1's hunter was Aeta, step 2 was cast to Oslaf        | Each step is cast alone, and the actor cooldown benches the last agent |
| Children cast as agents and targets (Lars Battle-Born, Aventus Aretino)                 | Nothing filters `NordRaceChild`; search answers with anyone |
| 4 of 6 concessions read "they ran out of ideas"                                         | Adaptation has no retry, so any rejected answer concedes    |
| Births rejected for `obtain`, `persuade`, `investigate`, over-long descriptions         | The model writes the step; the parser has to police it      |

None of these is fixable by tightening a prompt, because each one is the model being asked to write something
the code then has to interpret. This milestone stops asking.

---

## Design

### A step is a verb with filled slots

Every step is one **verb** from a closed manifest. A verb is a fixed record:

- **A sentence** with typed slots, for example `{agent} asks around {place} about {mark}.`
  It is the only description a step ever has. The dashboard shows it, the memories are built from it, and nothing
  about a step is written by a model.
- **Slots**, each a **person**, a **place** or a **thing**, each with a **candidate rule** that decides in code
  who or what may fill it.
- **Awareness**, per person slot: whether that person knows the step happened — `contact` (whenever the attempt
  reached them, on success or failure: a bribe's mark knows they were offered coin whether they took it or not),
  `failure` (only when the step fails: a watch's mark knows only if they spotted the watcher), or `never`. A
  timeout reaches nobody. Awareness is what would decide whether the mark remembers the step, which needs
  templates this draft does not carry yet; see open question 4.
- **A resolution profile** — one of the eight existing `StepType` rows — so `PlotResolution` keeps sizing, rolling
  and gating exactly as it does now. Several verbs may share a profile.
- **Nine sentence templates** — the dashboard line and eight first-person memories; see below.

`{agent}` and `{mastermind}` are slots in every verb. `{mastermind}` is always the plot's mastermind. `{agent}` is
filled at dispatch from the casting ladder, which is the only point at which availability is known.

### Slots are filled by choice, never by text

Every slot that is not bound to an earlier step is filled by the **SkyrimNet decision API** (public API v12,
`PublicSendCustomDecisionToLLM`), which answers a `choice` question with one of the keys it was offered and
nothing else. The model sees a short list of candidates the rule has already admitted, each as one line —
name, what they do, how the mastermind knows them — and picks one. There is no free text to parse, no menu
membership test, and no way to name somebody who does not exist.

A slot can instead be **bound** to an earlier step of the same plot, in which case no question is asked:
`deliver`'s `{item}` is whatever an earlier `steal` or `buy` obtained, and its `{agent}` is whoever obtained it.
Binding is how a plan stays one story.

**A verb has a `{place}` only when the place is what the step acts on** — a community whose knowledge or opinion
is the point, as with `ask_around` and `spread_rumors`. It is never where some person in the step happens to be.
NPCs move on schedules and with quests, so a step pinned to where its mark lives would describe them somewhere they
often are not, and every memory written from it would say so.

### The starting verb manifest

Wording is a first draft for review; the shape is the proposal. Profiles are the existing `StepType` ids.

| Verb            | Sentence                                                               | Slots and rules                                                                                    | Awareness          | Profile   |
| --------------- | ---------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------- | ------------------ | --------- |
| `watch`         | `{agent}` watches `{mark}`, learning their habits                      | mark: person, known to mastermind                                                                  | mark: failure      | surveil   |
| `ask_around`    | `{agent}` asks around `{place}` about `{mark}`                         | mark: person, known to mastermind; place: settlement where the mark or someone close to them lives | mark: failure      | locate    |
| `steal`         | `{agent}` steals `{item}` from `{mark}`                                | item: thing, item pool; mark: person, known to mastermind                                          | mark: failure      | acquire   |
| `buy`           | `{agent}` buys `{item}` from `{seller}`                                | item: thing, item pool; seller: person, trades for a living                                        | seller: contact    | acquire   |
| `deliver`       | `{agent}` brings `{item}` to `{recipient}`                             | item: bound to an earlier steal/buy; agent: bound to whoever got it; recipient: person             | recipient: contact | deliver   |
| `plant`         | `{agent}` hides `{item}` among `{mark}`'s belongings                   | item and agent: bound as for deliver; mark: person                                                 | mark: failure      | conceal   |
| `bribe`         | `{agent}` pays `{mark}` to look the other way                          | mark: person with standing in a rostered faction                                                   | mark: contact      | recruit   |
| `win_over`      | `{agent}` wins `{mark}` over to `{mastermind}`'s side                  | mark: person, known to mastermind, not hostile to them                                             | mark: contact      | recruit   |
| `threaten`      | `{agent}` warns `{mark}` to stay out of `{mastermind}`'s affairs       | mark: person, known to mastermind                                                                  | mark: contact      | recruit   |
| `spread_rumors` | `{agent}` spreads ugly talk about `{mark}` around `{place}`            | mark: person; place: settlement, mark's or mastermind's                                            | mark: failure      | discredit |
| `denounce`      | `{agent}` denounces `{mark}` to `{authority}`                          | mark: person; authority: whoever the mark answers to (Step 18)                                     | authority: contact | discredit |
| `sabotage`      | `{agent}` sabotages `{mark}`'s livelihood                              | mark: person who trades or works for a living                                                      | mark: failure      | sabotage  |
| `silence`       | `{agent}` pays `{witness}` to keep quiet                               | witness: person aware of an earlier step of this plot, else known to mastermind                    | witness: contact   | conceal   |
| `falsify`       | `{agent}` falsifies `{item}` in `{subject}`'s name (Step 27)           | item: thing, a document from the pool; subject: person                                             | subject: failure   | acquire   |
| `dig_up`        | `{agent}` digs up `{item}` proving `{mark}`'s wrongdoing (Step 27)     | item: thing, a document from the pool; mark: person, known to mastermind                           | mark: failure      | locate    |
| `blackmail`     | `{agent}` blackmails `{mark}` into serving `{mastermind}` (Step 28)    | mark: person, known to mastermind                                                                  | mark: contact      | recruit   |
| `destroy`       | `{agent}` destroys `{item}` belonging to `{mark}` (Step 28)            | item: thing, item pool; mark: person, known to mastermind                                          | mark: failure      | sabotage  |
| `petition`      | `{agent}` petitions `{authority}` on `{mastermind}`'s behalf (Step 28) | authority: whoever the mastermind answers to (Step 28)                                             | authority: contact | recruit   |
| `smith`         | `{agent}` smiths `{item}` (Step 30)                                    | item: thing, a smithing recipe somebody the mastermind can send is able to make (Step 30)          | none               | craft     |
| `concoct`       | `{agent}` concocts `{item}` (Step 30)                                  | item: thing, a potion or poison as for `smith`                                                     | none               | craft     |
| `enchant`       | `{agent}` makes `{item}` at an arcane enchanter (Step 30)              | item: thing, an enchanted variant of an item, as for `smith`                                       | none               | craft     |
| `set`           | `{agent}` makes `{item}` at a jeweler's bench (Step 30)                | item: thing, a ring, necklace or circlet recipe, as for `smith`                                    | none               | craft     |
| `fletch`        | `{agent}` fletches `{item}` (Step 30)                                  | item: thing, a bow, crossbow, arrow or bolt recipe, as for `smith`                                 | none               | craft     |
| `expose`        | `{agent}` shows `{item}` to `{authority}` as proof against `{mark}`    | item and agent: bound as for deliver; mark: person; authority: as for `denounce` (Step 33)         | authority: contact | discredit |

**Rules every person slot carries without saying so:** alive, adult, not the player, not a ghost, in the plot
population, not the mastermind (except `{mastermind}` itself), and not the same person as any other slot of the
same step.

**Verb preconditions** gate what is offered at each position: `deliver` and `plant` need an earlier step that
obtained an item; any verb with a slot whose rule admits nobody is not offered at all.

### Planning

The mastermind draw and the ambition-and-scheme call are unchanged. The current third call, the step prompt that
writes descriptions and agent queries, is replaced by a loop of decision requests:

```text
for position 1..cap:
    which verb comes next?        choice among the verbs whose preconditions hold, plus `done` from position 2
    done?                         stop
    fill that verb's slots        one request, one question per independent slot; a second for any slot
                                  whose candidates depend on the first answer
```

Each request carries the mastermind's biography, the ambition, the scheme, and the steps chosen so far as
rendered sentences, in `[ state ]` blocks. `{agent}` is left open in the rendered sentences until dispatch.

**Revised by Step 12: planning runs backwards from the goal.** The forward loop above was built and run, and the
decision model answered "what next?" with the same step until the cap; see Step 9's first attempt. Planning now asks
for the step that accomplishes the scheme first, puts a theft or purchase in front of any step that needs an item,
and then offers concrete preparations of the planned steps, from a table in the verb manifest (Step 11), until the
model answers `ready`.

### Dispatch

`{agent}` is filled from the casting ladder's eligible candidates. With one eligible candidate (often the
mastermind alone) no question is asked. With several, one decision request picks among them, given the rendered
step. A bound agent (the holder of an item) is taken without asking, and **a plot's own previous agent is exempt
from the actor cooldown** for that plot, so a hunter sent on step 1 can be sent again on step 2.

The resolution profile then sizes the step exactly as today.

### Adaptation

A failed step asks one decision question: `retry` (the same step, a different agent, the failed one excluded),
`substitute` (a different verb at this position, then the normal slot fill), or `concede`. The cap of
`iPlotMaxAdaptations` stays. There is no concession sentence: conceding is a choice, and the mastermind's
memory of it is the shared `mastermind_conceded` template.

**Revised by Step 20: adapting means another way.** A failure says the route it was on no longer works, so `retry`
is gone. The failed step, and the planned steps it was there to serve, are abandoned for the rest of the plot, and
the re-plan has to reach the same finish without them.

### Memories: templates, written at dispatch and resolution

Every verb carries nine sentence templates, filled from its slots, and every plot shares one more for giving up.
The verb's first template is the present-tense, third-person line the dashboard shows. All the others are
**first person and past tense**, written as memories in the style gossip already uses
(`GossipContent::ComposeHeard`). No LLM writes memory text.

A step can end three ways, and each has its own templates. It **succeeds**; it **fails**, meaning the step's
mishap roll landed and the attempt went wrong in the act (caught, refused, stopped); or it **times out**,
meaning the budget ran out before the work was done.

| Template                    | Voice                                                               | Written                  | To                                  |
| --------------------------- | ------------------------------------------------------------------- | ------------------------ | ----------------------------------- |
| `dashboard`                 | Third person, present: what the step is                             | Never; display only      | The dashboard                       |
| `agent_dispatched`          | The agent, on being given the task and setting off to do it         | At dispatch              | The agent                           |
| `mastermind_dispatched`     | The mastermind, on giving the agent the task for their scheme       | At dispatch              | The mastermind                      |
| `self_dispatched`           | The mastermind, on deciding to do the step themselves               | At dispatch              | A self-agent, instead of both above |
| `agent_succeeded`           | The agent, on succeeding                                            | When the step succeeds   | The agent, or the self-agent        |
| `agent_failed`              | The agent, on the attempt going wrong                               | When the step fails      | The agent, or the self-agent        |
| `agent_timed_out`           | The agent, on running out of time                                   | When the step times out  | The agent, or the self-agent        |
| `mastermind_told_failed`    | The mastermind, on being told the attempt went wrong                | When the step fails      | The mastermind, unless self-agent   |
| `mastermind_told_timed_out` | The mastermind, on being told the agent ran out of time             | When the step times out  | The mastermind, unless self-agent   |
| `mastermind_conceded`       | The mastermind, on giving up the plot. One template, not per verb   | On giving up the plot    | The mastermind                      |

Rules that fall out of that table, all checked by Step 3's suite:

- **The agent's outcome templates never name `{mastermind}`.** When the mastermind is their own agent these are
  the memories they keep, and "I won Ysolda over to Ysolda's side" is what naming them would produce.
- **Only the dispatch templates and `mastermind_conceded` name `{scheme}`.** Those are where the motive is
  recorded and where it is abandoned; repeating it on every outcome would turn a memory of a step into a
  restatement of the plot.
- **`mastermind_conceded` names nothing but `{scheme}`.** It follows a failure memory the mastermind has already
  kept, which says what went wrong; this one only says what they stopped trying to do.

The plot-level template:

```text
mastermind_conceded         I gave up on my plan to {scheme}.
```

It is written whenever a plot ends without its scheme done and with its mastermind still alive: the decision
chose `concede`, the adaptation cap was hit, or no agent could be found for the next step. A mastermind who
dies keeps nothing, for obvious reasons.

**`{scheme}` is rendered as a verb phrase**, so that "as part of my plan to `{scheme}`" reads as a sentence: first
letter lowercased, final punctuation dropped. That only works if the scheme is written as an imperative clause —
"spread a rumor that Hulda is a secret heavy drinker", not "He is secretly funding an excavation". The ambition
prompt is changed to require that form; see Step 5.

Every memory carries the plot output tag (the `GossipHarvest::kOwnOutputTag` precedent), the other parties of the
step as related actors, the step's `{place}` as its location when the verb has one and no location otherwise, and
an importance and memory type (`EXPERIENCE` or `KNOWLEDGE`, from SkyrimNet's documented set) fixed per template.
A failure is the highest importance a step writes, above a timeout, which is what Phase 17 Step 21 will seed
gossip from.

### The verb templates

Wording is a first draft for review. Slot names are the ones in the manifest above, and `{scheme}` is the plot's
scheme rendered as described.

#### `watch`

```text
dashboard                   {agent} watches {mark}, learning their habits.
agent_dispatched            {mastermind} asked me to keep watch on {mark} and learn their habits, so I set off to do it.
mastermind_dispatched       I sent {agent} to keep watch on {mark} and learn their habits, as part of my plan to {scheme}.
self_dispatched             I decided to keep watch on {mark} myself and learn their habits, as part of my plan to {scheme}.
agent_succeeded             I kept watch on {mark} until I knew their habits.
agent_failed                {mark} spotted me watching them, and I had to break it off.
agent_timed_out             I kept watch on {mark}, but ran out of time before I learned their habits.
mastermind_told_failed      {agent} reported back that {mark} had spotted them watching.
mastermind_told_timed_out   {agent} reported back that they had run out of time before learning {mark}'s habits.
```

#### `ask_around`

```text
dashboard                   {agent} asks around {place} about {mark}.
agent_dispatched            {mastermind} asked me to go around {place} asking questions about {mark}, so I set off to do it.
mastermind_dispatched       I sent {agent} to ask around {place} about {mark}, as part of my plan to {scheme}.
self_dispatched             I decided to ask around {place} about {mark} myself, as part of my plan to {scheme}.
agent_succeeded             I asked around {place} and learned what I needed to know about {mark}.
agent_failed                Word got back to {mark} that I was asking questions about them around {place}, and I had to stop.
agent_timed_out             I asked around {place} about {mark}, but ran out of time before anyone told me anything useful.
mastermind_told_failed      {agent} reported back that {mark} had heard they were asking questions around {place}.
mastermind_told_timed_out   {agent} reported back that they had run out of time before learning anything useful about {mark}.
```

#### `steal`

```text
dashboard                   {agent} steals {item} from {mark}.
agent_dispatched            {mastermind} asked me to steal {item} from {mark}, so I set off to do it.
mastermind_dispatched       I sent {agent} to steal {item} from {mark}, as part of my plan to {scheme}.
self_dispatched             I decided to steal {item} from {mark} myself, as part of my plan to {scheme}.
agent_succeeded             I stole {item} from {mark}.
agent_failed                {mark} caught me trying to steal {item} from them.
agent_timed_out             I tried to steal {item} from {mark}, but ran out of time before I could get my hands on it.
mastermind_told_failed      {agent} reported back that {mark} had caught them trying to steal {item}.
mastermind_told_timed_out   {agent} reported back that they had run out of time before they could steal {item} from {mark}.
```

#### `buy`

```text
dashboard                   {agent} buys {item} from {seller}.
agent_dispatched            {mastermind} asked me to buy {item} from {seller}, so I set off to do it.
mastermind_dispatched       I sent {agent} to buy {item} from {seller}, as part of my plan to {scheme}.
self_dispatched             I decided to buy {item} from {seller} myself, as part of my plan to {scheme}.
agent_succeeded             I bought {item} from {seller}.
agent_failed                I tried to buy {item} from {seller}, but the deal went sour.
agent_timed_out             I tried to buy {item} from {seller}, but ran out of time before we could strike a deal.
mastermind_told_failed      {agent} reported back that the deal with {seller} for {item} had gone sour.
mastermind_told_timed_out   {agent} reported back that they had run out of time before striking a deal with {seller} for {item}.
```

#### `deliver`

```text
dashboard                   {agent} brings {item} to {recipient}.
agent_dispatched            {mastermind} asked me to bring {item} to {recipient}, so I set off to do it.
mastermind_dispatched       I sent {agent} to bring {item} to {recipient}, as part of my plan to {scheme}.
self_dispatched             I decided to bring {item} to {recipient} myself, as part of my plan to {scheme}.
agent_succeeded             I brought {item} to {recipient}.
agent_failed                I was stopped on my way to bring {item} to {recipient}.
agent_timed_out             I ran out of time before I could bring {item} to {recipient}.
mastermind_told_failed      {agent} reported back that they had been stopped on the way to bring {item} to {recipient}.
mastermind_told_timed_out   {agent} reported back that they had run out of time before bringing {item} to {recipient}.
```

#### `plant`

```text
dashboard                   {agent} hides {item} among {mark}'s belongings.
agent_dispatched            {mastermind} asked me to hide {item} among {mark}'s belongings, so I set off to do it.
mastermind_dispatched       I sent {agent} to hide {item} among {mark}'s belongings, as part of my plan to {scheme}.
self_dispatched             I decided to hide {item} among {mark}'s belongings myself, as part of my plan to {scheme}.
agent_succeeded             I hid {item} among {mark}'s belongings.
agent_failed                {mark} caught me trying to hide {item} among their belongings.
agent_timed_out             I ran out of time before I could hide {item} among {mark}'s belongings.
mastermind_told_failed      {agent} reported back that {mark} had caught them trying to hide {item} among their belongings.
mastermind_told_timed_out   {agent} reported back that they had run out of time before hiding {item} among {mark}'s belongings.
```

#### `bribe`

```text
dashboard                   {agent} pays {mark} to look the other way.
agent_dispatched            {mastermind} asked me to pay {mark} to look the other way, so I set off to do it.
mastermind_dispatched       I sent {agent} to pay {mark} to look the other way, as part of my plan to {scheme}.
self_dispatched             I decided to pay {mark} to look the other way myself, as part of my plan to {scheme}.
agent_succeeded             I paid {mark}, and they agreed to look the other way.
agent_failed                I offered {mark} coin to look the other way, but they would not take it.
agent_timed_out             I ran out of time before I could get {mark} alone to offer them coin.
mastermind_told_failed      {agent} reported back that {mark} had refused the bribe.
mastermind_told_timed_out   {agent} reported back that they had run out of time before they could offer {mark} the bribe.
```

#### `win_over`

```text
dashboard                   {agent} wins {mark} over to {mastermind}'s side.
agent_dispatched            {mastermind} asked me to win {mark} over to their side, so I set off to do it.
mastermind_dispatched       I sent {agent} to win {mark} over to my side, as part of my plan to {scheme}.
self_dispatched             I decided to win {mark} over to my side myself, as part of my plan to {scheme}.
agent_succeeded             I won {mark} over.
agent_failed                I tried to win {mark} over, but they would not be swayed.
agent_timed_out             I ran out of time before I could win {mark} over.
mastermind_told_failed      {agent} reported back that {mark} would not be won over.
mastermind_told_timed_out   {agent} reported back that they had run out of time before winning {mark} over.
```

#### `threaten`

```text
dashboard                   {agent} warns {mark} to stay out of {mastermind}'s affairs.
agent_dispatched            {mastermind} asked me to warn {mark} to stay out of their affairs, so I set off to do it.
mastermind_dispatched       I sent {agent} to warn {mark} to stay out of my affairs, as part of my plan to {scheme}.
self_dispatched             I decided to warn {mark} to stay out of my affairs myself, as part of my plan to {scheme}.
agent_succeeded             I warned {mark} off, and they took the warning to heart.
agent_failed                I tried to warn {mark} off, but they would not be frightened.
agent_timed_out             I ran out of time before I could warn {mark} off.
mastermind_told_failed      {agent} reported back that {mark} would not be warned off.
mastermind_told_timed_out   {agent} reported back that they had run out of time before warning {mark} off.
```

#### `spread_rumors`

```text
dashboard                   {agent} spreads ugly talk about {mark} around {place}.
agent_dispatched            {mastermind} asked me to spread ugly talk about {mark} around {place}, so I set off to do it.
mastermind_dispatched       I sent {agent} to spread ugly talk about {mark} around {place}, as part of my plan to {scheme}.
self_dispatched             I decided to spread ugly talk about {mark} around {place} myself, as part of my plan to {scheme}.
agent_succeeded             I spread ugly talk about {mark} around {place}, and people started repeating it.
agent_failed                I spread ugly talk about {mark} around {place}, but it was traced back to me.
agent_timed_out             I ran out of time before the talk about {mark} took hold around {place}.
mastermind_told_failed      {agent} reported back that the talk about {mark} around {place} had been traced back to them.
mastermind_told_timed_out   {agent} reported back that they had run out of time before the talk about {mark} took hold around {place}.
```

#### `denounce`

```text
dashboard                   {agent} denounces {mark} to {authority}.
agent_dispatched            {mastermind} asked me to denounce {mark} to {authority}, so I set off to do it.
mastermind_dispatched       I sent {agent} to denounce {mark} to {authority}, as part of my plan to {scheme}.
self_dispatched             I decided to denounce {mark} to {authority} myself, as part of my plan to {scheme}.
agent_succeeded             I denounced {mark} to {authority}, and {authority} listened.
agent_failed                I tried to denounce {mark} to {authority}, but {authority} would not hear it.
agent_timed_out             I ran out of time before I could get an audience with {authority} to denounce {mark}.
mastermind_told_failed      {agent} reported back that {authority} would not hear a word against {mark}.
mastermind_told_timed_out   {agent} reported back that they had run out of time before getting an audience with {authority}.
```

#### `sabotage`

```text
dashboard                   {agent} sabotages {mark}'s livelihood.
agent_dispatched            {mastermind} asked me to sabotage {mark}'s livelihood, so I set off to do it.
mastermind_dispatched       I sent {agent} to sabotage {mark}'s livelihood, as part of my plan to {scheme}.
self_dispatched             I decided to sabotage {mark}'s livelihood myself, as part of my plan to {scheme}.
agent_succeeded             I sabotaged {mark}'s livelihood.
agent_failed                {mark} caught me trying to sabotage their livelihood.
agent_timed_out             I ran out of time before I could do {mark}'s livelihood any real harm.
mastermind_told_failed      {agent} reported back that {mark} had caught them trying to sabotage their livelihood.
mastermind_told_timed_out   {agent} reported back that they had run out of time before doing {mark}'s livelihood any real harm.
```

#### `silence`

```text
dashboard                   {agent} pays {witness} to keep quiet.
agent_dispatched            {mastermind} asked me to pay {witness} to keep quiet, so I set off to do it.
mastermind_dispatched       I sent {agent} to pay {witness} to keep quiet, as part of my plan to {scheme}.
self_dispatched             I decided to pay {witness} to keep quiet myself, as part of my plan to {scheme}.
agent_succeeded             I paid {witness}, and they agreed to keep quiet.
agent_failed                I offered {witness} coin to keep quiet, but they would not take it.
agent_timed_out             I ran out of time before I could pay {witness} to keep quiet.
mastermind_told_failed      {agent} reported back that {witness} would not be paid to keep quiet.
mastermind_told_timed_out   {agent} reported back that they had run out of time before paying {witness} to keep quiet.
```

#### `falsify` (Step 27)

```text
dashboard                   {agent} falsifies {item} in {subject}'s name.
agent_dispatched            {mastermind} asked me to falsify {item} in {subject}'s name, so I set off to do it.
mastermind_dispatched       I sent {agent} to falsify {item} in {subject}'s name, as part of my plan to {scheme}.
self_dispatched             I decided to falsify {item} in {subject}'s name myself, as part of my plan to {scheme}.
agent_succeeded             I falsified {item} in {subject}'s name, good enough to pass.
agent_failed                My attempt to falsify {item} in {subject}'s name was found out.
agent_timed_out             I ran out of time before I could falsify {item} in {subject}'s name.
mastermind_told_failed      {agent} reported back that their attempt to falsify {item} in {subject}'s name had been found out.
mastermind_told_timed_out   {agent} reported back that they had run out of time before falsifying {item} in {subject}'s name.
```

#### `dig_up` (Step 27)

```text
dashboard                   {agent} digs up {item} proving {mark}'s wrongdoing.
agent_dispatched            {mastermind} asked me to dig up {item} proving {mark}'s wrongdoing, so I set off to do it.
mastermind_dispatched       I sent {agent} to dig up {item} proving {mark}'s wrongdoing, as part of my plan to {scheme}.
self_dispatched             I decided to dig up {item} proving {mark}'s wrongdoing myself, as part of my plan to {scheme}.
agent_succeeded             I dug up {item} proving {mark}'s wrongdoing.
agent_failed                {mark} found out I was digging into their affairs, and I had to stop.
agent_timed_out             I dug into {mark}'s affairs, but ran out of time before I found anything I could prove.
mastermind_told_failed      {agent} reported back that {mark} had found out they were digging into their affairs.
mastermind_told_timed_out   {agent} reported back that they had run out of time before finding proof against {mark}.
```

#### `blackmail` (Step 28)

```text
dashboard                   {agent} blackmails {mark} into serving {mastermind}.
agent_dispatched            {mastermind} asked me to blackmail {mark} into serving them, so I set off to do it.
mastermind_dispatched       I sent {agent} to blackmail {mark} into serving me, as part of my plan to {scheme}.
self_dispatched             I decided to blackmail {mark} into serving me myself, as part of my plan to {scheme}.
agent_succeeded             I blackmailed {mark}, and they gave in.
agent_failed                I tried to blackmail {mark}, but they called my bluff.
agent_timed_out             I ran out of time before I could put the squeeze on {mark}.
mastermind_told_failed      {agent} reported back that {mark} had called their bluff.
mastermind_told_timed_out   {agent} reported back that they had run out of time before blackmailing {mark}.
```

#### `destroy` (Step 28)

```text
dashboard                   {agent} destroys {item} belonging to {mark}.
agent_dispatched            {mastermind} asked me to destroy {item} belonging to {mark}, so I set off to do it.
mastermind_dispatched       I sent {agent} to destroy {item} belonging to {mark}, as part of my plan to {scheme}.
self_dispatched             I decided to destroy {item} belonging to {mark} myself, as part of my plan to {scheme}.
agent_succeeded             I destroyed {item} belonging to {mark}.
agent_failed                {mark} caught me trying to destroy {item} of theirs.
agent_timed_out             I ran out of time before I could get at {item} belonging to {mark}.
mastermind_told_failed      {agent} reported back that {mark} had caught them trying to destroy {item}.
mastermind_told_timed_out   {agent} reported back that they had run out of time before destroying {item} belonging to {mark}.
```

#### `petition` (Step 28)

```text
dashboard                   {agent} petitions {authority} on {mastermind}'s behalf.
agent_dispatched            {mastermind} asked me to petition {authority} on their behalf, so I set off to do it.
mastermind_dispatched       I sent {agent} to petition {authority} on my behalf, as part of my plan to {scheme}.
self_dispatched             I decided to petition {authority} myself, as part of my plan to {scheme}.
agent_succeeded             I petitioned {authority}, and they granted what was asked.
agent_failed                I petitioned {authority}, but they turned me away.
agent_timed_out             I ran out of time before I could get an audience with {authority}.
mastermind_told_failed      {agent} reported back that {authority} had turned them away.
mastermind_told_timed_out   {agent} reported back that they had run out of time before getting an audience with {authority}.
```

#### `smith` (Step 30)

```text
dashboard                   {agent} smiths {item}.
agent_dispatched            {mastermind} asked me to smith {item} for them, so I set to work at the forge.
mastermind_dispatched       I asked {agent} to smith {item} for me, as part of my plan to {scheme}.
self_dispatched             I decided to smith {item} myself, as part of my plan to {scheme}.
agent_succeeded             I smithed {item}, and it came out well.
agent_failed                I ruined the work on {item} at the forge.
agent_timed_out             I ran out of time before I could finish smithing {item}.
mastermind_told_failed      {agent} reported back that they had ruined the work on {item}.
mastermind_told_timed_out   {agent} reported back that they had run out of time before finishing {item}.
```

#### `concoct` (Step 30)

```text
dashboard                   {agent} concocts {item}.
agent_dispatched            {mastermind} asked me to concoct {item} for them, so I set to work.
mastermind_dispatched       I asked {agent} to concoct {item} for me, as part of my plan to {scheme}.
self_dispatched             I decided to concoct {item} myself, as part of my plan to {scheme}.
agent_succeeded             I concocted {item}.
agent_failed                I spoiled the mixture while concocting {item}.
agent_timed_out             I ran out of time before I could finish concocting {item}.
mastermind_told_failed      {agent} reported back that they had spoiled the mixture for {item}.
mastermind_told_timed_out   {agent} reported back that they had run out of time before finishing {item}.
```

#### `enchant` (Step 30)

```text
dashboard                   {agent} makes {item} at an arcane enchanter.
agent_dispatched            {mastermind} asked me to make {item} for them at an arcane enchanter, so I set to work.
mastermind_dispatched       I asked {agent} to make {item} for me at an arcane enchanter, as part of my plan to {scheme}.
self_dispatched             I decided to make {item} at an arcane enchanter myself, as part of my plan to {scheme}.
agent_succeeded             I bound the enchantment and made {item}.
agent_failed                The enchantment slipped while I was making {item}, and the work was ruined.
agent_timed_out             I ran out of time before I could finish enchanting {item}.
mastermind_told_failed      {agent} reported back that the enchantment had slipped and ruined {item}.
mastermind_told_timed_out   {agent} reported back that they had run out of time before finishing {item}.
```

#### `set` (Step 30)

```text
dashboard                   {agent} makes {item} at a jeweler's bench.
agent_dispatched            {mastermind} asked me to make {item} for them, so I set to work at my bench.
mastermind_dispatched       I asked {agent} to make {item} for me, as part of my plan to {scheme}.
self_dispatched             I decided to make {item} myself, as part of my plan to {scheme}.
agent_succeeded             I made {item}, and it is fine work.
agent_failed                My hand slipped, and I spoiled the work on {item}.
agent_timed_out             I ran out of time before I could finish {item}.
mastermind_told_failed      {agent} reported back that they had spoiled the work on {item}.
mastermind_told_timed_out   {agent} reported back that they had run out of time before finishing {item}.
```

#### `fletch` (Step 30)

```text
dashboard                   {agent} fletches {item}.
agent_dispatched            {mastermind} asked me to fletch {item} for them, so I set to work.
mastermind_dispatched       I asked {agent} to fletch {item} for me, as part of my plan to {scheme}.
self_dispatched             I decided to fletch {item} myself, as part of my plan to {scheme}.
agent_succeeded             I fletched {item}, true and straight.
agent_failed                I spoiled the work on {item}.
agent_timed_out             I ran out of time before I could finish fletching {item}.
mastermind_told_failed      {agent} reported back that they had spoiled the work on {item}.
mastermind_told_timed_out   {agent} reported back that they had run out of time before finishing {item}.
```

#### `expose` (Step 33)

```text
dashboard                   {agent} shows {item} to {authority} as proof against {mark}.
agent_dispatched            {mastermind} asked me to show {item} to {authority} as proof against {mark}, so I set off to do it.
mastermind_dispatched       I sent {agent} to show {item} to {authority} as proof against {mark}, as part of my plan to {scheme}.
self_dispatched             I decided to show {item} to {authority} myself, as proof against {mark}, as part of my plan to {scheme}.
agent_succeeded             I showed {item} to {authority}, and they took it as proof against {mark}.
agent_failed                I showed {item} to {authority}, but they would not take it as proof against {mark}.
agent_timed_out             I ran out of time before I could show {item} to {authority}.
mastermind_told_failed      {agent} reported back that {authority} would not take {item} as proof against {mark}.
mastermind_told_timed_out   {agent} reported back that they had run out of time before showing {item} to {authority}.
```

### SkyrimNet Beta 26 becomes the floor

The decision API exists from public API v12, which is SkyrimNet Beta 26. This milestone raises the plugin's hard
floor from Beta 25 to Beta 26, and plots additionally require the user to have a decision provider configured:
without one every request fails with `no_decisions_route`, and the plot subsystem logs that once and stays idle.
There is no chat fallback.

### What this removes

- `narrative_engine_plot_steps.prompt`, `narrative_engine_plot_adapt.prompt`, `PlotStepParse`, `PlotAdaptParse`
  and their fixtures.
- The free-text target and agent resolution in `PlotTick` (`ResolveTarget`, the agent query scorer,
  `Referents` and the coverage thresholds measured for them).
- `PlotMenus` as a prompt menu; its relevance ordering survives as the order candidate lists are cut in.

Whether `CharacterIndex` and the ONNX embedding model survive is an open question below.

---

## Implementation plan

Step ownership, verification standards and the per-step close-out (tick the box, write up what the verification
showed, one commit) follow [`PHASE_17_FACTION_PLOTS.md`](PHASE_17_FACTION_PLOTS.md#how-steps-are-verified),
except that verification now includes the co-located Catch2 suites: `pwsh -File build.ps1 test` must pass.

### Step 1 — Beta 26 floor, and the decision API wrapper

- [x] Complete

**[CLAUDE]**

1. Point the build at the Beta 26 devkit headers; raise `min_skyrimnet_version`, README and
   `docs/DEVELOPMENT.md` to Beta 26.
2. `SkyrimNetAPI::SendCustomDecision`: a synchronous overload for worker threads, like `SendCustomPromptToLLM`,
   that blocks on the callback and returns the parsed answers per question id, or the error code.
3. `FakeSkyrimNet` gains the export, answering from a scripted table, so the wrapper is testable without a game.

**Verification:** build and tests pass. The wrapper's suite covers a choice answer, several questions in one
response, each documented error code, and a callback that never fires within the timeout.

Done. `SkyrimNetAPI::SendCustomDecision` with `ParseDecisionResult` and `DecisionResult::Choice`; the fake DLL's
`PublicSendCustomDecisionToLLM`, reporting API v12 by default, with a scripted queue of answers and a never-answers
mode; `min_skyrimnet_version` 0.26.0; the floor raised in the README, `CUSTOM_PROMPTS.md`, `DEVELOPMENT.md` and the
configure error. 438 tests pass, four of them new.

**The build needed no repointing.** The search already resolves `SkyrimNet Devkit/CppAPI`, and the devkit installed
there is Beta 26 rc4, so the v12 export was already declared. What changed is that an older devkit now fails to
compile `SkyrimNetAPI.cpp` instead of compiling fine, and `DEVELOPMENT.md` says so.

**The wrapper's own failures are named, not just SkyrimNet's.** `unavailable` (no SkyrimNet, or one below v12, where
the header leaves the export null), `not_queued`, `timeout` and `malformed` sit beside SkyrimNet's seven codes, so a
caller can tell "this install cannot ask" from "this request was refused" from "the model never answered". The plot
subsystem's one-time idle warning keys on the first of those and on `no_decisions_route`.

**Unlike the LLM call, a decision gives up.** `SendCustomPromptToLLM` waits forever; a decision waits 60 s by
default. A decision is a short classification, so a request still unanswered after a minute is stuck rather than
slow, and the plot worker should not be held by it. The promise is shared, so a callback arriving after the timeout
is harmless.

**A result that is not a choice reads as no choice.** `Choice()` returns empty for a `score` or `noul` answer as well
as for a missing question, so a caller matching keys can never match an empty string by accident.

### Step 2 — What the decision API will actually do

- [x] Complete

**[CLAUDE]** builds a dashboard debug button that sends one fixed probe template and logs the raw result.
**[USER]** presses it with a decision provider configured and records the results here.

The probe settles what the design above assumes and has not verified:

1. Criteria lines generated by an Inja loop over the context render as options.
2. Keys of the form `c1`…`c20` are accepted and returned verbatim.
3. Twenty options in one question behave as well as five.
4. Several questions in one template are all answered.
5. Latency per request, which decides whether planning a five-step plot is affordable.

If any of these fails, the plan is revised here before Step 3.

**Both halves are done; the results are below.** The Plots tab has a **Probe decisions** button.
It sends `narrative_engine/probe` (under `prompts/decisions/` in the plugin folder) on the plot worker and writes
its findings to `NarrativeEngine.log` as `DecisionProbe:` lines, one per question above:

- **Item 1 and item 3** are one question: the capital of the Reach, from twenty places generated by an Inja loop,
  Markarth placed thirteenth so position is no help. A key that was never offered means the loop did not render.
- **Item 2** checks both questions' keys against the ones offered.
- **Item 3** also asks a five-option question, written out by hand, so twenty and five can be compared.
- **Item 4** counts how many of the three questions came back, the third being a yes/no.
- **Item 5** is the line giving the round trip in milliseconds.

Steps 3 to 8 were built without waiting for this run, at the user's direction, and assume all five hold. If one
does not, the decision templates of Steps 5–7 are what change; the verb manifest, candidate rules and memories do
not depend on the API at all.

**First run (2026-10-11), one request: four of five hold.** Options generated by a loop rendered and were chosen
from (1); both keys came back verbatim (2); all three questions in the request were answered (4); the round trip
took 255 ms (5). Item 3 failed: asked for the capital of the Reach out of twenty bare place names, the model chose
Solitude, with Markarth outside its top three, while it answered the five-option common-knowledge question
correctly. Its likeliest three were all near the top of the list (c2, c4, c1), which could be position bias -- but
the two questions differed in length and in what they needed to know at once, so one request could not say which.

**The user's reading, adopted as a rule: the decision model does not know Skyrim, so every decision supplies what it
needs to know about each choice.** A bare name is not a choice the model can make; a name with what it is, why it
is on the list and what it is like is.

**Second probe: rounds of choices that carry their own background.** The button now asks six rounds, each with one
right answer, and asks each twice -- as written and reversed -- so the right answer sits at two positions:

| Round           | Choices                                              | Background                         |
| --------------- | ---------------------------------------------------- | ---------------------------------- |
| `capital`       | 20 places; the Reach's capital is 13th               | on each option's line              |
| `capital_state` | the same 20                                          | in a `[ state choices ]` block     |
| `watcher`       | 12 invented people; one is fit to tail someone unseen | on each option's line              |
| `watcher_bios`  | the same 12, each a few sentences long               | in a `[ state choices ]` block     |
| `next_step`     | the 13 verbs, after a theft that needs delivering    | each verb's own dashboard sentence |
| `proof`         | 8 pool items; one proves who owns land               | on each option's line              |

The report gives each round's answer, where the right answer sat and the probability the model gave it, then the
totals: overall, by where the background went, and by which half of the list held the right answer. Those answer
what planning needs next -- whether background on a line is enough, whether a biography in a state block is read,
and whether the top of a list is favoured. The people are invented so that no background claims anything about a
real record.

**Second run (2026-10-11), twelve requests: twelve right.** Every round was answered correctly in both orders,
with the right answer given 100% probability everywhere except `next_step` (97% as written, 96% reversed).

| Split                                      | Right  |
| ------------------------------------------ | ------ |
| Background on each option's line           | 8 of 8 |
| Background in a `[ state choices ]` block  | 4 of 4 |
| Right answer in the first half of the list | 6 of 6 |
| Right answer in the second half            | 6 of 6 |

The requests averaged 210 ms. So:

- **The first run's failure was missing knowledge, not position.** The same twenty-place capital question, with a
  line of background per place, was answered right with Markarth 13th and 8th. Nothing favours the top of a list.
- **Item 3 holds once choices carry background:** twenty options did as well as eight.
- **Both placements work.** A multi-sentence biography in a state block is read as reliably as a line on the option,
  so a candidate's full biography can go in a state block when one line is not enough.
- **The verbs need nothing more.** Chosen from all thirteen by their dashboard sentences alone, the right one came
  back at 96–97%.
- **Planning a plot by decisions is cheap:** twelve requests took about two and a half seconds in all.

Every round had one clearly right answer, so this shows the model uses the background it is given, not how well it
judges a scheme; Step 9 is where that shows.

**What this says about planning:** a person's line carries their occupation and how the mastermind knows them, but
a place is offered as its name alone and an item as its spoken name alone. Under the rule above both need a line of
background -- what kind of place and whose, what the item is -- which is a follow-up to Step 5. The verbs stay as
they are.

### Step 3 — The verb manifest

- [x] Complete

**[CLAUDE]**

The verb table replacing `kStepTypes` as the step vocabulary: sentences, slots, rules, bindings, awareness,
profiles and memory templates, all `constexpr`, with a renderer that fills a sentence or template from slot
values. `Step` carries a verb id and slot values; the co-save record version is bumped.

**Verification:** a suite asserts that every verb carries all nine templates; every template names only slots
its verb declares; the three agent outcome templates never name `{mastermind}`; only the dispatch templates and
`mastermind_conceded` name `{scheme}`, and `mastermind_conceded` names nothing else; every bound slot points at a
verb that can supply it; every person slot has an awareness; and a rendered sentence round-trips through the
co-save.

Done. `include/PlotVerbs.h` holds the thirteen verbs as a `constexpr` table, with `src/PlotVerbs.cpp` rendering
them; `PlotModel::Step` gains `verb` and `slots`, and `Label(step)` renders a verb step from its dashboard
template; the co-save record goes to version 8.

**The table is generated from this document's template blocks**, so the wording here and in the code cannot have
drifted when this step was committed. From now on the code is the source; a wording change goes into both.

**`agent_dispatched` does not name `{scheme}` either.** The rule above lists the dispatch templates, and the agent's
is one of them, but none of the drafted agent templates names the scheme, and that is right: the agent is told
what to do, not why. The suite pins it as written.

**Two more properties are pinned than the step asked for**, because each is a sentence that would otherwise read
wrong in somebody's memory: each voice names who it should (the agent the mastermind who sent them, the
mastermind the agent they sent, a self-agent neither), and only `ask_around` and `spread_rumors` have a place.

**A sentence opening with an uncast agent reads "Someone …".** Rendering capitalises the first letter, since a
template can open with a slot and an unfilled slot reads as `someone`, `somewhere` or `something`.

**A verb the build no longer knows drops the plot on load**, like a target whose form has gone: the step can be
neither rendered nor resolved. The same goes for a slot whose form no longer resolves.

The verb's dashboard sentence is rendered but not yet shown: nothing creates a verb step until Step 5.

### Step 4 — Candidate rules

- [x] Complete

**[CLAUDE]**

The rule vocabulary (known to mastermind, trades for a living, senior in a court, rostered standing, settlement,
item pool category, bound) and a builder that turns a rule into an ordered, capped candidate list.
Children leave the plot population entirely, which closes the gap for masterminds as well as slots.

**Verification:** an engine-mock suite over a fabricated province asserts each rule's admits and refusals, that a
child is never a candidate for anything, that bound slots ask nothing, and that ordering is stable
run to run.

Done. `include/PlotCandidates.h` with `src/PlotCandidates.cpp` builds the keyed list for any slot; `PlotMenus::Classify`
is public so candidates are ordered by the same ladder the menus used; population members carry `tradesForLiving`
and `authority`; `PlotPopulation::IsChild` keeps children out of the population. 448 tests pass, five of them new.

**"Trades for a living" and "senior in a court" are vanilla's Job factions.** Fifteen trade factions (merchant,
smith, innkeeper, farmer, miner, fence and the rest) and four authorities (jarl, steward, guard captain, orc chief),
every EditorID looked up in the Spriggit export. Mods put their own NPCs in these factions, so a modded shopkeeper
qualifies with no list of ours to maintain. A faction that fails to resolve is warned about by name and skipped.

**Order is the menu ladder, then strangers last.** Tie and colleague, tie, colleague, same hold, then everybody else;
standing and then FormID break ties, so the same request lists the same people in the same order every time. A
stranger's line says where they live, since the ladder has nothing to say about them.

**A place depends on its mark, so it is asked after the mark.** `ask_around`'s place is the mark's settlement and
then those of the people close to them; `spread_rumors`' is the mark's and then the mastermind's. Asked before the
mark is chosen, both lists are empty.

**A witness is somebody an earlier step actually reached**, read from each earlier step's person slots whose
awareness fired for the outcome it had. A plan is laid out before any of it happens, so at birth nobody has been
reached yet and `silence` falls back to the people the mastermind knows; the rule earns its keep when adaptation
re-plans after a failure.

**Children leave the population, not just the lists.** The audit found Lars Battle-Born cast as an agent and Aventus
Aretino as a target, and a child could equally have been drawn as a mastermind. `IsChild` reads the race's child
flag, and the population census line now reports how many were left out. Gossip keeps them.

**Open question 1 is settled by elimination rather than comparison.** The embedding model and `CharacterIndex` exist
to match a free-text description of a person against biographies. Rigid steps have no such description: a slot is
filled by the model choosing from a list built here. Once Step 6 removes the free-text resolution, nothing calls
either, and they go with it. Biographies themselves stay; they give each candidate's line its occupation.

### Step 5 — Planning by decision

- [x] Complete

**[CLAUDE]**

The verb-then-slots loop, its decision templates under `prompts/decisions/narrative_engine/`, and the removal of
the step prompt and its parser. The ambition prompt is changed to return the scheme as an imperative clause, and
its parser rejects a scheme that does not open with a verb, because every dispatch memory reads "as part of my
plan to `{scheme}`".

**Verification:** a suite drives the loop against scripted decision answers and asserts a plan is built only from
offered keys, preconditions gate `deliver` and `plant`, `done` ends the plan, the position cap holds, and a
decision error aborts the birth without spending the slot.

Done. `include/PlotPlanning.h` with `src/PlotPlanning.cpp` lays out a plan; `plot_verb.prompt` and `plot_slot.prompt`
under `prompts/decisions/narrative_engine/` ask the two questions; `PlotBirth::Compose` plans through them after its
two chat calls; `narrative_engine_plot_steps.prompt` and `ParsePlan` are deleted. 451 tests pass, three of them new.

**One question per request, not one per independent slot as the design says.** A template that generates several
question blocks from a loop is not something the Step 2 probe checks; a template with one fixed question and a
loop-generated option list is. So every slot is its own request, which costs a call per slot rather than per step.
A slot with exactly one candidate asks nothing, and a bound one asks nothing, which keeps the count down: a
steal-then-deliver plan asks seven questions in all. If the probe shows loop-generated questions render, folding a
step's independent slots into one request is a change to `FillSlots` and one template.

**The decision call is a parameter.** `Plan` takes the asking function, so the suite scripts the model's answers in
order and asserts on what was asked; the plot worker binds it to `SendCustomDecision`. The loop needed no fake DLL.

**Each option is the verb's own dashboard sentence with nothing filled in** -- "Someone steals something from
someone." -- so the list says what each verb does without a second description to keep in step with the first. The
plan so far is shown numbered, and a step that has already happened says how it went.

**A planned step's target is its first person slot.** The resolution model sizes a step by its target's importance
and distance, and holds a conspicuous one while that target is near the player; both need the target the old
free-text resolution used to supply. Setting it from the slot keeps sizing and the presence gate working unchanged.

**A session that cannot decide stops trying.** The first `unavailable`, `no_decisions_route` or `ai_disabled`
answer logs one warning saying plots need a decision provider, and every birth after that returns before its first
chat call. A new session clears it, so setting a provider up and reloading is enough.

**The scheme is now checked to open with a verb.** `SchemeOpensWithVerb` refuses a scheme opening with a pronoun, an
article or "to", or with a name followed by what its owner means to do ("Korir plans to ..."). The audited run's "He
is secretly funding a small excavation" is in the suite. The ambition prompt says why the form matters -- every
dispatch memory quotes it -- and states it as a hard constraint.

**The dashboard captions a verb step with its verb** and shows its rendered sentence behind the node, through
`PlotVerbs::Caption`. Dispatch is unchanged until Step 6: a verb step is cast by the ladder's weighted draw, since it
carries no agent description for the old scorer to read.

### Step 6 — Dispatch by decision

- [x] Complete

**[CLAUDE]**

Agent selection over the ladder's eligible set, bound agents, the own-plot cooldown exemption, and the removal of
free-text target and agent resolution.

**Verification:** a suite asserts a single eligible candidate asks nothing, a bound agent is taken as bound, a
plot's previous agent is eligible again on its next step, and a decision error falls back to the ladder's own
order rather than stalling the plot.

Done. `PlotCasting::EligibleAgents` replaces `SelectActor`; `PlotPlanning::PickAgent` and `ItemHolder` hold the choice
and the binding; `PlotTick::DispatchStep` casts through them. The free-text resolution is gone, and with it
everything that existed to serve it: `CharacterIndex`, the ONNX embedding model and its vocabulary, the per-save
vector cache, the name and attribution indexes in `CharacterBioLibrary`, and the headers-only ONNX Runtime vcpkg port.
Biographies stay -- the chat prompts render them and each candidate's line takes its occupation from them -- so
`CharacterBios` keeps the catalogue, the layered loading and the parsed sections. 454 tests pass after a clean build,
three of them new.

**The ladder still decides who may be asked; the decision only chooses among them.** `EligibleAgents` offers everybody
available on the best rung that has anybody, and the mastermind alongside a rung-3 list or alone when no rung has
anyone -- the same places the weighted draw used to let the mastermind in. Offering every rung at once would let
acquaintances crowd out the subordinate who is the obvious choice, and it is the rung order that made senior
masterminds delegate 83% of the time in the audited runs. What the decision fixes is which subordinate: the
audit's Jorleif sending the court wizard to canvass the kitchens was a uniform draw within a rung.

**A bound agent is whoever holds the item.** `deliver` and `plant` go to the agent of the latest `steal` or `buy`
in the plot's history that succeeded, without asking. If that person is busy in another plot the step waits a tick
rather than ending the plot; if they are dead, or nothing was ever obtained, the step cannot run and the plot ends.
Dispatch therefore has three results now -- dispatched, wait, impossible -- where it had two.

**The cooldown exemption is from rest, not from work.** A plot's earlier agents may be offered again despite their
actor cooldown, which is what lets the hunter sent on step one be sent on step two. Somebody engaged in another plot
is still unavailable.

**Anyone the step's sentence names is excluded as its agent**, by passing the rendered sentence to the ladder's
existing name test. That covers every person slot -- a denunciation's authority, a delivery's recipient -- and not
only the mark the target field carries.

**A step written as prose still dispatches**, cast by the same ladder and decision, but with no target unless one
was set. Only adaptation still writes such steps, until Step 7.

### Step 7 — Adaptation by decision

- [x] Complete

**[CLAUDE]**

`retry` / `substitute` / `concede`, the cap, and the removal of the adapt prompt and its parser.

**Verification:** a suite asserts `retry` excludes the failed agent, `substitute` replaces only the failed
position onward, `concede` ends the plot, a decision error concedes, and the cap ends a plot that never concedes.

Done. `PlotPlanning::Adapt` asks `plot_adapt.prompt`'s one question and applies the answer; `PlotBirth::Replan` plans a
substitute from the plot's history with the same decisions birth uses; `PlotTick::AdaptPlot` only ends, logs and
counts. `PlotAdapt`, its parser and `narrative_engine_plot_adapt.prompt` are deleted, `PlotStepParse` is down to the
player check the ambition parser still needs, and a step's agent query and free-text target go with them -- nothing
writes either once every step is a verb -- taking the co-save record to version 9. 455 tests pass after a clean
build, one of them new with eight sections.

**Adaptation is pure, as planning is.** `Adapt` takes the plot, the cap, the decision call and a re-planner, so the
suite drives every path without the plot worker: a retry resets the step at the cursor and touches nothing else, a
substitute replaces the plan from the cursor on, a failed substitute concedes rather than stalling, and a plot fed
nothing but retries stops at exactly the cap.

**Retry is offered only when somebody else could take the step.** A `deliver` or `plant` belongs to whoever holds the
item, so retrying it with "somebody else" is not a thing the world allows; for those the choice is a different way or
giving up. And a retry is never handed back to anybody who already failed that very step in this plot -- if they were
the only one eligible, the mastermind does it, and if the mastermind was the one who failed, the plot ends.

**Substitute re-plans the rest of the plot, not one position.** The design said "a different verb at this position,
then the normal slot fill"; a different first step usually needs different steps after it, and the planner already
plans from history, showing the model how each earlier step went. The new tail is capped at what is left of six.

**A conceded plot's concession is the mastermind's memory of it** -- "I gave up on my plan to ..." -- rather than the
model's sentence that used to be asked for. The dashboard shows it beside the plot; the trace still records why, as a
reason of its own.

### Step 8 — Memories

- [x] Complete

**[CLAUDE]**

The memory templates written at dispatch, at resolution and on giving up, with the tag, related actors,
location, importance and type rules above, and `{scheme}` rendered as a verb phrase.

**Verification:** a suite asserts, per verb, exactly who receives which template:

- at dispatch, `agent_dispatched` to the agent and `mastermind_dispatched` to the mastermind, or
  `self_dispatched` alone when the mastermind is their own agent;
- on success, `agent_succeeded` to the agent and nothing to a mastermind who sent somebody else;
- on failure, `agent_failed` to the agent and `mastermind_told_failed` to a mastermind who sent somebody else;
- on a timeout, `agent_timed_out` and `mastermind_told_timed_out` the same way;
- `mastermind_conceded` to the mastermind when the plot is given up by decision, by the adaptation cap, or for
  want of an agent, and to nobody when it ends because the mastermind died;
- and nothing to anybody else in the step.

Done. `include/PlotMemories.h` with `src/PlotMemories.cpp`: three pure functions decide who keeps which memory at
dispatch, resolution and giving up, and `Write` sends them through `SkyrimNetAPI::AddMemory` to each keeper's actor
reference. `PlotTick` calls it after a step is dispatched, after one resolves, and from `EndPlot` for a plot conceded
or out of attempts. 458 tests pass, three of them new, each run across all thirteen verbs.

**The importance and type of each template are fixed here.** Sending, being sent and deciding to do it yourself are
0.45; succeeding 0.55; running out of time 0.55 for the agent and 0.5 for the mastermind hearing of it; failing 0.75
and 0.7; giving up 0.6. Hearing about a step is `KNOWLEDGE`; everything else is `EXPERIENCE`. The suite pins the
property the numbers exist for, that a failure outweighs a timeout in both voices.

**A memory relates everybody else in the step** -- mastermind, agent and every person slot, never the keeper -- and
its location is the step's place where the verb has one and nothing otherwise, as the design says.

**Every memory carries the tag `narrative_engine_plot`.** Nothing reads it yet. Two consequences are Phase 17 Step
21's to settle rather than this step's: gossip's harvest does not exclude the tag, so a plot memory can already become
a rumor, which is the loop the feature was justified by; and plot birth's memory query does not exclude it either, so
a mastermind's next scheme can be shaped by how the last one went, which answers the audit's finding that every
scheme started from nothing.

**A mastermind who dies keeps nothing**, and neither does anybody when a plot ends because its mastermind died.

### Step 9 — In-game validation

- [ ] Complete

**[USER]**

Replaces Phase 17 Step 18. With a decision provider configured, run a session long enough for plots to finish.

1. Do the verbs chosen make sense for the scheme, and the people chosen make sense for the verbs?
2. Do leaders delegate to plausible subordinates?
3. Talk to a mastermind and an agent mid-step and after it. Do they know what they did, for whom, and how it went?
4. Find a mastermind whose agent failed. Do they know who failed them, and at what?
5. Record the decision-request rate per in-world day.

**First attempt (2026-10-11): the plans were not coherent, so validation stopped at check 1 and Steps 10–12 were
added.** Five plots were born. Read from the requests in `SkyrimNet.log`, every request carried the plan so far, and
the model answered as if it had not:

| Mastermind     | Scheme                                      | Plan                                                                                 |
| -------------- | ------------------------------------------- | ------------------------------------------------------------------------------------ |
| Talen-Jei      | steal three flawless amethysts from Maven   | steal a diamond from Hemming Black-Briar, six times, to the cap                      |
| Eltrys         | steal the internal ledger from the Treasury | steal a set of written orders from Rhiada, six times, to the cap                     |
| Thaena         | convince Dagur to ban mages from the inn    | denounce Dagur to Korir, twice                                                       |
| Galdrus Hlervu | forge a ledger framing Othreloth            | watch him, denounce him, steal a letter from him, plant it on him, deliver it to him |
| Anuriel        | forge the deeds for a Black-Briar orchard   | steal a deed from Maven, then plant it among Maven's belongings                      |

Four causes, each of which Steps 10–12 answer:

1. **The decision model matches; it does not plan.** Asked "what next?", it picks the option whose words best fit
   the scheme, whatever the plan already holds. Step 2's probe could not see this: every round had one answer that
   fit its description, which is exactly what this model is good at. Laying out a sequence is not that, and asking
   it to was the design's mistake, not the model's.
2. **Nothing refuses a repeat**, so a model that answers `steal` every time fills the plan to its cap.
3. **Nothing orders the steps.** Denouncing before there is any evidence, and planting a thing back on the person it
   was taken from, are both orders the verb manifest says nothing against.
4. **The scheme's own people can miss the list.** Maven was not among the twelve people Talen-Jei was offered for
   the mark, so the model took Hemming, whose line mentions her. Talen-Jei knows nothing of Maven by the ladder's
   reckoning, but a scheme that names her shows they know of her.

Run again once Steps 10–12 are done. The second attempt is recorded after Step 12, and led to Steps 13–16.

### Step 10 — The scheme's own people head every list

- [x] Complete

**[CLAUDE]**

A person the scheme names is offered first for any person slot whose rule admits them, and is admitted to a slot
whose rule is about whom the mastermind knows (`AnyPerson`, `KnownToMastermind`, `NotHostileKnown` unless hostile,
and `AwareOfEarlierStep`'s fallback) even when the ladder calls them a stranger. A rule about what somebody is --
trades for a living, senior in a court, rostered standing -- still has to be met. Their line says they are named in
the scheme.

A person is named when one of the words of their name appears in the scheme as a whole word and belongs to at most
two people's names in the population: "Maven" names Maven Black-Briar; "Black-Briar", shared by her whole family,
names none of them. This only ever ranks and admits; the model still chooses.

The item list carries the whole pool. It was capped at twenty, and the pool has twenty-one items.

**Verification:** a suite asserts that a named stranger heads a `KnownToMastermind` list, that a named person who
does not trade is still kept off a `TradesForLiving` list, that a surname shared across a family names nobody, and
that every pool item is offered.

Done. `PlotCandidates::NamedIn` reads the names, `Request::scheme` carries the scheme to every slot planning asks
about, and a named person's line reads "Brand-Shei (lives in Riften; named in the scheme)". 460 tests pass, one of
them new, with five new sections in the candidate suite.

**A word, not a full name.** Schemes say "Othreloth" for Elder Othreloth and "Maven" for Maven Black-Briar, so
matching whole names would miss the people schemes name most. A word of a name counts when it is three letters or
more, stands whole in the scheme with a trailing `'s` dropped, matches case, and is carried by at most two people's
names. "Black-Briar" is carried by five, so a scheme against the family names none of them, and the model chooses
from the ladder as before.

**Ranking only, never resolving.** The prior-art failure modes of fuzzy name resolution do not apply: a false match
puts one extra person at the top of a list the model still chooses from, and a missed one leaves the list as it was.

**The planning suite's first fixture changed answer.** Its scheme, "Ruin Hulda's trade", names Hulda, so she is now
first rather than second on the watch list, and the scripted answer moved from `c2` to `c1`.

### Step 11 — Preparations in the verb manifest

- [x] Complete

**[CLAUDE]**

Each verb lists the verbs that may **prepare** it -- be done just before it, to make it work -- and which of the
preparation's slots are bound to which of its own. Bound slots are what keep a plan one story: the watch before a
theft watches the person about to be robbed, and nobody has to choose that.

| Verb            | Prepared by                                                                                                                                                   |
| --------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `watch`         | `ask_around` (mark = mark)                                                                                                                                    |
| `ask_around`    | nothing                                                                                                                                                       |
| `steal`         | `watch` (mark = mark), `ask_around` (mark = mark), `bribe` (open)                                                                                             |
| `buy`           | `ask_around` (mark = seller)                                                                                                                                  |
| `deliver`       | nothing beyond getting the item                                                                                                                               |
| `plant`         | `watch` (mark = mark), `bribe` (open)                                                                                                                         |
| `bribe`         | `watch` (mark = mark), `ask_around` (mark = mark)                                                                                                             |
| `win_over`      | `ask_around` (mark = mark)                                                                                                                                    |
| `threaten`      | `watch` (mark = mark), `ask_around` (mark = mark)                                                                                                             |
| `spread_rumors` | `watch` (mark = mark), `ask_around` (mark = mark)                                                                                                             |
| `denounce`      | `plant` (mark = mark), `steal` (mark = mark), `watch` (mark = mark), `spread_rumors` (mark = mark), `win_over` (mark = authority), `bribe` (mark = authority) |
| `sabotage`      | `watch` (mark = mark), `ask_around` (mark = mark), `bribe` (open)                                                                                             |
| `silence`       | `ask_around` (mark = witness)                                                                                                                                 |

**Getting an item is not a preparation the model may skip.** A verb with a bound item -- `deliver`, `plant` -- needs
an earlier `steal` or `buy` of that very item, and planning puts one in front of it every time (Step 12).

**Verification:** the verb suite asserts every preparation names a real verb, every binding joins two person slots
that both exist, no verb prepares itself, and no open slot of a preparation is bound.

Done. `PlotVerbs::kPreparations` holds the table above, twenty-four rows, and `PlotVerbs::PreparationsOf` lists one
verb's. 461 tests pass, one of them new.

**A flat table beside the verbs, not a field on each.** Every verb is a positional aggregate whose last member is its
nine templates; a preparations array added to it would sit after them in every entry, far from the slots it binds.
A table of rows reading "steal is prepared by watch, mark = mark" reads the way this step's table does.

**A binding is a pair or nothing.** The suite asserts a row binds both ends or neither, so an unbound `bribe` can
never half-bind, and that both ends of a binding are person slots: places and items are never carried from one
step to another by a preparation. The item a `plant` needs is got by Step 12's obtain, not bound here.

### Step 12 — Planning backwards from the goal

- [x] Complete

**[CLAUDE]**

Planning is turned around so that every question is one the model can answer by matching:

1. **The goal.** "Which kind of step, once done, accomplishes the scheme?" -- every offerable verb, as before. Its
   slots are filled as before. An item slot with nothing already in hand is chosen from the pool.
2. **Getting the item.** A step with an item that nobody holds gets a `steal` or a `buy` of that item placed just
   before it; which of the two is a decision when both can be filled. The person it is got from is never one of the
   people in the step it serves, so nothing is planted back on whoever it was taken from.
3. **Preparations.** "What must be done first for this plan to work?" Each option is a concrete step -- a
   preparation of one of the planned steps, its bound slots already filled in, saying which step it comes before --
   plus `ready`. The chosen one goes immediately before the step it prepares, its open slots are filled without
   offering anybody already in that step, and an item it needs is got as in 2. Asked again until `ready` or the cap.

**Nothing repeats.** An option that would repeat a step already planned, or already in the plot's history, is not
offered; and the last slot a step asks never offers the filler that would complete a repeat.

**The cap still bounds the plan.** A preparation is offered only when there is room for it and for the step that
gets its item. A preparation that cannot be filled, or an answer that was not offered, ends the preparing and keeps
the plan as it stands, which is already coherent; only the goal's failures reject a plan.

**Re-planning after a failure is the same, given the history:** an item a succeeded step already got is bound
rather than got again, and nothing that already happened is offered. A bound agent is whoever got that item, matched
by the item rather than by being the latest theft.

**The trace shows the plan.** A `PLAN` line per step follows `BORN` and a substitute's `ADAPT`, and both carry how
many decisions the plan took -- most of what Step 9's fifth check asks for.

**Verification:** a suite drives the planner with scripted answers and asserts the goal is asked first and lands
last; a `deliver` goal gets a theft or purchase before it from somebody other than its recipient; a preparation
lands immediately before the step it prepares with its bound slot filled; a model that answers the same thing every
time cannot repeat a step; `ready` and the cap each end the plan; an item already in hand is bound without being got
again; and a decision error aborts the birth.

Done. `PlotPlanning::Plan` is a small planner that asks the goal, gets items and offers preparations, through three
new templates under `prompts/decisions/narrative_engine/`: `plot_goal` (replacing `plot_verb`), `plot_obtain` and
`plot_prepare`. `plot_slot` now also tells the model what the step is for. The trace gains `PLAN` lines after `BORN`
and after a substitute's `ADAPT`. 463 tests pass; the planning suite was rewritten as four cases, eighteen sections.

**Every question is now a matching question.** The goal question asks which step accomplishes the scheme, which is
what the model was already answering when it was asked "what next?" -- now that answer is put last instead of
repeated. The obtain question is steal or buy for a named item. A preparation option is a whole step, its person
already filled in and saying which step it comes before: "Before step 2: Someone watches Maven Black-Briar, learning
their habits." The model never has to hold a sequence in mind; the code builds the sequence from where each answer
goes.

**Repeats are kept out at two points.** A preparation option whose verb and bound people match a step already in the
plot is never offered, which is what keeps "watch Hulda" from being offered again once it is planned. And the last
slot a step asks drops any filler that would make the whole step equal one already there, which covers the goal when
re-planning: a plot that has already watched Hulda, re-planning a watch, can only watch somebody else.

**The people a step serves are off limits to whatever serves it.** A preparation's open slots, and the step that
gets an item, exclude every person in the step they serve (`PlotCandidates::Request::excluded`). That is what stops
a deed being stolen from Maven and planted back among Maven's belongings, the Anuriel plan from Step 9's first
attempt, and a bribe before a theft paying the person being robbed.

**An item in hand means a step that succeeded.** `PlotCandidates::Bind` now binds only an item a succeeded step got;
before, it bound the latest step that would get one, which was right for a forward plan and wrong for a history. A
plan's own steps never bind, because each item it needs is chosen from the pool and got by a step placed in front of
it. `ItemHolder` takes the step and matches its item, since a plot can now get two different items.

**Where the decision count goes.** The count is on the first `PLAN` line rather than on `BORN`, so a substitute's
re-plan reports its own count the same way. It covers planning only; the agent decisions at dispatch and the adapt
question are one each and visible as `DISPATCH` and `ADAPT` lines.

**A bad preparation answer ends the preparing, not the plan.** Everything planned before it already holds together,
so an unoffered key, or a preparation whose slots cannot be filled, keeps the plan as it stands. Only the goal's
failures reject a plan, and a decision that is not answered still aborts the birth.

**`PlotBirth.h` had a stray comment.** `Compose`'s documentation had drifted above `Occupation` when Steps 5 and 6
added declarations between them; it is back on `Compose`, which now also reports the decision count.

**Second attempt at Step 9 (2026-10-11): coherent sequences, wrong ends.** Five plots, no repeats, every
preparation in front of the step it serves. What was still wrong traces mostly to the scheme rather than the plan:

| Mastermind     | Scheme                                         | Plan                                                                |
| -------------- | ---------------------------------------------- | ------------------------------------------------------------------- |
| Falk Firebeard | document the bribes paid by Thane Erikur       | denounce Erikur to Laila Law-Giver -- one step, to Riften's jarl    |
| Idesa Sadri    | compile a ledger of Torsten Cruel-Sea's lapses | watch Hillevi, steal a dossier from her, hand it to Torsten himself |
| Nenya          | falsify the tax reports on Siddgeir's spending | watch Helvard, steal a letter from him, hand it to Siddgeir         |
| Atar           | forge reports of Forsworn attacks on caravans  | ask around and watch Cosnach, steal a sealed letter, deliver it     |
| Malur Seloth   | steal a spellbook from Nelacar                 | six steps, three of them about Enthir, ending in stolen orders      |

1. **The scheme is written without the vocabulary.** The ambition call is never shown the verbs or the item pool,
   so it writes schemes about documenting, compiling, forging and falsifying, which no verb does, and about a
   spellbook the pool does not hold. The goal is then the nearest verb, and Step 10's ranking makes the scheme's
   named target the likeliest recipient of a delivery -- which is how Torsten and Siddgeir are handed the evidence
   against themselves.
2. **The decision model does not weigh how a candidate is related.** Elisif headed the authority list as Falk's own
   jarl; the model took Laila Law-Giver, whose name matches a scheme about bribes.
3. **`ready` is offered from the first preparation question,** so a plan can be a single step.
4. **Preparations prepare preparations.** The bribe of Enthir, a bystander to the theft, got an ask-around and a
   watch of its own, filling Malur's plan to the cap with steps about the wrong person.

### Step 13 — The scheme is written in the plan's vocabulary

- [x] Complete

**[CLAUDE]**

The ambition call is shown every verb, as its generic sentence, and every item in the pool. The scheme must be one
act whose final step is one of those verbs, naming each person that step involves -- whom it is done to, and for a
delivery or a denunciation, to whom -- and naming an object only as the pool lists it. The call also returns `goal`,
the id of that final verb, which the parser holds to the manifest. Birth takes the goal as given and asks no goal
question; a re-plan after a failure still asks, since the history changes what can finish the scheme.

**Verification:** a parser suite asserts a known `goal` is kept and an unknown or missing one is refused, and the
planning suite asserts a given goal skips the goal question and is planned last.

Done. `narrative_engine_plot_ambition.prompt` gains a "What a scheme can end in" section listing every verb by id
and generic sentence and every pool item, and asks for `goal`; `ParseAmbition` refuses a goal `PlotVerbs::Find` does
not know, which `AskTwice` puts back to the model once with the reason. `PlotPlanning::Input::goal` carries it into
planning. 464 tests pass, one of them new, with two new sections in the planning suite.

**The goal is chosen by the call that can reason.** The ambition call is a full chat model that already decides what
the scheme is; deciding what it ends in is the same judgement. Choosing the goal there, and writing the scheme as that
act, removes the one planning question that asked the decision model to judge rather than match.

**A goal that cannot be filled falls back to asking.** If the verb the scheme ends in has nobody to fill a slot --
nobody bribable, say -- planning asks the goal question as before rather than throwing the birth away.

**The example in the prompt names no one.** "Denounce the thane to the jarl for taking bribes" shows the shape
without handing every mastermind the same thane and jarl to reach for.

**`PlotVerbs::Generic` is the verb as a kind of step.** It was `VerbLine` inside planning; the ambition context needs
the same sentence, so it moved beside the other renderers.

### Step 14 — A denunciation goes to the mark's own court

- [x] Complete

**[CLAUDE]**

`SeniorInCourt` admits the authorities of the mark's hold when there are any, then the mastermind's, then everybody
with authority. Who hears a denunciation is a matter of jurisdiction, which code knows and the model demonstrably
does not weigh.

**Verification:** the candidate suite asserts the mark's jarl alone is offered when the mark's hold has one, and the
fallbacks in order.

Done. The `SeniorInCourt` case of `PlotCandidates::Build` narrows the authorities to the mark's hold, then the
mastermind's, then leaves them all. 464 tests pass, with three new sections in the candidate suite.

**Narrowed, not reordered.** Elisif was already first on Falk Firebeard's list, and the model took the eighth name.
Putting the right court first does nothing for a model that does not weigh order or relation, so the other holds'
authorities are left off the list whenever the right hold has one. The usual result is a list of the hold's jarl,
steward and guard captain, which the model can choose among on what each one's line says they do.

**Hold, not settlement.** A jarl's court sits in one city but rules the hold, so a thane in a farmhouse outside
Solitude is still Elisif's to hear about. Hold is also the field every population member carries; a settlement is
missing for some.

### Step 15 — A plan is more than one step

- [x] Complete

**[CLAUDE]**

`ready` is offered only once a plan has two steps, as `done` was in the forward planner: one step is an errand, not a
scheme. A goal nothing can prepare still makes a one-step plan, because then there is nothing to offer.

**Verification:** the planning suite asserts the first preparation question offers no `ready` and the second does.

Done. The preparation question adds `ready` once the plan holds `kPlanFloor` steps. 464 tests pass; the planning
suite's "ready" section now asserts both questions, and three sections that had answered `ready` at the first
question answer a preparation instead.

**The floor is the same constant as the item floor.** Two steps is both the least room a goal needing an item is
given and the least a plan may be; one name keeps them from drifting apart, and either reading of it is right.

**A one-step plan is still possible, and only when nothing can prepare the goal.** Then the question is never asked
at all. Every verb but `ask_around` and `deliver` has a preparation in the table, and a `deliver` always brings the
step that gets its item, so in practice that leaves an `ask_around` goal, which is itself a preparation verb.

### Step 16 — Only the goal and what supplies it are prepared

- [x] Complete

**[CLAUDE]**

Preparations are offered for the goal and for the steps that get items, never for a preparation. A watch before a
theft is part of the theft; a watch before the bribe before the theft is a plan about the person bribed.

**Verification:** the planning suite asserts that once a preparation is planned, none of its own preparations is
offered, while the goal's remaining preparations still are.

Done. The planner keeps, beside each planned step, whether it may be prepared: the goal and every step placed to get
an item may, a preparation may not. 465 tests pass, one of them new.

**A step that gets an item is prepared like the goal.** It is part of the line the scheme runs along -- the theft of
the deed is how the deed is planted -- so watching the person it is stolen from is as fair a preparation as watching
the goal's mark. That includes the theft placed in front of a `plant` that is itself a preparation, since that theft
is still how the evidence comes to exist.

**Two preparations of one step now land in the order they were chosen.** Each goes just before the step it serves,
so a watch chosen first and an ask-around chosen second read "watch, ask around, steal"; when the ask-around could
prepare the watch, it read the other way round. The planning suite's sequence section was changed to match.

**Malur's plan, re-run by hand against the new rules,** keeps the ask-around and watch of Nelacar and the bribe of
Enthir, and loses the ask-around and watch of Enthir: four steps, all about the theft.

**Third attempt at Step 9 (2026-10-11): plans that hold together, schemes that do not explain themselves.** Thaena
steals skooma from Kraldar to plant on Nelacar; Betrid asks around about Raerek before spreading talk about him;
Helvard takes a dossier from Dengeir for Siddgeir. Two plots were wrong:

1. **Nirya's scheme named the player.** "Denounce Faralda to Varian" -- Varian is the player's character, made
   Arch-Mage in this save, and named throughout Nirya's memories. The ambition prompt says not to involve the
   player without saying who the player is, and the parser refuses only the word "Dragonborn". Varian is not in the
   population, so planning denounced Faralda to Korir instead -- the only authorities `SeniorInCourt` knows are a
   hold's civic court, never the leadership of the organisation the mark belongs to.
2. **Orthus Endario's scheme has nothing to do with his ambition.** His ambition is a transfer to Solitude; his
   scheme is to steal Suvaris Atheron's logbook. Both come from his biography -- the transfer from his aspirations,
   the logbook from his relationships -- and whatever the model reasoned to connect them is thrown away. Step 13
   made this worse: a scheme written as its own last step is a bare act, "steal the logbook", which carries no
   purpose on the dashboard or in "as part of my plan to ..." in every memory.

### Step 17 — The player is named, and refused

- [x] Complete

**[CLAUDE]**

The ambition prompt is told the player's name in its "Not about the player" section, and the parser refuses an
ambition, scheme or finishing step that names the player -- by any word of their name, matched whole and with its
case, as well as by "Dragonborn". The name is read on the plugin thread and handed to the plot worker, which never
touches the player itself.

**Verification:** the parser suite asserts an answer naming the player by name is refused, one naming nobody passes,
and a word of the name inside another word, or in lowercase, does not count.

Done. `PlotStepParse::NamesThePlayer` takes the player's name beside the title it always checked; `ParseAmbition`
passes it to every field it reads; `PlotTick::Poll` stores the name each poll for `PlotBirth::PlayerName` to read; the
ambition prompt names the player under "Not about the player". 467 tests pass, two of them new.

**Read every poll, not once a session.** The name is "Prisoner" until character creation and can change with
`showracemenu`, so a copy taken at load would be wrong for a new game's first hours. Reading it costs one string per
poll; the plot worker reads only the copy, under a lock, and never the player.

**The prompt says what to do about the player holding a post.** Nirya's memories made Varian Arch-Mage, so the
College's authority, in her eyes, is the player. Told only "not the player", a model has nowhere to turn; told that
somebody else will have to be the one a scheme turns to, it has.

**Case matters for the name and not for the title.** "Dragonborn" is never anybody's name, so any case of it counts;
a character's name can be an ordinary word -- Hunter, Wolf, Brand -- and only its capitalised form is the name. Words
under three letters are ignored for the same reason.

### Step 18 — A denunciation goes to whoever has authority over the mark

- [x] Complete

**[CLAUDE]**

`SeniorInCourt` becomes `AuthorityOverMark`: first whoever leads, or is senior in, a rostered organisation the mark
belongs to and stands above the mark in it -- the College's senior mages for a College mage, the family head for a
Black-Briar -- then Step 14's hold courts. Courts are rostered organisations themselves, so a steward is denounced
to their jarl by the first rule.

**Verification:** the candidate suite asserts a member of an organisation is denounced to its senior members ahead
of any jarl, never to a junior or to themselves, and that a mark in no organisation falls through to Step 14's order.

Done. `Rule::AuthorityOverMark` replaces `SeniorInCourt`, and its case in `PlotCandidates::Build` offers the mark's
superiors before falling to Step 14's hold courts. 467 tests pass, with two new sections in the candidate suite.

**A superior is senior and above the mark, in the same rostered organisation.** Senior is standing 0.6, the line
PlotBirth already draws for "who is senior in it", so the junior members of an organisation are never who a
complaint is taken to. Above the mark is strict, so two Masters of the College are not one another's authority.

**The organisation's head is often the player, and then the next senior stands in.** Nirya's College had Varian as
Arch-Mage. The player is not in the population, so the College's other senior mages are the list -- the same
substitution Step 17's prompt asks the scheme call to make.

**The rule covers courts without a case for them.** Courts are rostered organisations of their own, with the jarl
leading, so a steward's superior is their jarl by the first rule. The hold courts are only reached for a mark who
belongs to nothing with a hierarchy, or who heads what they belong to.

### Step 19 — The scheme says what it is for; the finishing step says how it ends

- [x] Complete

**[CLAUDE]**

The ambition call returns the scheme and its last step separately:

- `scheme` is again what the mastermind is trying to bring about, written freely as before Step 13 -- "prove the
  Shatter-Shield clan is in league with the Blood Horkers" -- and must be a step toward the ambition and not another
  thread of the biography. The dashboard and every memory quote it.
- `goal` and `finish` are the step that ends it: a listed verb, and that act written out with its people and any
  item -- "steal the Shatter-Shield logbook from Suvaris Atheron". Planning plans `goal` last and ranks the people
  `finish` names first.

The plot keeps `finish` in the co-save, so a re-plan after a failure reads it too; the record version goes to 10.

**Verification:** the parser suite asserts `finish` is read and must open with a verb; the planning suite asserts
people named in `finish` head the goal's lists and that the slot question is told how the scheme ends; the co-save
suite round-trips `finish`.

Done. `AmbitionOutcome::finish`, `PlotModel::Plot::finish` and `PlotPlanning::Input::finish` carry it; the co-save
writes it after the scheme at record version 10; `PlotCandidates::Request::naming` replaces `scheme`, and planning
passes the scheme and the finish together; every decision template shows "It is meant to end with this" under the
scheme; the `PLAN` header line prints it. 468 tests pass, one of them new, with new sections in the parser, planning
and co-save suites.

**The scheme prompt is back to purposes, with one new rule.** The scheme is again written in whatever verb suits
it, and a new paragraph requires it to move along the same ambition rather than another thread of the biography,
with the reasoning written first and required to say how. Orthus's biography offered the transfer and the logbook
as two threads, and nothing asked the model to join them.

**The finish carries the people.** It is held to the scheme's instruction form, names the people its step involves,
and is what makes Step 10's ranking useful: a scheme to "expose the bribes" names nobody to rank, while its finish
names the thane and the jarl.

**A re-plan reads the finish too.** It is the end a plot is still trying to reach after a failure, so it is saved
rather than rebuilt; the goal question a re-plan asks sees it in its "aims" block.

**Memories quote the scheme, unchanged in code.** "As part of my plan to expose Thane Erikur's bribe-taking" is the
purpose again; Step 13's "as part of my plan to denounce Erikur to Elisif" said only what the plan's last step was.

**A finish naming the player is refused like everything else.** Nirya's "denounce Faralda to Varian" would now be
a finish, and Step 17 refuses it there.

**Fourth attempt at Step 9 (2026-10-11): schemes with purposes, plans that end where they say.** Five plots, each
scheme a purpose ("create a fake debt for Thongvor Silver-Blood", "discredit Viola Giordano's amateur investigation
into the Butcher"), each plan ending in its finish, two to four steps. One thing was wrong: Thonar Silver-Blood's watch
of Reburrus Quintilius was caught, and the adaptation answered `retry`, sending Betrid Silver-Blood to watch Reburrus
again. A failure means the route it was on does not work; trying the same step with somebody else is not adapting.

### Step 20 — Adapting finds another way

- [x] Complete

**[CLAUDE]**

A failure abandons a route, and the re-plan must find another one to the same finish:

1. **What is abandoned.** The failed step, and every later planned step it served -- the theft a failed watch was
   preparing, and what that theft supplied, short of the finish itself. A step serves another when it is one of its
   preparations with the bound person the same (Step 11's table), or when it gets the item the other uses. Each is
   recorded on the plot as its verb and the person it acts on, and stays abandoned for the rest of the plot, across
   every later adaptation. The plot's co-save record goes to version 11.
2. **What the re-plan may not do.** No step of an abandoned verb against its abandoned person, anywhere in the plan
   -- not as the goal, a preparation, or the step that gets an item. A failed theft from Reburrus leaves the letter to
   be got from somebody else, or bought. When the finish itself failed, its own verb against its own person is
   abandoned, so the plot has to end some other way.
3. **What the adaptation question offers.** Another way, or giving up. `retry`, `CanRetry`, `Retried` and
   `FailedAgents` go: no step can recur, so there is nobody to keep a retry away from.

**Verification:** a suite asserts a failed preparation abandons itself and the step it prepared but not the finish
or anything unrelated; a failed finish abandons itself; a re-plan never offers an abandoned step as goal, preparation
or supplier; the adaptation question offers no retry; and the abandoned routes survive the co-save.

Done. `PlotModel::Plot::abandoned` holds the ways given up on; `PlotPlanning::RouteOf` and `Serves` work out what a
failure abandons; `Adapt` offers another way or giving up and records the route before re-planning; the planner drops
abandoned ways from every target list; the co-save writes them at record version 11; the trace notes each one after
`ADAPT` and then lists the new plan. 470 tests pass; the adaptation suite was rewritten around Thonar's plot as three
cases, and the co-save suite gained a section.

**A way is a verb against a person, not a whole step.** A failed theft of the letter from Reburrus abandons stealing
from Reburrus, not only stealing that letter: he caught somebody at it, and robbing him of anything else is the same
road. The person is the verb's first person slot -- the one it acts on -- so a failed denunciation of Faralda to Korir
abandons denouncing Faralda, to anybody.

**What the failure served is worked out from the plan, not recorded at planning.** A step serves a later one when it
is one of that step's preparations with the bound person the same, or when it gets the item the later step uses --
the same two links planning made. Working it out from the steps themselves means nothing new is saved per step, and
a plan restored from an older co-save is read the same way.

**The finish survives a failure on the way to it.** Thonar's failed watch of Reburrus abandons the watch and the theft
it prepared, and keeps the planting on Thongvor and the watch of Thongvor, which the failure did not touch; the
re-plan still ends in the planting, and gets the letter some other way. Only a failure of the finish itself closes it.

**Abandoned for the whole plot.** Each adaptation adds to the list and none removes from it, so a second failure
cannot route back through the first.

**Retry is gone, with what kept it honest.** `CanRetry`, `Retried` and `FailedAgents` existed so a retried step went
to somebody new. No step can now recur, so dispatch no longer filters for them. An answer of `retry` from a model that
remembers the old option concedes, as any unoffered answer does.

### Step 21 — A verb's id in prose is written as words

- [x] Complete

**[CLAUDE]**

The fourth attempt's Viarmo wrote his finish as "spread_rumors about the court historian around Solitude": the id
from the list, not the act in words. Such an answer is not refused. Every verb keeps its id's plain words -- its
dashboard caption, lowercased: `spread_rumors` is "spread rumors", `win_over` "win over", `ask_around` "ask
around" -- and the parser writes every id that stands as a whole word in the scheme or the finish as those words.
`goal` stays an id, since that is what it is for.

**Verification:** the verb suite asserts every verb's words carry no underscore and say its id with spaces; the
parser suite asserts an id in the finish or scheme is written as words, and that an id inside a longer word is left
alone.

Done. `PlotVerbs::Words` gives a verb's plain words and `PlotVerbs::Plain` writes every id in a text as them;
`ParseAmbition` passes the scheme and the finish through it before checking that each opens with a verb. 471 tests
pass, one of them new, with a new section in the parser suite.

**The caption is the replacement, so there is nothing new to keep in step.** Every verb already carries a
title-case caption for its node on the dashboard -- "Spread Rumors", "Win Over" -- which is its id said in words. The
verb suite holds each one to its id with spaces, so a verb added later cannot have a caption that mends its id into
something else.

**An id is mended only where it stands as a word.** A word is a run of letters, digits and underscores, so
"watchtower" is not `watch` and "my_spread_rumors_plan" is not `spread_rumors`. Ids without an underscore come out
the same as they went in.

**Fifth attempt at Step 9 (2026-10-11): adaptation takes other roads.** Eleven plots; seven adaptations, each
abandoning the failed way and planning around it -- Hogni Red-Arm's caught watch of Banning became an ask-around and
a bribe before the theft, and Thonar's caught theft from Thongvor got the letter from Betrid instead. Four things
were wrong:

1. **Two plots conceded on a template error.** `plot_prepare` was sent with one option: Quintus Navale's re-plan was
   one step long with one preparation to offer, and Step 15 withholds `ready` until a plan has two steps. SkyrimNet
   refuses a choice question with fewer than two options, so the re-plan failed and the plot conceded.
2. **A failed finish was adapted around.** Hogni's theft from Banning was the finish; the re-plan stole from Betrid
   Silver-Blood instead, under a finish still naming Banning. The user's ruling: a failed finish fails the scheme.
3. **The scheme's target was hired as a helper.** Raerek's re-plan paid Thongvor Silver-Blood -- the man his scheme
   is against -- to look the other way, because Step 10 puts named people first on every list, an unbound bribe's
   included.
4. **A finish named an object the pool does not hold.** Malur's "steal a spellbook from Nelacar" was planned as a
   theft of written orders.

Brelyna's watch of Tolfdir, held for its whole budget while the player stood nearby and then timed out, was
abandoned like any failure; the user ruled that correct.

### Step 22 — A question with one option is not asked

- [x] Complete

**[CLAUDE]**

The preparation question with a single option and no `ready` takes that option without asking, as a slot with one
candidate already does, and so does the goal question when only one verb can be filled. A choice question is only
ever sent with two options or more.

**Verification:** the planning suite asserts a one-step plan with one possible preparation plans it without a
question, and a goal question with one fillable verb is not asked.

Done. `Prepare` takes the lone option when the list it built has one entry, and `Goal` takes the lone verb when only
one can be filled. 471 tests pass; one section is new, and six whose scripts answered a one-option question now
script one answer fewer.

**The goal half is not exercised by the suite.** The fixture province can fill most verbs, and building one where
exactly one verb is fillable would be a fixture for a single line; the goal change is the same `size() == 1` test as
the preparation's, beside the same comment. The planning suite's first case moved from a watch, which has one
preparation, to a theft, which has two, so it still covers what a preparation question looks like.

**Why the other questions were already safe.** A slot is asked only with more than one candidate, the obtain question
only when both a theft and a purchase can be filled, and the adaptation question always offers two answers.

### Step 23 — A failed finish fails the scheme

- [x] Complete

**[CLAUDE]**

When the step that fails is the plan's last -- the finish -- the plot ends there, failed, with a new outcome,
`finish_failed`, and no adaptation question is asked. The finish is the end the scheme was for; a different act
against somebody else is a different scheme. The agent's and the mastermind's failure memories are written as for
any failure, and no concession memory is: the mastermind did not give it up, it was lost.

**Verification:** a suite asserts a failed finish ends the plot with `finish_failed` and asks nothing, and a failed
step before the finish still adapts.

Done. `PlotModel::PlotOutcome::FinishFailed` is new, `PlotPlanning::FinishFailed` says whether the step that failed was
the plan's last, and the tick ends such a plot before adaptation is reached. 471 tests pass; the `RouteOf` case for
a failed finish became the two `FinishFailed` sections, since a failed finish no longer has a route to abandon.

**This replaces part of Step 20.** Step 20 abandoned a failed finish's own verb against its own person and re-planned
to some other ending; Hogni's plot showed where that leads -- the same theft from somebody else, under a finish still
naming Banning. Now a failed finish is not adapted at all, so `RouteOf` never meets one.

**The test is pure; the ending is one call.** Whether a failure is the finish's is a function of the plot's cursor and
plan, asserted in the suite; the tick's branch calls it and `EndPlot`, which already writes nothing for an outcome
other than a concession or the cap, so the mastermind keeps the "told it failed" memory and no "gave up" memory.

### Step 24 — The scheme's people are not hired as helpers

- [x] Complete

**[CLAUDE]**

A preparation with no bound person -- the bribe before a theft, the bribe before a sabotage -- never offers anybody
the scheme or its finish names. Those people are what the plot is about; paying one of them to look the other way
is a different plot.

**Verification:** the planning suite asserts an unbound preparation's list leaves out the people the scheme names,
while a bound one still takes them.

Done. The planner reads who the scheme and finish name once, and adds them to the excluded people of every
preparation whose slot is open. 472 tests pass, one of them new; the bound case is the existing "preparation needs an
item" section, which still plants on Hulda, whom the scheme names.

**Only open slots, because bound ones are the point.** A watch of the mark, the planting on the person denounced, the
win-over of the authority -- each is bound to a person of the step it prepares, and that person is often the one the
scheme names. What Step 24 stops is a slot with no tie to anybody being filled by Step 10's ranking, which puts the
named first on every list and so put Thongvor first on Raerek's bribe list.

### Step 25 — A finish names its object from the pool

- [x] Complete

**[CLAUDE]**

When the goal is a verb with an item -- `steal`, `buy`, `deliver`, `plant` -- the parser refuses a finish that names
none of the pool's items, and says which items there are, so the one retry can choose among them. An item counts as
named when its name, without its leading article, appears in the finish: "a deed to a property" is named by "steal
the deed to a property from Hemming".

**Verification:** the parser suite asserts a finish naming a pool item passes, one naming an object the pool lacks is
refused with the list, and a goal with no item is never checked.

Done. `ParseAmbition` takes the pool's names and refuses a finish that needs one and names none; birth passes the
same names the ambition prompt is shown, from one `PoolItemNames`. 472 tests pass, with three new sections in the
parser suite.

**A verb takes an item when it has a thing slot.** That is `steal` and `buy`, whose item is chosen from the pool, and
`deliver` and `plant`, whose item is got by an earlier step -- all four need the finish to say which.

**The match is generous on purpose.** The article is dropped and case ignored, so "the deed to a property" names
"a deed to a property"; anything stricter would refuse finishes that are right. A finish naming a pool item in
passing while meaning another object would slip through, which costs no more than the planner choosing the item
from the list as it does today.

**The refusal carries the list,** so the one retry `AskTwice` allows is asked with exactly the names it may use.

**Sixth attempt at Step 9 (2026-10-11): too few verbs.** Fourteen plots, sound plans, and a clear pattern across this
run and the five rotations behind it: schemes keep asking for acts the manifest does not have, and planning answers
with the nearest one. Eight schemes wanted a forgery -- the orchard deeds, the tax reports twice, the reports of
Forsworn attacks, the fake debt in three runs, the prophetic warning, the contract naming Hemming heir -- and got a
stolen "incriminating letter" from somebody uninvolved. Five wanted proof dug up and got a theft. Two wanted
blackmail and got a theft that stopped short of using it. Two wanted records destroyed, one of which sabotaged an
innocent trader's livelihood because `sabotage` needs a trader. Four wanted something granted -- a transfer, funding,
a ship, a leadership bid -- and one of them bought a purse of septims. Seven named a kind of record the pool lacks.

### Step 26 — Records in the item pool

- [x] Complete

**[CLAUDE]**

The pool gains a merchant's ledger, a set of research notes and a private journal as documents, and a spellbook as a
valuable, each a vanilla record checked against the Spriggit export. A new candidate rule, `DocumentPool`, admits
only the pool's documents, for the verbs of Step 27 whose item has to be paper.

**Verification:** the candidate suite asserts `DocumentPool` lists the documents and nothing else.

Done. `PlotItems.ini` gains `dunPOIChillwindJournal2` as "a merchant's ledger", `dunAlftandDwemerStudy01` as "a set
of research notes" and `dunHarmugstahlJournal01` as "a private journal" among the documents, and
`SpellTomeCandlelight` as "a spellbook" among the valuables; `Rule::DocumentPool` and its case in
`PlotCandidates::Build` admit the documents alone. 472 tests pass, with a new section in the candidate suite.

**Vanilla has no ledger.** Every book in the Spriggit export was searched by its English name; nothing is called a
ledger or a book of accounts, so "a merchant's ledger" is the Chillwind Depths "Merchant's Journal", the nearest
anonymous book of trade. The research notes and the journal are likewise the anonymous ones, not Calcelmo's or
Kodlak's, so no plot about one person's papers carries another person's quest item.

**The spellbook is a real spell tome.** Candlelight is the cheapest and most common; a plot that delivers it puts a
tome in somebody's hands that teaches the spell, which is what a stolen spellbook should do.

### Step 27 — `falsify` and `dig_up`: verbs that make documents

- [x] Complete

**[CLAUDE]**

Two verbs that obtain an item, so either can supply a `deliver` or a `plant` as `steal` and `buy` do:

- `falsify` -- `{agent}` falsifies `{item}` in `{subject}`'s name. Item: a document. Subject: anybody, the person whose
  name or hand the forgery bears, aware on failure. Profile `acquire`. Prepared by `ask_around` (mark = subject).
  The word is `falsify`, not `forge`, so a model never reads it as smithing.
- `dig_up` -- `{agent}` digs up `{item}` proving `{mark}`'s wrongdoing. Item: a document. Mark: known to the
  mastermind, aware on failure. Profile `locate`. Prepared by `watch` and `ask_around` of the mark; and it prepares
  `denounce` (mark = mark).

A step that needs a document nobody holds may get it by falsifying it as well as by stealing or buying it; the
person a forgery is in the name of is not kept out the way the person robbed is, because framing somebody with a
paper in their own name is the point.

**Verification:** the verb suite's rules hold for both; the planning suite asserts a needed document offers
`falsify` beside `steal` and `buy`, a needed valuable does not, and a falsified document supplies a `plant`.

Done. `falsify` and `dig_up` join `PlotVerbs::kVerbs` with the templates above; `kPreparations` gains four rows; the
planner's `Obtain` offers `falsify` for an item the pool lists as a document. 473 tests pass, one of them new; the
verb suite's rules -- templates, awareness, the place rule, preparation bindings -- passed for both verbs unchanged.

**The forgery supplies a delivery rather than a planting in the suite.** The planting chain is the same `Serves` link
through the item, which the new case asserts directly; the fixture's only documents are what a delivery of a deed
needs.

**`dig_up` is a `locate`.** Its resolution is the investigation row `ask_around` already uses, which is the closest
of the eight: the risk is being noticed asking, not being caught in the act.

**Neither verb is a preparation of anything but what Step 27 named.** `dig_up` prepares a denunciation; the
blackmail it is also for arrives in Step 28 with the verb that uses it.

### Step 28 — `blackmail`, `destroy` and `petition`

- [x] Complete

**[CLAUDE]**

- `blackmail` -- `{agent}` blackmails `{mark}` into serving `{mastermind}`. Mark: known to the mastermind, aware on
  contact. Profile `recruit`. Prepared by `dig_up`, `steal` and `watch` of the mark: the leverage, got first.
- `destroy` -- `{agent}` destroys `{item}` belonging to `{mark}`. Item: anything in the pool. Mark: known to the
  mastermind, aware on failure. Profile `sabotage`. Prepared by `watch` of the mark and an open `bribe`.
- `petition` -- `{agent}` petitions `{authority}` on `{mastermind}`'s behalf. Authority: by `AuthorityOverMark`,
  which with no mark in the step answers for the mastermind -- their own superiors first, then their hold's court.
  Aware on contact. Profile `recruit`. Prepared by `win_over` and `bribe` of the authority.

**Verification:** the verb suite's rules hold for all three; the candidate suite asserts a petition's authority is the
mastermind's superior where they have one and their hold's court where they do not.

Done. The three verbs join `PlotVerbs::kVerbs`, eighteen in all, with the templates above; `kPreparations` gains
seven rows, thirty-five in all; `AuthorityOverMark` takes the mastermind as its subject when the step has no mark.
473 tests pass, with two new sections in the candidate suite; the verb suite's rules passed for all three unchanged.

**One rule serves denunciation and petition.** Both ask whoever has authority over somebody: the person denounced, or
the person asking. Splitting it would have meant two copies of the superiors-then-courts order to keep in step; the
mark's absence already says which person a step is about.

**`destroy` takes anything in the pool, not only paper.** Records are what the run's schemes wanted destroyed, and
they are in the pool now; but ruining a rival's mead or a spellbook is the same act, and limiting it to documents
would leave the sabotage of things to `sabotage`, which only reaches a trader's livelihood.

**What the scheme call sees changes with no prompt edit.** The ambition prompt lists every verb from the manifest,
so all five new verbs are offered as finishes as soon as they exist; Step 21's plain words cover `dig_up` the same
way they cover `spread_rumors`.

### Step 29 — Recipes, and the people who make them

- [x] Complete

**[CLAUDE]**

Steps 30 and 31 add five crafting verbs, one per craftsman the game data can identify: `smith`, `concoct`,
`enchant`, `set` and `fletch`. This step gives them what they need to stand on: the recipes, who can follow them, and
a resolution profile for making things. Tailoring, brewing and cooking are left out. Vanilla has no clothing or
alcohol recipe at all, and nothing in it marks anybody as a cook.

A new module, `PlotRecipes`, is loaded at `kDataLoaded` beside the item pool. It is built from the loaded forms, so
modded recipes and modded craftsmen count the same as vanilla ones. Each recipe has a craft, the item it makes as it
is said, the skill it asks for, and the number and worth of its ingredients:

- **`smith`, `set`, `fletch`:** constructible objects made at the forge (`CraftingSmithingForge`), the Skyforge or
  Dawnguard's workbench.
  - An armor piece covering nothing but a ring, amulet or circlet slot is `set`.
  - A bow, a crossbow, an arrow or a bolt is `fletch`.
  - Any other weapon or armor is `smith`.
  - Anything else made there is skipped.
- **`concoct`:** every potion and poison that is not food and has a name, so long as two different ingredients carry
  its costliest effect. Vanilla has no recipe records for potions; this shared effect is what makes one in the game.
  Its ingredients are the cheapest such pair.
- **`enchant`:** every playable weapon or armor piece that has an enchantment and a template whose name it begins
  with ("Iron Sword of Burning" from "Iron Sword"). These are the enchanted variants of base items. Its ingredients
  are the base item and a soul, and the soul is worth what the enchantment adds to the item's value.
- **Skill stands in for perks.** A forge recipe's `HasPerk` conditions name perks, and each perk's own
  `GetBaseActorValue Smithing` condition gives the skill that perk needs. The highest of these is the recipe's
  skill; Daedric work, for example, asks for 90. Other conditions are ignored. Potions and enchantments have no perk
  gates in vanilla, so they ask for no skill.
- **One recipe per name within a craft:** the duplicate kept is the one asking the least skill.

A member of the plot population practises a craft when vanilla's job factions or vendor classes say so:

| Craft     | Profession                                                                                |
| --------- | ----------------------------------------------------------------------------------------- |
| `smith`   | `JobBlacksmithFaction`, class `VendorBlacksmith`                                          |
| `concoct` | `JobApothecaryFaction`, class `VendorApothecary`                                          |
| `enchant` | `JobCourtWizardFaction`, `JobTrainerEnchantingFaction` -- vanilla has no enchanter's job  |
| `set`     | `JobJewelerFaction`                                                                       |
| `fletch`  | `JobFletcherFaction`, class `VendorFletcher`                                              |

The skill profile cached on each member gains Smithing, Alchemy and Enchanting. `set` and `fletch` are smithing work.

A ninth resolution profile, `craft`, sizes the making of a thing. It is never conspicuous: work at your own bench is
not something to hide from the player.

**Verification:** a new suite covers the pure half of `PlotRecipes`:

- which craft a made thing belongs to;
- how a recipe's item is said: with its article, and arrows and bolts as a sheaf;
- that duplicates collapse to the least demanding recipe;
- that more and costlier ingredients never make a step easier.

The load logs how many recipes each craft has.

Done. `PlotRecipes` is two files: the pure rules in `PlotRecipes.cpp` and the reading in `PlotRecipesLoad.cpp`, the
split the item pool uses. `Plugin.cpp` loads it after the pool, and `PlotMenus::World` carries the result as
`recipes`. Each member's `crafts` bits come from the table above, and their skills from the three new fields of the
skill profile. `StepType::Craft` takes a base threshold of 10 and a budget of 6 ticks, the same scale as a watch.
478 tests pass, with a new suite for the recipes' pure half; the plugin builds with no warnings.

**The forge's three crafts are told apart by what they make, not where.** Jewelry, bows and swords share one bench in
vanilla, so the bench says only that something is forge work. An armor piece is jewelry when every body slot it covers
is a ring, amulet or circlet slot, which keeps a circlet that is also a helmet out of a jeweler's hands.

**Ingredient worth is a log scale.** An iron dagger's ingredients are worth about ten septims and Daedric armor's
about a thousand, so a linear scale would put nearly everything at the bottom. Worth is three quarters of the figure
and the number of ingredients a quarter; the suite asserts neither can make a job smaller.

**A step's craft skill is read at dispatch, not by `Suitability`.** `Suitability` takes a step type, and one type
covers five crafts with three skills; its `craft` row answers a neutral 0.5 that Step 30's dispatch replaces with the
agent's own skill.

### Step 30 — Five crafting verbs

- [x] Complete

**[CLAUDE]**

`smith`, `concoct`, `enchant`, `set` and `fletch` join the manifest. Each has one slot, the thing made, and the
`craft` profile. Each obtains its item, so a later `deliver` or `plant` can use what was made, the way `steal`,
`buy` and `falsify` already supply them. The templates are above; every person a step names other than the agent and
the mastermind is in a slot, and these verbs have no person slot.

**Who makes it.** The agent must practise the craft and have the recipe's skill, on top of everything the casting
ladder already asks.

- The ladder is walked as usual, rung by rung, but a candidate who cannot make the thing is not on it.
- The mastermind does it themselves only if they can make it.
- If somebody who can make it exists but is busy, the step waits.
- If nobody can make it at all, the plot cannot go on.

**What may be made.** A craft verb's item slot lists that craft's recipes that somebody the mastermind could send
(or the mastermind) is able to make. That is the ladder without regard to who is busy right now, filtered by craft
and skill. Recipes named in the scheme or its finish come first; the rest are spread across the range of worth, so
the list is not every Daedric piece in the game.

**How hard it is.** The ingredients are not part of the plot; they are part of the difficulty. A crafting step's
importance comes from its recipe's ingredients, from how many there are and what they are worth, in place of a
target's standing. Travel is nothing, since the work is done at the crafter's own bench. How well the agent suits the
step is their skill in that craft.

**Never a finish on their own.** Making a thing and keeping it is an errand, and these verbs have nothing that
prepares them, because gathering ingredients is not part of the plot. So `Goal` never offers them, and the scheme
call does not list them as finishes. A gift is a `deliver` of something made (Step 31).

**A failed craft is abandoned by its item.** A verb with no person slot is abandoned against its thing, so a re-plan
does not try to make the same item again.

**Verification:**

- the verb suite's rules hold for all five;
- the candidate suite asserts a craft verb lists only what a craftsman the mastermind can reach is skilled enough to
  make;
- the casting suite asserts a crafting step's agents all practise the craft, and that the mastermind stands alone
  only if they can make the thing;
- the resolution suite asserts the `craft` profile sizes like any other.

Done. The five verbs join `PlotVerbs::kVerbs`, twenty-three in all, with the templates above. Each item slot carries
one of five new rules, `SmithRecipe` to `FletchRecipe`, which `PlotVerbs::CraftOf` maps back to a craft.
`PlotCasting::EligibleAgents` takes an `admit` filter, and dispatch passes `CanMake` for the step's recipe. 481 tests
pass: new sections in the casting and candidate suites, and a new resolution suite; the verb suite's rules passed for
all five unchanged.

**There was no resolution suite.** Step sizing had never been tested on its own. The new suite is small, and covers
only what the `craft` row adds: its sizing grows with the job, it round-trips through its id, and it is not held near
the player.

**Who the mastermind can reach ignores who is busy.** The candidate rule asks `QualifyAgent` with an empty occupancy
table, so a smith off on another plot's errand still counts when the plan is laid. Dispatch is where busy matters,
and there the step waits for them, as a delivery waits for whoever holds its item.

**Only the item slot names what is made.** None of the five has a person slot, so the step has no target. A failed
craft is abandoned against its item, by `TargetSlot` falling back to the verb's first slot.

### Step 31 — What is made, in a plan

- [x] Complete

**[CLAUDE]**

A delivery or a planting whose item nobody has got chooses from the pool **and** from what the mastermind's
craftsmen can make. A thing named in the scheme or its finish comes first, then the pool, then a spread of the rest,
up to the usual cap. When the chosen thing is something a reachable craftsman can make, the craft verb is offered
beside `steal` and `buy` as a way to get it. The crafter then holds it, and the bound agent of the delivery or the
planting is them:

```text
1. Endon makes a Gold Diamond Necklace at a jeweler's bench.
2. Endon brings a Gold Diamond Necklace to Elisif the Fair.
```

That pair is the gift a craft can finish a scheme with. A concocted poison can be planted the same way.

The scheme call is shown a few things the mastermind's people can make, up to four per craft spread across the range
of worth, and is told that a delivery or a planting may use them. A finish that names one of them is accepted as one
naming a pool item.

**Verification:** the planning suite asserts:

- a delivery of a crafted necklace plans the jeweler's step first and the delivery after it, served by it;
- a planted poison is concocted first;
- a crafted thing is not offered as something to make when nobody reachable can make it.

The ambition parse accepts a finish naming a crafted thing it was shown.

Done. A new rule, `PoolOrMade`, is what an unbound item slot now falls back to, in place of `ItemPool`.
`PlotCandidates::CanBeMade` lets `Obtain` offer a craft verb, and `PlotCandidates::MadeExamples` is what `PlotBirth`
shows the scheme call as `made`, adding the same names to the items its finish may name. The ambition prompt lists them
under the pool. 482 tests pass, with a new planning case, two new candidate sections and a new parse section; the
plugin builds with no warnings.

**The room left after the pool is shared between crafts, not between recipes.** Enchanted variants outnumber every
other recipe by an order of magnitude, so one spread over all of them together would fill the list with enchanted
gear. Each craft with something to offer gets an even share instead.

**Making a thing keeps nobody out.** A theft serving a delivery never robs the person it is for. Nobody is robbed in
the making of a necklace, so a craft step, like a forgery, takes no exclusions from the step it serves.

**The suite's gift is offered to steal or make, not to buy.** The recipient it delivers to is Hulda, the fixture's only
trader, and a delivery is never bought from the person it is for. That is the existing rule, and the case keeps it.

### Step 32 — Making, finding and hiding are never the finish

- [x] Complete

**[CLAUDE]**

The seventh attempt at Step 9 produced twenty plots, and twelve of them stopped short of what their scheme was for:

- **Eight ended in a forgery,** every one a frame or a discredit, the forged paper put nowhere.
- **Two ended in finding proof** (`dig_up`) shown to nobody: "expose the financial corruption of Erikur".
- **Two exposures ended in a theft:** "expose the secret debts of Jarl Igmund" by stealing a ledger from Raerek.
- **Five frames ended in a planting,** the thing hidden and nobody pointed at it.

Not one ended in a denunciation. The scheme call writes the scheme first and then takes the verb its wording matches:
"fabricate evidence" becomes `falsify`. Nothing on the list takes a document and does something with it.

Asking the decision model whether a finish accomplishes its scheme would not help. It matches words, and would answer
that `falsify` finishes "falsify the quarterly tax reports". The verbs say it themselves instead:

- **A verb now says whether it can finish a scheme.** `falsify`, `dig_up` and `plant` cannot, and nor can the crafting
  verbs, which Step 30 kept out of the goal another way; that way becomes this one. What a forgery or a found ledger is
  for is a later step, and a planted thing is evidence only once somebody is sent to look for it.
- **`Goal` and the scheme call's list leave them out.** The ambition parse refuses a goal that is not on the list, with
  the reason, for the one retry.
- **`spread_rumors` can be prepared by `plant`,** as `denounce` already is. Those two are what lead people to find what
  was planted, so a frame ends in one of them, with the planting before it.

`steal` and `buy` stay finishes. "Acquire the lycanthropy research from Heimskr" really does end at the theft.
Making them finish only some schemes is Step 33's job, by giving an exposure a verb of its own.

**Verification:** the verb suite asserts which verbs finish; the planning suite asserts `Goal` offers none that do
not, and that a rumor may be prepared by a planting; the parse suite asserts a goal that cannot finish is refused.

Done. `PlotVerbs::Verb` gains `finishes`, true unless set, and false for `plant`, `falsify`, `dig_up` and the five
crafting verbs. `Goal` and the scheme call's verb list read it in place of Step 30's `CraftOf` test, and
`ParseAmbition` refuses such a goal, saying it only prepares for a later step. `kPreparations` gains the planting
before a rumor, thirty-six rows in all. 483 tests pass, with a new planning case and new sections in the verb and parse
suites; the plugin builds with no warnings.

**Step 31's poisoned frame is now a denunciation.** Its case planted a concocted poison as the goal, which this step
forbids. It now plants the poison as a preparation of denouncing the person it is planted on, the shape a frame takes
from here on: concoct, plant, denounce.

**The refusal names the reason, not only the list.** A goal the list does not show could be a typo, but one of these
is a step the model knew and chose, so the retry is told what the step is for.

### Step 33 — `expose`: proof, shown to somebody with authority

- [x] Complete

**[CLAUDE]**

A new verb, `expose` -- `{agent}` shows `{item}` to `{authority}` as proof against `{mark}`:

- **Item:** bound, as a delivery's is. The agent is whoever got it.
- **Mark:** anybody, aware never, as in `denounce`.
- **Authority:** by `AuthorityOverMark`, aware on contact.
- **Profile:** `discredit`. Prepared by `win_over` and `bribe` of the authority.
- **Templates:** above.

Planning backwards then builds what these schemes were reaching for: steal, dig up or falsify the ledger, then show it.
Two changes to `Obtain` make it read right:

- **`dig_up` is a way to get a document,** beside `steal`, `buy` and `falsify`. When the step it serves has a mark, the
  proof is dug up against that mark, without asking; otherwise the mark is asked for as usual.
- **The proof may be taken from the person it exposes.** A theft serving a step never robs anybody in that step, so
  the bribe before a theft never pays the person robbed. For an exposure that rule is wrong: the ledger proving
  Erikur's corruption is Erikur's. The exposure's mark may be the one robbed; its authority still may not.

**Verification:** the verb suite's rules hold for `expose`. The planning suite asserts:

- an exposure's document can be stolen from its mark, dug up against them, bought or falsified;
- a dug-up proof is about the exposure's mark without a question;
- what was got supplies the exposure.

Done. `expose` joins `PlotVerbs::kVerbs`, twenty-four in all, with the templates above, and `kPreparations` gains its
two rows, thirty-eight in all. `Obtain` now builds each way of getting an item with what it is given: the slots it
starts with and who it keeps out. 484 tests pass, with a new planning case; the delivery of a forged deed now offers
`dig_up` too, and its case says so. The verb suite's rules passed for `expose` unchanged.

**`dig_up` keeps people out only when it is asked whom to dig into.** Serving a step with a mark, it is dug up against
that mark, so there is nobody to keep out. Serving a delivery, it is asked whom the proof is against, and the
recipient is not offered, as for a theft.

**Only an exposure lifts the rule on robbing.** A planting's mark is the person framed, and robbing them of what is
then planted on them would be a different scheme. The exception is written as one, against `expose` by name, rather
than as a property every verb would have to declare.

### Step 34 — A plan acts on the people its scheme names

- [x] Complete

**[CLAUDE]**

The same run planned four plots against somebody other than the person their scheme named:

| Plot | The scheme and its finish named | The plan acted on | Why |
| ---- | ------------------------------- | ----------------- | --- |
| 1 | "Beirandering", nobody | Sven | The name is two names run together, and matches nobody |
| 2 | Svidi | Hemming Black-Briar | The finish's person was offered first, and another was chosen |
| 3 | Lentilus Batiatus, nobody | Urag gro-Shub | Nobody of that name is in Skyrim |
| 5 | Jarl Idgrod | Idgrod the Younger, after a re-plan | The forgery's subject was asked again, and both Idgrods carry the name |

The plugin knows who exists, so four rules close the gaps:

- **A finish names somebody who exists.** When the goal acts on a person, the ambition parse refuses a finish that
  names nobody in the plot population other than the mastermind. The reason it gives says so, and the one retry can
  name somebody real.
- **The goal is done to whom the finish names.** Filling the goal, a person slot whose list holds anybody the finish
  names offers only them, and one is taken without asking. Being ranked first was not enough: the decision model chose
  past Svidi. A slot whose list holds nobody the finish names is asked as before, so the rules still decide who may
  fill it.
- **A re-plan keeps the goal.** Adaptation never abandons the finish, so a re-plan now plans back from the goal step
  as it was planned, every slot kept, rather than asking for the goal again.
- **A forgery serving a step with a mark is in that mark's name,** without asking, as a dug-up proof is about them
  (Step 33). Framing Jarl Idgrod with a letter means a letter in her name, not in the name of somebody else who
  shares it.

**Verification:**

- the parse suite asserts a finish naming nobody who exists is refused, and one naming somebody is not;
- the planning suite asserts:
  - a goal slot offers only the person the finish names when it lists them;
  - a re-plan keeps every slot of the goal and asks no goal question;
  - a forgery for a planting is in the mark's name.

Done. 485 tests pass, with a new planning case and a new parse section; the plugin builds with no warnings.

- **The parse check.** `ParseAmbition` takes a `namesSomebody` test, which `PlotBirth::Compose` builds from
  `PlotCandidates::NamedIn` with the mastermind left out.
- **Narrowing the goal.** The planner keeps whom the finish alone names. `FillSlots` narrows a goal's person slots to
  them, when any are on the list.
- **Keeping the goal.** `PlotPlanning::Input` gains `goalStep`, which `PlotBirth::Replan` sets to the plan's last step.
- **The forgery's subject.** `Obtain` gives a forgery serving a step with a mark that mark as its subject, as Step 33
  gives a dug-up proof its mark.

**Every person slot of the goal is narrowed, not only its target.** A finish that says "bring the necklace to Laila"
names a recipient; one that says "denounce Hulda to Balgruuf" names an authority. Narrowing only the first person slot
would have left the second to the old ranking, which is what chose past Svidi.

**Two planning cases changed with the rule.** Step 13's case asserted that the finish's person was offered first beside
the scheme's. It now asserts the goal is done to the finish's person without a question. Step 31's crafted gift no
longer asks for its recipient, and so may be bought from Hulda, whom it is no longer delivered to.

**A kept goal that can no longer be filled asks the goal question as before.** That happens when its verb has stopped
finishing or a slot's person has been given up on. A re-plan can still find a way; a goal it cannot keep is not a
reason to concede.

**What stays open.** A scheme that names two people sharing a name word still offers both: "Jarl Idgrod" names Idgrod
Ravencrone and Idgrod the Younger alike. The re-plan that went astray on her is closed, since the goal and the
forgery's subject are both held now; a fresh plot naming her by the shared word alone still asks.

### Step 35 — Probe: picking a sentence's nouns out of its own words

- [x] Complete

**[CLAUDE]** rebuilds the **Probe decisions** button around the question below.
**[USER]** presses it and records the results here.

The eighth attempt at Step 9 showed that a finish's prose does not say which person fills which role. "Expose a
merchant's ledger to Jarl Elisif" names whom the proof is shown to, and Step 34 put Elisif in the slot for whom it is
against. Steps 36–37 have the scheme call name each role outright. This probe measures the alternative: leave the
finish as prose, and have the decision model pick each role's words out of it.

The method under test:

1. Take a sentence of 120 characters or less naming several things: "Bob ran to Whiterun to purchase 9 red claws from
   Alan Grimes".
2. Split it into words, and build every run of consecutive words: "Bob", "Bob ran", ..., "Alan Grimes", "Grimes". A
   sentence of twelve words gives seventy-eight. A run that repeats an earlier one's words is offered once.
3. Ask one decision question per role -- "What was purchased?", "Who was the seller?" -- with every run as a choice.

Each question is right when the chosen run is one of its accepted answers. A sentence can name a thing with or without
its article, so "9 red claws" and "red claws" are both right. A choice that contains the answer or lies inside it is
counted as **near**, because trimming a run is cheap and choosing the wrong one is not.

Every question also offers "(not named in the sentence)". One sentence is the finish that went wrong, and is asked for
whom the proof is against: the right answer there is that the sentence does not say. That is the case a reader of
finishes has to get right, so it is measured beside the others.

Each question is asked twice, the runs in the order they were built and reversed, as the second probe did. The report
gives each answer, where the right one sat among how many, and the probability the model gave it. The totals are right,
near and wrong: overall, by order, and for the question with no answer in its sentence.

**Verification:** the probe's suite asserts the runs a sentence gives, that every sentence is within 120 characters
and every accepted answer is one of its runs, and how an answer is judged.

**The `[CLAUDE]` half is done; the run is the user's.** The button now asks the questions below, each twice, 46
requests in all. The second probe's rounds are replaced; their results are recorded under Step 2.

| Round            | Sentence                                                                         | Chars | Choices | Questions                                       |
| ---------------- | -------------------------------------------------------------------------------- | ----- | ------- | ----------------------------------------------- |
| `purchase`       | Bob ran to Whiterun to purchase 9 red claws from Alan Grimes                     | 60    | 78      | what was bought, the seller, the buyer, where   |
| `expose`         | Show a merchant's ledger to Jarl Elisif as proof against Erikur                  | 63    | 67      | what is shown, to whom, against whom            |
| `expose_unnamed` | Expose a merchant's ledger to Jarl Elisif                                        | 41    | 29      | against whom (not named), to whom               |
| `denounce`       | Denounce Vignar Gray-Mane to Jarl Balgruuf for hiding stolen gold in Whiterun    | 77    | 79      | who is denounced, to whom, where the gold is    |
| `plant`          | Hide a bottle of skooma among Hulda's belongings at the Bannered Mare            | 69    | 79      | what is hidden, whose belongings, where         |
| `steal`          | Steal a deed to a property from Maven Black-Briar in Riften                      | 59    | 66      | what is stolen, from whom, where                |
| `gift`           | Bring a gold diamond necklace to Elisif the Fair in Solitude                     | 60    | 67      | what is brought, who receives it                |
| `follow`         | Hrolfdir asked Sorli to follow the merchant Fenja from the Riften docks to ...   | 108   | 188     | who is followed, who follows, where it ends     |

Choices count the runs and "(not named in the sentence)". `follow` is the longest a round may be, so its 188 choices
also test how long a list the decision API takes. The template asks for all of the words that name the answer and no
others, and to choose not named when the sentence does not say.

**First run (2026-10-12), 46 requests: 44 right, 1 near, 1 wrong.**

| Split                                         | Right | Near | Wrong |
| --------------------------------------------- | ----- | ---- | ----- |
| Questions the sentence answers (44)           | 43    | 1    | 0     |
| The question it does not answer (2)           | 1     | 0    | 1     |
| Runs as built (23)                            | 23    | 0    | 0     |
| Runs reversed (23)                            | 21    | 1    | 1     |

The requests averaged 268 ms. So:

- **The model picks out what a sentence names.** Every question whose answer was written there came back right or
  near: buyers, sellers, things, places, recipients, accusers, hyphenated and three-word names, and no more words than
  named them -- "Alan Grimes", not "from Alan Grimes".
- **A list's length and order do not matter.** All six questions on the 188-choice sentence were right, and right
  answers were chosen from first place, last place, #169 and #151 alike. The decision API takes at least 188 choices,
  at the same cost as eight.
- **It cannot tell that a sentence leaves a role out.** Asked whom "Expose a merchant's ledger to Jarl Elisif" is
  against, it chose "not named" at 51% one way and Jarl Elisif the other, giving "not named" 31%: the same mistake
  Step 34 made, pulling the one person named into the empty role.
- **The near was a possessive:** "Hulda's belongings" for "Hulda's", which matching against names would read as Hulda.

Reading a finish's roles out of its prose would be reliable for what it names and would not notice what it leaves
out, which is the case it was for, at the cost of a decision per role. Steps 36–37 go ahead as written: the scheme
call names each role itself, a missing one is a missing key the parse refuses, and no decision is spent. Picking runs
out of a sentence stays available for reading text a model wrote freely.

### Step 36 — The scheme call names each role of its finish

- [x] Complete

**[CLAUDE]**

The scheme call answers with a `roles` object beside `finish`, naming whatever fills each slot of the goal verb, keyed
by the slot's name:

```text
"goal": "expose",
"finish": "show a merchant's ledger to Jarl Elisif as proof against Erikur",
"roles": { "item": "a merchant's ledger", "mark": "Erikur", "authority": "Jarl Elisif" }
```

- **Each slot says what it is for.** The verb list the call is shown gives each verb's slots with a few words on each:
  `expose` has item, the proof; mark, whom it is against; authority, whom it is shown to. A slot's description is a
  new field in the verb manifest, so the list and the parse read the same table.
- **Every role is resolved, and checked against its slot's rule.**
  - A person resolves by their whole name, or by a word of it that nobody else's name carries.
  - A place resolves to a settlement by name.
  - A thing resolves to an item from the pool, or a thing the mastermind's people can make.
  - A resolved person must also be admitted by the slot's rule, without the scheme naming them counting as knowing
    them.
- **What fails comes back to the retry with what would not.** Each failure gets a sentence, and where the rule's
  list is short, the list:
  - a name that resolves to nobody alive;
  - a name two people share;
  - a person the rule does not admit;
  - a missing role.

  The eighth attempt's failures each meet one of these:
  - "Guard Grelod" is Grelod the Kind, whom a Markarth guard's enemy does not know.
  - Mogrul is in no plot.
  - Mirabelle Ervine was dead.
  - "Jarl Idgrod" is two people.
- **Jarl Balgruuf is refused for Vignar, with the reason.** The denunciation rule sends Vignar's to his superiors
  first, and the rule decides; the retry is told whom he answers to. A rule a finish could override would be
  no rule, and Step 18 made that one after the decision model sent Falk Firebeard's denunciation to Riften.

The roles are sanitized at extraction, as every string from a model is.

**Verification:** the parse suite asserts each failure above and a clean answer resolving every role, and that the
verb list shows each slot's description.

Done. 488 tests pass, with new cases in the candidate and parse suites and a new rule in the verb suite; the plugin
builds with no warnings.

- **The manifest.** `PlotVerbs::Slot` gains `says`, and every slot of every verb carries one.
- **The scheme call.** It is shown each verb's roles, and its prompt asks for `roles` as a hard constraint.
- **The parse.** `ParseAmbition` reads `roles`, sanitized, and refuses one naming the player as it refuses any field
  that does. It hands them to a `RoleResolver` and keeps what comes back as `AmbitionOutcome::roles`.
- **The resolver.** `PlotBirth::Compose` passes one that calls the new `PlotCandidates::ResolveRoles`.

The verification's examples are the suite's own: an unknown mark, a name nobody has, a dead one, a name two people
share, an authority the rule passes over, a missing mark, a thing that cannot be, and a place inside a longer phrase.

**A name word counts for less the more people carry it.** A person is whoever's name the role's words match best,
each matched word worth one over the number of names carrying it. "Grelod" alone settles Grelod the Kind, a word in a
dozen names barely counts, "Idgrod Ravencrone" beats Idgrod the Younger on the word only she carries, and "Jarl
Idgrod" ties them and is refused. Only capitalised words are read, so "the" in "Elisif the Fair" matches nothing.

**Each role is judged against what came before it.** Roles resolve in slot order, so an authority is checked against
the mark just resolved, as planning would fill them. When the mark could not be resolved, a slot that depends on it
is not judged at all, rather than being refused for reasons that only follow from the missing mark.

**The roles are resolved and not yet used.** Planning still fills the goal as Step 34 left it until Step 37.

### Step 37 — The goal is filled from the roles

- [x] Complete

**[CLAUDE]**

Planning takes the resolved roles as the goal step's slots, asking nothing about them: the goal is done to whom the
scheme call said, as it said. Three checks that read roles out of the finish's prose are removed, because this step
stops doing so:

- Step 34's narrowing of the goal's lists to whomever the finish names.
- Step 34's check that the finish names somebody.
- Step 25's check that the finish names a pool item. Its role does that now, and the prose check would refuse a
  finish that says "steal the ledger" when the item role names "a merchant's ledger". A re-plan still
keeps the goal it had, and a forgery for a step with a mark is still in that mark's name.

A goal whose roles can no longer be filled is asked for as before. That happens when a person has died since the
scheme call.

**Verification:** the planning suite asserts a goal planned from roles asks no slot question and fills each slot as
given, and that the Elisif case -- proof shown to Elisif against Erikur -- plans Erikur as the mark.

Done. 488 tests pass; the plugin builds with no warnings.

- **The goal from its roles.** `PlotPlanning::Input` gains `goalRoles`, which `PlotBirth::Compose` fills from the
  parse, and `Goal` plans from them before anything else.
- **What is removed.** `ParseAmbition` loses its `items` and `namesSomebody` parameters and the checks behind them.
  `FillSlots` loses its narrowing.
- **Tests.** The planning suite's case for the finish's people now covers roles; the Elisif case is played by Hulda,
  against whom a deed is shown to Laila by a finish that names only Laila. Step 13's case is back to its original form,
  the finish's people heading the list. Step 31's gift asks its recipient again. The parse cases for the removed
  checks are gone, with their ground covered by the role resolution suite.

**A preset person is checked for being alive.** Planning took any person it was given, a kept goal's or a role's,
without asking whether they were still there. The new case for a goal whose mark has died found it: the theft was
planned against nobody. `Fillable` now refuses a given person who is no longer in the population or no longer alive,
which covers a re-plan's kept goal as well.

**A goal whose roles fail keeps its verb.** It is the slots that are asked again, not the goal: the scheme call chose
the verb and it is still fillable, only not against the people named. That is the existing path for a goal the scheme
call chose, and the case says so rather than expecting the goal question.

### Step 38 — A name that finds nobody says why

- [x] Complete

**[CLAUDE]**

The ninth attempt at Step 9 planned every scheme against the people its roles named. It also refused four schemes
outright, against one before, each after its retry. Three of those named people who exist in the game, and every
refusal said only that the name "names nobody alive whom a plot can reach":

- **Rorlund**, the priest in Solitude's temple.
- **Mogrul**, the Raven Rock moneylender.
- **Lleril Morvayn**, Raven Rock's councilor.

The retries named the same people again, as a model that knows they exist would. There are three different reasons
somebody resolves to nobody, and the export shows two of them here:

- **Dead, or not in the world.** Rorlund and Lleril are both in their town's list of unique residents, which is what
  makes somebody a gossip participant, so both should be in the plot population. That leaves the liveness test, which
  refuses the dead and the disabled.
- **In no plot at all.** No location lists Mogrul as a resident, so the gossip graph never made him a participant. It
  leaves out 786 unique NPCs for that reason, beside 172 that are ghosts or not people, and the plot population leaves
  out children.
- **Nobody of that name.** An invented name, which the scheme call should replace.

The plot population now carries who it left out and why: each unique, named NPC the gossip graph passed over, with its
reason, and each child. A role that finds nobody among the living is matched against the dead, the disabled and the
left out, and its refusal says which:

- "Rorlund is dead or not in the world";
- "Mogrul cannot take part in these plots (no home location)";
- "'Jarl Vigilia Rulcanella' names nobody in Skyrim".

The refusal goes to the plots log and to the retry alike, so the log now answers what this run's could not.

**Verification:** the candidate suite asserts each of the three refusals, and that a living person who shares a name
with a left-out record still resolves to the living one.

Done. `GossipGraph::LeftOutNpcs` lists each unique, named NPC the graph passed over, with "no home location", "no home
in any hold", "a ghost" or "not a person". `PlotCasting::Population` gains `absent`, which `PlotPopulation::Build` fills
from that list and with the children it leaves out. Person roles in `ResolveRoles` go through a shared scorer: the
living first, then the dead and disabled, then the absent. 488 tests pass, with the candidate suite's refusal cases
rewritten for the three reasons; the plugin builds with no warnings.

**Rorlund and Lleril are in the population; Mogrul is not.** The export puts both of the first two in their town's
list of unique residents, which every participant comes from, and it lists Mogrul nowhere. The next run's refusals
will say whether the first two are dead or disabled in that save, which is the one thing the export cannot.

**The living win a shared name.** A ghost record named like a living person is in the absent list. The living are
scored first and the absent only when no living person matches, so the living person is never refused for it.

### Step 39 — A refused role is offered who would do

- [x] Complete

**[CLAUDE]**

A refusal for a rule now names who the rule admits, when the list is short. Every other refusal names nobody, so the
retry has nothing to choose from instead and names the same person again. Every refusal of a role now ends with whom
it could be: the whole list when there are eight or fewer, and otherwise the first eight, which are the people the
mastermind is closest to, as the rule ranks them. A thing still lists the pool.

**Verification:** the candidate suite asserts a nobody refusal, a dead one and a rule refusal over a long list each end
with candidates.

Done. `Admitted`, which ended a rule refusal with a short list, now ends every refused person and place role. It gives
the whole list when there are eight or fewer, else "it could be, among others," and the first eight. 488 tests pass,
with a new section in the candidate suite; the plugin builds with no warnings.

**The first eight are the closest, not a sample.** Candidate lists are ranked by how the mastermind knows each person,
then by standing. The eight a long list offers are the ones the scheme would most plausibly reach for, which is what a
retry replacing somebody it could not have should choose from.

**A name two people share is not given a list.** Its refusal already names the two it could be; anything more would
invite a third.

### Step 40 — A word many names share names nobody

- [x] Complete

**[CLAUDE]**

"Captain Veiled Lotus", an invented name, was refused as "could be Captain Valmir, Captain Aldis, Captain Metilius,
Captain Lonely-Gale, Captain Wayfinder or Captain Veleth". It should have been refused as nobody. A word of a name
counted for one over the number of names carrying it, so a title six people carry still counted, and tied all six.

A word carried by more than two names now counts for nothing, as the older finish check counted it. "Captain", "Jarl"
and "Guard" name nobody; "Idgrod", which two people carry, still asks which of them.

**Verification:** the candidate suite asserts an invented name behind a common title is refused as nobody, and that a
real name behind one still resolves.

Done. The scorer `ResolveRoles` shares across the living, the dead and the absent skips any word more than two of its
names carry. 488 tests pass, with a new section in the candidate suite; the plugin builds with no warnings.

**The same rule as `NamedIn`, for the same reason.** That check settled on two so a parent and child sharing a first
name are both found; past two a word is a title or a family name, and says nothing about which person is meant.
"Gray-Mane" and "Black-Briar" now count for nothing on their own, as they never did there: "Vignar Gray-Mane"
resolves by "Vignar".

### Step 41 — An authority judges only those below it

- [x] Complete

**[CLAUDE]**

The ninth attempt exposed Jarl Igmund to his own steward, and offered the orc chiefs Burguk and Larak as authorities
over a Markarth matter. `AuthorityOverMark` falls back to whoever holds authority in the mark's hold: the jarl, the
steward, the guard captain and every orc chief in it, as equals.

- **Authority has a rank.** Each member's authority comes from the job faction that gives it: jarl, then steward, then
  guard captain, highest first. An orc chief is an authority apart (below).
- **The court tiers admit only those above the mark.** An accusation against a steward goes to the jarl. One against a
  guard captain goes to the steward or the jarl. One against anybody holding no court office goes to any of them, as
  now. The mark's superiors in an organisation still come first, unchanged.
- **A top official is exposed to their whole organisation.** Skyrim has no throne a jarl answers to. When nobody in
  the mark's own hold outranks an official mark -- a jarl, or a steward whose jarl is gone -- the denunciation or
  exposure is made to the organisation they lead as a whole, not to a person. The authority slot holds no person, only
  how the organisation is said: the rostered faction the mark stands highest in ("the Court of the Reach"), else the
  people of the mark's settlement. Nobody hears it as a person, so nobody keeps a memory of hearing it: it works as
  spreading rumors about them does. A petition has no such audience: a mastermind nobody outranks has nobody to
  petition, and the verb is not offered. The scheme call is told this beside the verbs. A role naming a person as
  the authority over a top official resolves to the organisation, whoever it named.
- **An orc chief judges only their own stronghold.** A chief is the authority over somebody who lives in the chief's
  settlement, and over nobody else; for those people the chief stands where a hold's court would.

**Verification:** the candidate suite asserts:

- a steward's authority is the jarl, and not the guard captain;
- a jarl's is their organisation, with no person in the slot, and a jarl has nobody to petition;
- an orc in a stronghold answers to its chief, and a Markarth smith never to one.

Done. 489 tests pass, with a new case in the candidate suite; the plugin builds with no warnings.

- **The office.** `PlotCasting::Member` trades its `authority` flag for an `Office`: none, guard captain, steward, jarl
  or orc chief. `PlotPopulation` reads it from the job factions, highest first.
- **The order `AuthorityOverMark` tries:**
  1. the subject's superiors in an organisation;
  2. a stronghold's chief;
  3. the court of the subject's own hold, above them;
  4. an official's audience;
  5. the court of the mastermind's hold;
  6. any court, above them.
- **The audience.** It is a candidate with no form, its text the mark's highest rostered organisation -- "the court of
  Whiterun" -- or the people of their settlement. `ResolveRoles` resolves an authority role to it whatever the role
  named, and `Fillable` lets a given slot with no form through without asking whether it lives.
- **The prompt.** The scheme prompt says authority runs only upward, and how a jarl or a chief is exposed.

**A slot with no person is safe everywhere it goes.** The model's placeholder rule already covers it: memories, the
agent ladder's exclusions and awareness all gate on the form being set, and the co-save keeps a zero as a zero. The
audience is written into the step's sentences as its text, so a denunciation reads "denounces Balgruuf the Greater to
the court of Whiterun".

**An official's audience comes only after their own hold's court.** A steward whose jarl is alive answers to the
jarl; only a steward with nobody above in the hold -- the jarl dead or gone -- is exposed to the court as a whole. The
fallbacks past the audience still serve whoever holds no office, as before.

### Step 42 — A preparation makes its step easier

- [x] Complete

**[CLAUDE]**

The ninth attempt planned eleven of twelve schemes at two steps, the model answering `ready` at its first chance. A
preparation changes nothing when it succeeds: the step it was for is sized as though it had not happened. There is
nothing to be gained by preparing, so nothing teaches a plan to.

A preparation that succeeded now makes the step it prepared easier. At dispatch, each succeeded step of the plot that
prepared the step being sized -- bound to one of its people, as the preparation table binds it -- multiplies that
step's threshold by `fPlotPreparationEase`, 0.8 by default, so two preparations make it 64% of the work. A step that
only supplied an item does not count: getting the ledger is what lets it be shown, not what makes showing it easier.
Only the threshold moves; the budget does not, so the same time covers less work and the odds rise.

The dispatch line in the plots log says how many preparations eased the step, and by how much.

**Verification:** the planning suite asserts which earlier steps count as a step's preparations, a supplying step
among them not counting. The resolution suite asserts that easing shrinks the threshold and leaves the budget.

Done. 490 tests pass, with a new planning case and a new resolution section; the plugin builds with no warnings.

- **The setting.** `fPlotPreparationEase` is new in the `[Plots]` section of the ini, default 0.8.
- **The easing.** `PlotResolution::EaseThreshold` applies it once per preparation.
- **Which steps count.** `PlotPlanning::Prepares` says which earlier steps prepared a step: `Serves` without its
  item-supply link and its open preparations.
- **At dispatch.** `PlotTick` counts the succeeded preparations and eases the threshold after the budget is set.
- **In the log.** The DISPATCH line ends "prepared=N eased=x" when any did.

**No step is ever eased below a quarter of its work.** The suite caught the first version promising that no number of
preparations makes a step free, while many of them at a low ease rounded the threshold to nothing. A step keeps at
least a quarter of its work however it was prepared; at 0.8 that takes seven preparations, more than a plan holds.

**The budget is not eased.** It is set from the whole of the work before the preparations are counted, as the doc
step says, so `MinimumViableBudget` cannot shrink it along with the threshold and take the raised odds back.

**An open preparation still serves its step.** A bribe of nobody in particular keeps its place in the plan and still
falls with the step when the step is abandoned; it only does not ease it.

### Step 43 — A plan prepares until its odds are good

- [ ] Complete

**[CLAUDE]**

`ready` is no longer a choice. A plan is prepared until every step that may be prepared -- the goal, and what gets
its items -- has an estimated chance of success of at least `fPlotPlanOdds`, 75% by default. Only then, or when nothing
more can be prepared, or at the cap, is it done.

- **The estimate is the resolution model's own.** A pure function plays the step's race a few hundred times from a
  fixed seed, with its threshold eased by the preparations already planned for it. It uses the step's type, the
  target's importance or the recipe's, the progress rates and the mishap chance. The agent and the trip are not
  known until dispatch, so it assumes an ordinary agent, middling in competence and suitability, and a middling trip.
- **Only steps below the odds are prepared.** The preparation question offers preparations of those steps alone;
  with one on offer it is taken without asking, as now. A goal against a jarl, whose race is long, is prepared more
  than one against a farmhand, and the plan says why.
- **One step is still an errand.** With only the goal planned, a preparation is still added if one exists, whatever
  the odds.

The plan line in the plots log gives each prepared step's estimated odds.

**Verification:** the resolution suite asserts the estimate is repeatable, rises with each preparation and falls with
importance. The planning suite asserts:

- a plan whose goal is already likely stops after its floor;
- one against a hard target is prepared until its odds are met, or until nothing more can be prepared;
- no `ready` is offered.

---

## Open questions

1. ~~**Does the embedding model stay?**~~ Settled in Step 4 and done in Step 6: it had no caller once slots were
   filled by choice, and is removed.
2. **Does a rumor need content?** `spread_rumors` says ugly talk is being spread without saying what. A fourth
   slot kind, a closed list of accusations, would let memories say what was claimed. It is not a person, place
   or thing, so it is deliberately left out of this draft.
3. **Data file or code for the wording?** The draft keeps sentences and templates in the `constexpr` table beside
   their rules. A data file under `statics/` would let wording be tuned without a rebuild, at the cost of a loader
   that validates slot names at load.
4. **Do marks remember?** The template set has nobody but the agent and the mastermind. A bribed guard, a
   recipient handed a parcel, or a mark who caught their watcher would each need a template of their own, and
   awareness already says which of them should get one. A mark who catches somebody is also what Phase 17 Step 21
   would seed gossip from.
5. **Does the mastermind hear about a success?** There are templates for being told of a failure and of a
   timeout and none for a success, so a mastermind whose agent succeeds keeps only the dispatch memory of
   sending them.
