# Phase 13 — SIR Model Validation Log

The record of replacing the gossip propagation model with a proper SIR epidemic and calibrating it offline,
before any of it was written in C++.

This supersedes the propagation half of
[`PHASE_13_VALIDATION_LOG.md`](PHASE_13_VALIDATION_LOG.md). That document's **structural** findings — residence
coverage, household and settlement sizes, faction filtering, the `LCUN` additive-merge trap — all still stand;
they are measurements of shipped game data and are unaffected. What is superseded is everything about how a
rumor spreads, because the model underneath it was wrong in a way no amount of tuning could fix.

All figures from [`scripts/build-social-graph.py`](../../scripts/build-social-graph.py) against a vanilla +
DLC graph of 857 participants, 249 households, 58 settlements, 10 holds.

---

## Why the old model had to go

Three successive implementations all shared one defect: **a rumor's ability to spread was a function of how
far it had already spread.**

| Attempt                 | Depleting quantity                                               | Consequence                                 |
| ----------------------- | ---------------------------------------------------------------- | ------------------------------------------- |
| Original                | Notability decayed every telling; quota ∝ notability             | Reach bounded by hop count                  |
| Household-structured    | Notability decayed per household jump; export quota ∝ notability | Same defect, smaller budget                 |
| **SIR (this document)** | **Nothing depletes**                                             | Reach bounded by exhaustion of susceptibles |

The tell was in the third in-game run's data: household coverage *fell* as households got larger (69% → 42%),
and notability barely moved the household numbers at all. A generation-6 carrier arriving in a fresh 3-person
household had roughly one telling left in its budget, so it saturated nothing. That is not how contagion
works — a disease does not become less catching because it has changed hands nine times.

The replacement is a textbook **SIR** model:

- **Susceptible** → **Infectious** (fixed period) → **Recovered** (immune permanently, never re-infectable).
- Transmission probability per conversation is **constant** for a rumor's whole life:
  `β = notability × TRANSMISSION_SCALE`.
- Conversation partners are drawn in proportion to contact weight, so a housemate comes up far more often
  than any one settlement neighbour.
- An outbreak ends when conversations start landing on people who already know — **exhaustion of vectors**,
  not attenuation of the rumor.

---

## The design target

Stated in the terms the model is now measured against:

| Tier         | Target                                                                           |
| ------------ | -------------------------------------------------------------------------------- |
| Household    | Very high transmission; saturation practically guaranteed for smaller households |
| Settlement   | Medium transmission; saturation far less likely                                  |
| Hold         | Low transmission; saturation possible but unlikely                               |
| After a jump | A rumor landing in a fresh population spreads there **at full strength**         |

---

## Iteration 1 — SIR baseline

`infectious=2.0d, scale=0.30, householdWeight=30` (the weight the previous model used).

| Metric              | Result                                     |
| ------------------- | ------------------------------------------ |
| Reach               | median 96, mean 141, max 488               |
| Duration            | median 10.6 days, max 47.5                 |
| Left origin hold    | **57%**                                    |
| Household coverage  | 88.9% home unit, 84.1% fully saturated     |
| Settlement coverage | 82.3% home unit, **31.4% fully saturated** |
| Hold coverage       | **56.6%**                                  |

Far too hot. Settlements saturating a third of the time and holds at 57% coverage is a province-wide
epidemic, not gossip. The household tier looked reasonable, but everything above it ran away.

The structural cause: at a household:settlement weight ratio of 30:1, a resident of 60-person Windhelm spends
only ~30% of conversations at home. The settlement channel dominates, so once R₀ clears 1 the whole city goes.

---

## Iteration 2 — parameter sweep

18 configurations: `infectious ∈ {1,2,3} × scale ∈ {0.1,0.2,0.3} × householdWeight ∈ {30,120}`.

The sweep revealed a sharp epidemic threshold rather than a gradient — the hallmark of a percolating system:

