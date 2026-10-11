# Phase 17 — Milestone 2: Rigid Steps

Milestone 1 (the original [`PHASE_17_FACTION_PLOTS.md`](PHASE_17_FACTION_PLOTS.md), Steps 1–17) built a plot
simulation whose masterminds, ambitions and schemes are good and whose steps are not. This milestone replaces how
a step is chosen, described and remembered. The mastermind draw, the ambition and scheme prose, the casting
ladder, the progress race and the co-save all stay.

> **Doc status: planned, not built.** No step is checked off. Steps 18 and 19 of the Phase 17 doc are superseded
> by this milestone; its Steps 20–22 still apply and follow it.

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
| `denounce`      | `{agent}` denounces `{mark}` to `{authority}`                     | mark: person; authority: person who leads or is senior in a court                                   | authority: contact  | discredit |
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

- [ ] Complete

**[CLAUDE]** builds a dashboard debug button that sends one fixed probe template and logs the raw result.
**[USER]** presses it with a decision provider configured and records the results here.

The probe settles what the design above assumes and has not verified:

1. Criteria lines generated by an Inja loop over the context render as options.
2. Keys of the form `c1`…`c20` are accepted and returned verbatim.
3. Twenty options in one question behave as well as five.
4. Several questions in one template are all answered.
5. Latency per request, which decides whether planning a five-step plot is affordable.

If any of these fails, the plan is revised here before Step 3.

**The `[CLAUDE]` half is done; the `[USER]` run is outstanding.** The Plots tab has a **Probe decisions** button.
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

- [ ] Complete

**[CLAUDE]**

The rule vocabulary (known to mastermind, trades for a living, senior in a court, rostered standing, settlement,
item pool category, bound) and a builder that turns a rule into an ordered, capped candidate list.
Children leave the plot population entirely, which closes the gap for masterminds as well as slots.

**Verification:** an engine-mock suite over a fabricated province asserts each rule's admits and refusals, that a
child is never a candidate for anything, that bound slots ask nothing, and that ordering is stable
run to run.

### Step 5 — Planning by decision

- [ ] Complete

**[CLAUDE]**

The verb-then-slots loop, its decision templates under `prompts/decisions/narrative_engine/`, and the removal of
the step prompt and its parser. The ambition prompt is changed to return the scheme as an imperative clause, and
its parser rejects a scheme that does not open with a verb, because every dispatch memory reads "as part of my
plan to `{scheme}`".

**Verification:** a suite drives the loop against scripted decision answers and asserts a plan is built only from
offered keys, preconditions gate `deliver` and `plant`, `done` ends the plan, the position cap holds, and a
decision error aborts the birth without spending the slot.

### Step 6 — Dispatch by decision

- [ ] Complete

**[CLAUDE]**

Agent selection over the ladder's eligible set, bound agents, the own-plot cooldown exemption, and the removal of
free-text target and agent resolution.

**Verification:** a suite asserts a single eligible candidate asks nothing, a bound agent is taken as bound, a
plot's previous agent is eligible again on its next step, and a decision error falls back to the ladder's own
order rather than stalling the plot.

### Step 7 — Adaptation by decision

- [ ] Complete

**[CLAUDE]**

`retry` / `substitute` / `concede`, the cap, and the removal of the adapt prompt and its parser.

**Verification:** a suite asserts `retry` excludes the failed agent, `substitute` replaces only the failed
position onward, `concede` ends the plot, a decision error concedes, and the cap ends a plot that never concedes.

### Step 8 — Memories

- [ ] Complete

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

### Step 9 — In-game validation

- [ ] Complete

**[USER]**

Replaces Phase 17 Step 18. With a decision provider configured, run a session long enough for plots to finish.

1. Do the verbs chosen make sense for the scheme, and the people chosen make sense for the verbs?
2. Do leaders delegate to plausible subordinates?
3. Talk to a mastermind and an agent mid-step and after it. Do they know what they did, for whom, and how it went?
4. Find a mastermind whose agent failed. Do they know who failed them, and at what?
5. Record the decision-request rate per in-world day.

---

## Open questions

1. **Does the embedding model stay?** Candidate lists are cut by relevance order today, and the decision model
   reads them. If ordering by relevance alone gives good lists, `CharacterIndex` and the ONNX runtime have no
   remaining caller and should go. Settle at Step 4 by comparing the lists it produces with and without them.
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
