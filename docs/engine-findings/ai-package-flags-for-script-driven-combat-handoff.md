# AI package flags for script-driven combat handoff

## TL;DR

When a Papyrus alias script needs to flip an actor from a package-driven
"approach the target" phase into vanilla combat at a chosen moment (the
classic ambush / spawn-and-engage pattern), the approach package must
let combat preempt it. On the package's flags tab:

- **Ignore Combat: FALSE (unchecked).**
- **Interrupt Override: None.**

With either of those set the "wrong" way, the script's `StartCombat`
call gets visibly accepted but the actor keeps running the package —
walking to the destination, stopping, standing there for 5-10 seconds,
then *finally* swinging. Looks like the script broke; actually it's the
package refusing to yield.

## The intuition that misled us

We had originally set these flags to "protect" the approach phase from
being derailed by guards / civilians the actor might encounter en route
— the worry being that a stray combat interaction would abort the
package before the ambush staged.

That protection isn't what those flags do. They don't say "ignore
*incoming* combat from third parties"; they say "ignore the engine's
combat interruption *of this package*, period" — including the
combat the script itself triggers.

For en-route protection from civilian interference, use
`SetActorValue("Aggression", 0)` (Unaggressive). An Unaggressive actor
won't initiate combat with bystanders, which is the actual safety net
we wanted. See also
[`avoid-setghost-with-ai-packages.md`](avoid-setghost-with-ai-packages.md)
for the related "ignored by NPCs" pattern that does NOT involve Ghost.

## Symptoms when set wrong

- Actor reaches the approach destination and just *stands there*.
- The script's `StartCombat(akTarget)` call logs successfully (no
  Papyrus error).
- `sqv` on the parent quest looks healthy — alias filled, stage
  advanced, script running.
- 5–10 seconds later combat AI finally kicks in and the actor swings.

If you see that timing gap, the package flags are almost certainly the
cause.

## The working setup, in summary

For the bandit ambush approach package
(`_ne_BanditAmbushQuest_ApproachPackage`-equivalent):

| Field | Value | Why |
| --- | --- | --- |
| Ignore Combat | unchecked | Lets the script's `StartCombat` actually preempt. |
| Interrupt Override | None | Same — `Combat` here had the same dominating effect. |
| Aggression (on the actor, via `SetActorValue`) | 0 (Unaggressive) | Replaces the en-route "don't fight civilians" protection. |

Tested against the `_ne_BanditAmbushQuest_SpawnedBandit` alias script's
approach → release flow.