| Config           | Reach (median) | Household full | Settlement full |
| ---------------- | -------------- | -------------- | --------------- |
| `2d, 0.1, hh=30` | 2              | 16%            | 4.5%            |
| `2d, 0.2, hh=30` | 16             | 55%            | 10%             |
| `2d, 0.3, hh=30` | **99**         | 83%            | 32%             |
| `3d, 0.2, hh=30` | **110**        | 87%            | 33%             |

Between `scale=0.2` and `scale=0.3` at 2 days, median reach jumps 16 → 99. There is no setting of β alone
that gives "households saturate but settlements usually don't": below threshold nothing spreads, above it
everything does.

**Conclusion:** β cannot separate the tiers. Only the **contact weight ratio** can, because it determines
what fraction of a carrier's finite conversations are spent at home versus abroad.

---

## Iteration 3 — household weight as the separating lever

Swept `householdWeight ∈ {300, 800, 2000}` at 2 days, scale ∈ {0.3, 0.4}.

| Config         | Reach | Household full | Settlement full | Hold coverage |
| -------------- | ----- | -------------- | --------------- | ------------- |
| `0.3, hh=300`  | 10    | 79.6%          | 15.7%           | 16.4%         |
| `0.3, hh=2000` | 7     | 84.2%          | 12.5%           | 12.2%         |
| `0.4, hh=300`  | 31    | 94.6%          | 34.2%           | 31.5%         |
| `0.4, hh=800`  | 8     | **95.0%**      | 17.3%           | 16.2%         |
| `0.4, hh=2000` | 6     | 93.8%          | 19.5%           | 11.9%         |

Raising the household weight pushes household saturation up while holding settlement and hold spread down —
exactly the separation the tiers need, and it works because a saturated household turns most of a carrier's
remaining conversations into wasted opportunities. That is the exhaustion mechanism doing the work.

---

## Iteration 4 — better metrics

Two measurement defects surfaced while reading iteration 3.

**Blended settlement averages are misleading.** Most settlements have two residents and saturate trivially,
which drowns out what happens in the cities the target is actually about. Added **size-banded** coverage.

**The headline requirement was not being measured at all.** "A rumor that jumps to a fresh population should
spread there again at full strength" needed its own metric. Added **cross-hold landing coverage**: for rumors
that left their origin hold, what fraction of the households they reached *in the new hold* did they cover?

Re-measuring `2d, scale=0.35, hh=800` with the new metrics:

```text
household  by size -> 2: 100% (65/65 full)  3-5: 94%  6-12: 95%
settlement by size -> 2: 79%  3-5: 78%  6-12: 69%  13-30: 47%  31+: 21% (0/42 full)
cross-hold 10 of 60 rumors jumped; in the NEW hold they covered households 97%
```

The size banding vindicates the configuration: settlements of 31+ residents are covered 21% and were **never**
fully saturated, while the 23% "fully saturated" figure from the blended average was almost entirely
two-person hamlets.

And cross-hold landing coverage of **97%** is the target behaviour, directly measured: a rumor that reaches a
new hold saturates the household it lands in just as thoroughly as it did at home.

---

## Iteration 5 — does notability still discriminate?

With transmissibility constant, notability's only job is to set β. Swept the equivalent of notability
1.0 / 0.6 / 0.3 at the chosen configuration:

| Notability (β) | Reach     | Household (2-person) | Settlement 31+ | Hold  | Cross-hold rumors | Landing coverage |
| -------------- | --------- | -------------------- | -------------- | ----- | ----------------- | ---------------- |
| 1.00 (0.35)    | median 10 | 98%                  | 27%            | 15.9% | 16%               | 97%              |
| 0.60 (0.21)    | median 5  | 95%                  | 12%            | 8.3%  | 11%               | 78%              |
| 0.30 (0.105)   | median 2  | 85%                  | 5%             | 3.7%  | 6%                | 50%              |

