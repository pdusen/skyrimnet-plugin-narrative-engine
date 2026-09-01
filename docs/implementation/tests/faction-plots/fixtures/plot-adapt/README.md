# Plot adaptation fixtures

Driven against the same two-person, two-object menu the birth fixtures use, with the objective fixed as
**acquire object 1 — a signed contract**:

| Menu    | 0                | 1                  |
| ------- | ---------------- | ------------------ |
| People  | Brynjolf         | Maven Black-Briar  |
| Objects | a ruby           | a signed contract  |

| Fixture                  | Must produce                                                                  |
| ------------------------ | ------------------------------------------------------------------------------ |
| `revise.txt`             | a revised tail of two steps, ending on the objective                            |
| `concede.txt`            | a concession carrying its sentence                                              |
| `concede-no-reason.txt`  | a concession with a default sentence — the decision is the load-bearing part     |
| `objective-changed.txt`  | **rejection** — ends on `acquire 0`, a different object                          |
| `objective-dropped.txt`  | **rejection** — ends on a `discredit` and never reaches the objective at all     |
| `decision-unknown.txt`   | rejection — `escalate` is neither `revise` nor `concede`                         |
| `revise-no-plan.txt`     | rejection — a revision with nothing to revise to                                 |

The two objective fixtures are the point of this directory. Adaptation rewrites the path, never the
destination, and the two ways a model can break that are subtly different: `objective-changed.txt` ends on the
right *verb* aimed at the wrong *thing*, which is the one that would slip through a check that only compared
step types. `objective-dropped.txt` never gets there at all.

Both are **rejected rather than corrected**. Silently appending the objective to a plan that omitted it would
produce a runnable plot and hide the fact that the model did not understand the task — and a model that
changed the destination should not have the rest of its answer trusted either. The rejection becomes a
concession at the call site, so the plot still ends cleanly; what is refused is the revision, not the plot.

`concede-no-reason.txt` is deliberately lenient in the other direction. The decision is what the simulation
acts on and the sentence is only what a reader sees, so a missing sentence gets a default rather than throwing
away a valid answer.
