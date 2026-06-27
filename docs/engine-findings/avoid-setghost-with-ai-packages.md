# Avoid `SetGhost(true)` on actors that need to run AI packages

## TL;DR

`Actor.SetGhost(true)` silently breaks AI package execution. The actor
will accept the package (no Papyrus error, `EvaluatePackage` returns
normally), then stand at its spawn point doing nothing. `PathToReference`
calls and `KeepOffsetFromActor` directives also fail to produce
movement on a Ghost actor.

For "ignored by other NPCs" requirements, use one of these instead:

- `SetActorValue("Aggression", 0)` (Unaggressive) — enough on its own
  when the actor only needs to not initiate combat with bystanders.
- Temporarily add the actor to `PlayerFaction` (or a custom non-hostile
  faction) on spawn, remove on release — for cases where Aggression
  alone isn't enough because external factions (e.g. Whiterun guards)
  are hostile to the actor's base faction.

Reserve `SetGhost` for "must not be damaged" use cases that don't also
require the actor to follow a package.

## What happened

During the bandit ambush approach implementation, we wanted spawned
bandits to be ignored by passing NPCs while they ran the approach
package (so e.g. a Whiterun guard wouldn't see them, aggro, and derail
the ambush before the bandits reached the player). `SetGhost(true)`
seemed to fit: ghosts are untargetable.

After flipping it on, the bandits:

- Spawned at their XMarker correctly.
- Got their Travel / approach package assigned correctly (verified via
  `sqv` and Papyrus `Debug.Trace`).
- ...stood at the spawn point indefinitely.

Removing the `SetGhost(true)` call, with no other change, restored
movement. Re-adding it broke movement again.

## Why (best guess)

The CK wiki does not document this — the wiki's coverage of `SetGhost`
focuses on damage immunity and projectile pass-through. The observable
behavior is that the AI scheduler appears to deprioritize or exclude
Ghost actors in ways that prevent package-driven movement from
executing. We did not dig into the engine internals to pin down the
exact mechanism; the rule "don't combine Ghost with movement packages"
is empirically reliable and was enough to unblock us.

## The replacement patterns

### Aggression = 0 (most cases)

```papyrus
spawnedActor.SetActorValue("Aggression", 0)
```

An Unaggressive actor will not initiate combat with bystanders. Sets
the "ignored *by* other actors" outcome the wrong direction (the actor
just doesn't aggro out), but for the ambush case that's what we
actually wanted — the bandits weren't supposed to be invisible, just
to not get into a fight before reaching the player.

### Friendly faction add / remove (when bystanders aggro the actor)

When external factions are hostile to the spawned actor's base faction
(Whiterun guards vs. BanditFaction, etc.) `Aggression = 0` isn't
enough — the guard initiates the combat, not the bandit. In that case:

```papyrus
spawnedActor.AddToFaction(PlayerFaction)
; ... approach phase ...
spawnedActor.RemoveFromFaction(PlayerFaction)
spawnedActor.StartCombat(playerRef)
```

Or use a dedicated non-hostile faction defined in the mod's ESP rather
than `PlayerFaction` directly, depending on what relationships you want
the actor to have during the protected phase.

## Related findings

- [`ai-package-flags-for-script-driven-combat-handoff.md`](ai-package-flags-for-script-driven-combat-handoff.md)
  — the other flag-tweak from the same debugging session: how to make
  sure the script's `StartCombat` call actually interrupts the approach
  package.