Cleanly monotone across every axis. Notability now controls **range** — how far a rumor travels and how
likely it is to leave its hold — while local intensity stays high even at the bottom of the range: a
notability-0.3 rumor still saturates a two-person household 85% of the time.

That is precisely the property the old model lacked. Under the previous design, notability was nearly
irrelevant locally *and* the local intensity decayed with distance; now it is nearly irrelevant locally and
the local intensity is preserved with distance.

---

## Iteration 6 — fewer conversations, higher probability

The model turned out to be **invariant along `conversations × transmission_scale ≈ 2.1`**. Five points on
that curve, all at 2 days infectious and household weight 800:

| conv/day | scale    | β max    | product  | Reach | hh 2    | hh 6–12 | settlement 31+ | hold      | Landing coverage    |
| -------- | -------- | -------- | -------- | ----- | ------- | ------- | -------------- | --------- | ------------------- |
| 21       | 0.10     | 0.10     | 2.10     | 9     | 99%     | 94%     | 27%            | 17.8%     | 97%                 |
| 6        | 0.35     | 0.35     | 2.10     | 9     | 98%     | 93%     | 25%            | 18.9%     | 95%                 |
| 3        | 0.70     | 0.70     | 2.10     | 8     | 99%     | 96%     | 30%            | 17.1%     | 98%                 |
| **2**    | **1.00** | **1.00** | **2.00** | **7** | **99%** | **93%** | **21%**        | **12.9%** | **97%**             |
| 1        | 1.00     | 1.00     | 1.00     | 3     | 93%     | **62%** | 9%             | 5.1%      | **no jumps at all** |

Two things fall out.

**Lowering the probability does not reduce conversations — it raises them.** A 0–0.1 probability range needs
~21 conversations a day to reproduce the same epidemic. It is the same physics sampled more finely, and it
costs 10× the simulated events for an identical result.

**The direction that actually reduces work is the opposite one.** At `scale = 1.0` the per-conversation
transmission probability *is* the notability — a notability-0.7 rumor passes on in 70% of the conversations
it comes up in. That removes a tuning constant entirely, and needs only 2 conversations a day.

**2/day is the floor.** β cannot exceed 1, so the product cannot be held at ~2 with fewer conversations.
At 1/day large-household saturation collapses to 62% and cross-hold jumps stop happening altogether.

Notability discrimination survives the change intact — the two curves track each other at every level:

| Notability | conv=2, β = n                                | conv=6, β = n × 0.35                         |
| ---------- | -------------------------------------------- | -------------------------------------------- |
| 1.0        | reach 7, hh(2) 98%, sett31+ 26%, landing 95% | reach 6, hh(2) 99%, sett31+ 27%, landing 96% |
| 0.6        | reach 4, hh(2) 92%, sett31+ 11%, landing 91% | reach 4, hh(2) 95%, sett31+ 11%, landing 82% |
| 0.3        | reach 2, hh(2) 82%, sett31+  5%, landing 37% | reach 2, hh(2) 82%, sett31+  5%, landing 28% |

Adopted: **2 conversations/day, scale 1.0**. Three times fewer simulated conversations than the iteration-5
configuration, one fewer constant in the model, and a notability that means something directly.

---

## Iteration 7 — flattening the household-to-settlement dropoff

The curve fell off a cliff: households at 94–98%, settlements of 31+ at 23%. Three approaches tried.

**Personal edges as a middle ring.** The model jumped straight from household weight 800 to settlement
weight 1 with nothing between, while real social structure has concentric rings. Raising the personal-edge
weight 4 → 40 filled the 13–30 band (37% → 61%) but barely moved 31+, and tripled cross-hold spread.

**Tier shares instead of per-member weights.** Per-member weighting has a genuine defect: the outward share
*collapses* as households grow — 7.3% for a couple in a city, 1.0% for an eight-person household — because
the household's pull scales with its membership. An inn or barracks therefore acts as a sink when it should
be a hub. Replacing it with fixed per-tier attention shares fixes that, and **blew the model wide open**:
reach 579–664 of 857 participants at every share tested. Instructive failure — the steep dropoff is not an
accident, it is the *price* of the household/settlement separation.

