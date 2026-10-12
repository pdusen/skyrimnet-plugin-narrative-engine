# Phase 17 — Milestone 2: Rigid Steps

Milestone 1 (the original [`PHASE_17_FACTION_PLOTS.md`](PHASE_17_FACTION_PLOTS.md), Steps 1–17) built a plot
simulation whose masterminds, ambitions and schemes are good and whose steps are not. This milestone replaces how
a step is chosen, described and remembered. The mastermind draw, the ambition and scheme prose, the casting
ladder, the progress race and the co-save all stay.

> **Doc status: every `[CLAUDE]` step complete; Step 9's validation outstanding.** Steps 10–12 plan
> backwards from the goal, Steps 13–16 tie the plan to the vocabulary and bound the preparing, Steps 17–19 keep the
> player out, route denunciations by authority and give the scheme back its purpose, and Step 20 makes adapting find
> another way -- each added after an attempt at Step 9.
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

| Verb            | Sentence                                                          | Slots and rules                                                                                     | Awareness           | Profile   |
| --------------- | ----------------------------------------------------------------- | --------------------------------------------------------------------------------------------------- | ------------------- | --------- |
| `watch`         | `{agent}` watches `{mark}`, learning their habits                 | mark: person, known to mastermind                                                                   | mark: failure       | surveil   |
| `ask_around`    | `{agent}` asks around `{place}` about `{mark}`                    | mark: person, known to mastermind; place: settlement where the mark or someone close to them lives  | mark: failure       | locate    |
| `steal`         | `{agent}` steals `{item}` from `{mark}`                            | item: thing, item pool; mark: person, known to mastermind                                            | mark: failure       | acquire   |
| `buy`           | `{agent}` buys `{item}` from `{seller}`                            | item: thing, item pool; seller: person, trades for a living                                          | seller: contact     | acquire   |
| `deliver`       | `{agent}` brings `{item}` to `{recipient}`                         | item: bound to an earlier steal/buy; agent: bound to whoever got it; recipient: person              | recipient: contact  | deliver   |
| `plant`         | `{agent}` hides `{item}` among `{mark}`'s belongings              | item and agent: bound as for deliver; mark: person                                                  | mark: failure       | conceal   |
| `bribe`         | `{agent}` pays `{mark}` to look the other way                     | mark: person with standing in a rostered faction                                                    | mark: contact       | recruit   |
| `win_over`      | `{agent}` wins `{mark}` over to `{mastermind}`'s side             | mark: person, known to mastermind, not hostile to them                                               | mark: contact       | recruit   |
| `threaten`      | `{agent}` warns `{mark}` to stay out of `{mastermind}`'s affairs  | mark: person, known to mastermind                                                                   | mark: contact       | recruit   |
| `spread_rumors` | `{agent}` spreads ugly talk about `{mark}` around `{place}`       | mark: person; place: settlement, mark's or mastermind's                                             | mark: failure       | discredit |
| `denounce`      | `{agent}` denounces `{mark}` to `{authority}`                     | mark: person; authority: whoever the mark answers to (Step 18)                                      | authority: contact  | discredit |
| `sabotage`      | `{agent}` sabotages `{mark}`'s livelihood                         | mark: person who trades or works for a living                                                       | mark: failure       | sabotage  |
| `silence`       | `{agent}` pays `{witness}` to keep quiet                          | witness: person aware of an earlier step of this plot, else known to mastermind                     | witness: contact    | conceal   |

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
