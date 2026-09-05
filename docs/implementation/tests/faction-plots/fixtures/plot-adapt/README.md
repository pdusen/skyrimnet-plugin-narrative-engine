# Plot adaptation fixtures

Adaptation is a fourth call with the same shape as the third: no menu of people or objects, and a step
returned as a type, a sentence, and a description of who carries it out. Its parser is `PlotAdapt::Parse`,
which shares `PlotStepParse::ReadRoles` with birth so the two cannot disagree about what a step carries.

| Fixture                  | Must produce                                                                  |
| ------------------------ | ------------------------------------------------------------------------------ |
| `revise.txt`             | a revised tail of two steps                                                     |
| `concede.txt`            | a concession carrying its sentence                                              |
| `concede-no-reason.txt`  | a concession with a default sentence — the decision is the load-bearing part     |
| `ends-in-conceal.txt`    | **rejection** — the last step is the one that accomplishes the scheme            |
| `decision-unknown.txt`   | rejection — `escalate` is neither `revise` nor `concede`                         |
| `revise-no-plan.txt`     | rejection — a revision with nothing to revise to                                 |
| `revised-step-no-agent.txt` | rejection — a revised step that describes nobody to carry it out              |

## The two fixtures that used to be here

`objective-changed.txt` and `objective-dropped.txt` pinned the rule this directory was originally built
around: a revised plan had to END on the objective, exactly as given, and the two ways a model could break
that were subtly different — the right verb aimed at the wrong thing, versus never getting there at all.

That rule is gone, along with the thing it protected. The objective used to be a step appended to the plan,
so a revision could overwrite it. It is now a sentence on the Plot — the scheme — and a revision has no way
to reach it. Structural, rather than checked, which is a stronger guarantee and one fewer rule to get wrong.

## Why conceding is lenient and revising is not

`concede-no-reason.txt` is deliberately forgiving: the decision is what the simulation acts on and the
sentence is only what a reader sees, so a missing sentence gets a default rather than throwing away a valid
answer.

A rejected *revision* is treated as a concession at the call site, so the plot still ends cleanly. What is
refused is the revision, not the plot — the alternative is a scheme that stalls on a failed step and never
resolves, which is the worst of the three outcomes for a reader.