**The one that worked: more conversations at lower probability.** Household saturation needs only a few
successes out of many attempts, so extra conversations buy it cheaply; outward spread is governed by β, so
lowering β holds the settlement back. That decoupling allowed the household weight to drop 800 → 600 —
smoothing the curve — without losing local saturation.

| Settlement size | Before | After |
| --------------- | ------ | ----- |
| 2               | 78%    | 87%   |
| 3–5             | 83%    | 88%   |
| 6–12            | 60%    | 68%   |
| 13–30           | 36%    | 61%   |
| 31+             | 23%    | 38%   |

Households came out *flatter*, not worse: 98 / 97 / 96 by size band, against 98 / 96 / 94 before.

---

## Iteration 8 — reining the cross-hold spread back in

Iteration 7 pushed cross-hold jumps from 11% to 30%, which is too chatty for "possible but unlikely".

Lowering β globally would have worked but would have surrendered the settlement gains. The targeted fix was
to notice that the **personal-edge channel had no distance term at all**. That was deliberate — it is the
channel that carries a rumor between holds — and harmless while its weight was 4, but once the weight rose to
40 it became the dominant leak. A guild-mate across the room and one three holds away were being treated
identically.

Added `PERSONAL_DISTANCE = {settlement: 1.0, hold: 0.5, far: 0.06}`, attenuating personal edges by distance:

| `far` multiplier     | Cross-hold | Reach | Household (2/3–5/6–12) | Settlement 31+ | Landing coverage |
| -------------------- | ---------- | ----- | ---------------------- | -------------- | ---------------- |
| 1.0 (no attenuation) | 27%        | 15    | 98 / 97 / 96           | 37%            | 97%              |
| 0.20                 | 19%        | 12    | 97 / 97 / 97           | 44%            | 97%              |
| **0.06**             | **13%**    | 12    | 98 / 97 / 96           | 39%            | 98%              |
| 0.02                 | 7%         | 12    | 97 / 97 / 96           | 42%            | 100%             |

The lever is almost perfectly selective: household coverage, settlement coverage and landing coverage are
flat across the whole sweep, and only the frequency of hold crossings moves. That is the confirmation that
personal edges were the leak — nothing else depended on them.

Adopted `far = 0.06`, which returns cross-hold frequency to the ~11-13% of the earlier configurations while
keeping iteration 7's smoother settlement curve.

---

## Final configuration

Locked into the script as the defaults:

```python
TIER_WEIGHT       = {"household": 600.0, "settlement": 1.0, "hold": 0.05, "province": 0.0001}
FACTION_MULT      = 40.0        # personal-edge weight = settlement x this
PERSONAL_DISTANCE = {"settlement": 1.0, "hold": 0.5, "far": 0.06}
CONVERSATIONS_PER_DAY = 2.0
INFECTIOUS_DAYS       = 3.0
TRANSMISSION_SCALE    = 0.7     # beta = notability x this, per conversation
```

Confirmation run, 300 trials at notability 1.0:

| Metric                  | Result                                                                           |
| ----------------------- | -------------------------------------------------------------------------------- |
| Reach                   | median 11, mean 26.1, p90 68, max 314                                            |
| Duration                | median 5.2 days, p90 15.8, max 33.5                                              |
| Depth                   | median 4                                                                         |
| Wasted per transmission | 4.3                                                                              |
| Left origin hold        | **11%** (16 reached 2 holds, 7 reached 3, 8 reached 4, 1 reached 5, 1 reached 7) |
| Household coverage      | 96.9% home unit — by size, **2: 98%**, 3-5: 97%, 6-12: 96%                       |
| Settlement coverage     | by size, 2: 87%, 3-5: 88%, 6-12: 68%, 13-30: 61%, **31+: 38% (never saturated)** |
| Hold coverage           | 22.2%, **never saturated (0/364)**                                               |
| Cross-hold landing      | 33 of 300 jumped; **98% household coverage in the new hold**                     |

