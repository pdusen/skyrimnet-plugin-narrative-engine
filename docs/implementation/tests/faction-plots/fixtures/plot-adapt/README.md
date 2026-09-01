# Plot adaptation fixtures

Driven against the same two-person, two-object menu the birth fixtures use, with the objective fixed as
**acquire object 1 — a signed contract**:

| Menu    | 0                | 1                  |
| ------- | ---------------- | ------------------ |
| People  | Brynjolf         | Maven Black-Briar  |
| Objects | a ruby           | a signed contract  |

| Fixture                  | Must produce                                                                  |
| ------------------------ | ------------------------------------------------------------------------------ |
| `revise.txt`             | a revised tail of two steps                                                     |
| `concede.txt`            | a concession carrying its sentence                                              |
| `concede-no-reason.txt`  | a concession with a default sentence — the decision is the load-bearing part     |
| `ends-in-conceal.txt`    | **rejection** — the last step is the one that accomplishes the objective         |
| `decision-unknown.txt`   | rejection — `escalate` is neither `revise` nor `concede`                         |
| `revise-no-plan.txt`     | rejection — a revision with nothing to revise to                                 |

## The two fixtures that used to be here

`objective-changed.txt` and `objective-dropped.txt` pinned the rule this directory was originally built
around: a revised plan had to END on the objective, exactly as given, and the two ways a model could break
that were subtly different — the right verb aimed at the wrong thing, versus never getting there at all.

That rule is gone, along with the thing it protected. The objective used to be appended to the plan as a
final step, so a revision could overwrite it; it now lives on the Plot and is never in the plan at all. A
revision cannot rewrite the destination because the destination is not in front of it — which is a stronger
guarantee than the check, and the check had nothing left to test.

What survives is the same rule birth applies, and `ends-in-conceal.txt` pins it: the last step is the one
that accomplishes the objective, and covering your tracks accomplishes nothing.

`concede-no-reason.txt` is deliberately lenient in the other direction. The decision is what the simulation
acts on and the sentence is only what a reader sees, so a missing sentence gets a default rather than throwing
away a valid answer. A rejection becomes a concession at the call site, so the plot still ends cleanly; what
is refused is the revision, not the plot.