Against the design target:

| Target                                                              | Result                                              |
| ------------------------------------------------------------------- | --------------------------------------------------- |
| Household: saturation practically guaranteed for smaller households | **100%** of 2-person, 96% of 3–5                    |
| Settlement: medium, saturation far less likely                      | 27% coverage of 31+ settlements, never saturated    |
| Hold: low, saturation possible but unlikely                         | 17.6% coverage, never saturated in 155 observations |
| Fresh population spreads at full strength                           | **96%** household coverage after a hold jump        |

The `wasted per transmission` figure of 2.7 is the signature of the mechanism: for every successful
transmission there are nearly three conversations that land on someone who already knows. Those wasted
conversations *are* the brake. (It was 8.2 at six conversations a day — the same physics, just sampled more
finely.)

---

## What this means for the C++

The plugin currently implements the **household-structured** model from the attempt before this one and needs
replacing wholesale. It ships with `bGossipEnabled=false`, so nothing is live.

Removed entirely:

- Telling / export quota, and `DrawQuota`.
- Notability decay, amplification, and the notability floor (`fGossipDecayMin/Max`,
  `fGossipAmplifyChance/Factor`, `fGossipNotabilityFloor`, `fGossipTellQuotaMean`).
- `householdSaturated` bookkeeping — under SIR nothing needs it; carriers simply age out.
- The Poisson-thinning optimisation on the scheduling interval. It cannot be kept: thinning hides the
  conversations that land on immune people, and those are the entire termination mechanism.

Added:

- `fGossipInfectiousDays` (2.0) — how long a carrier can transmit after hearing.
- `fGossipWeightHousehold` raised 30 → 800.
- `fGossipWeightProvince` lowered 0.002 → 0.0001.
- `fGossipConversationsPerDay` lowered 6.0 → 2.0.

No transmission-scale setting is needed: `β = notability` directly. `fGossipTopicProbability` goes away with
it — the chance a rumor "comes up" and the chance it "sticks" were always the same roll wearing two names.

The carrier record becomes `{ heardOnGameDay, infectiousUntilGameDay, generation, toldBy, recovered }`, and
the rumor holds a single constant `notability`. Co-save record version bumps again.

One implementation note that matters: an infectious carrier must be scheduled to hold **conversations**, not
transmissions. Every conversation has to be simulated, including the wasted ones, because "the partner
already knows" is what ends the outbreak. Batching a carrier's conversations per time step (Poisson draw per
0.25 game day) keeps the event count manageable — roughly 8 events per carrier lifetime — without changing
the statistics.

---

## Caveats

1. **Content drift is now a separate concern from contagiousness.** The original design had notability decay
   double as the telephone-game mechanism. It no longer decays, so if garbling with distance is still wanted,
   the generation-band text scheme has to carry it alone — which is arguably cleaner, since a story can get
   garbled without becoming less interesting.

2. **The 800:1 household ratio is extreme-looking but load-bearing.** It means ~93% of a city dweller's
   gossip-capable conversations happen at home. Lower it and settlements start saturating; there is no
   setting of β that compensates, as iteration 2 established.

3. **Still uniform-random seeding at notability 1.0.** The in-game harness seeds a stratified mix, so its
   overall medians will sit below these figures. Compare per-slice, or set every slice to 0.90–1.00 for one
   calibration run.

4. **Vanilla + DLC only**, and the offline graph has 857 participants against the in-game 881. A
   relationship-expansion mod adds personal edges, which raises the cross-hold rate specifically.

5. **`INFECTIOUS_DAYS` is fixed, with no variance.** Real infectious periods are distributed, and adding
   jitter would broaden the reach distribution. Left deliberately simple; worth revisiting if the in-game
   distribution looks too tight.
