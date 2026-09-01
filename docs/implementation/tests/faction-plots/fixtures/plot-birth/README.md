# Plot birth fixtures

One file per way an LLM response can arrive, committed rather than fabricated in the probe so that the cases
are readable, editable, and stay put when the probe that reads them is deleted.

Every one of these is a shape a model actually produces. The markdown fence and the string-typed index are not
hypothetical failures invented to pad a test matrix — gossip lost a whole seed to a fence before
`StripMarkdownFences` existed, and a model that has been asked for a number will hand back `"1"` often enough
that rejecting it would mean throwing away good plans over a formatting detail.

They are driven against a fixed two-person, two-object menu, so an index in a fixture always means the same
thing:

| Menu    | 0                | 1                  |
| ------- | ---------------- | ------------------ |
| People  | Brynjolf         | Maven Black-Briar  |
| Objects | a ruby           | a signed contract  |

| Fixture                   | Must produce                                                                |
| ------------------------- | --------------------------------------------------------------------------- |
| `well-formed.txt`         | a plot; the objective and every plan target resolve to what they named        |
| `target-out-of-range.txt` | rejection naming the step and the index                                      |
| `step-type-unknown.txt`   | rejection — `assassinate` is not in the manifest                              |
| `malformed-json.txt`      | rejection, and no partially built plot                                        |
| `markdown-fenced.txt`     | a plot — the fence *and the prose before it* are stripped                     |
| `dirty-text.txt`          | a plot whose ambition has lost its smart quotes, em-dash, en-dash, NBSP and zero-width space — and **kept** its accented Latin |
| `empty-plan.txt`          | rejection — a scheme with no groundwork at all reads as a wish                |
| `index-as-string.txt`     | a plot — `"1"` and `1` mean the same thing                                    |
| `objective-is-conceal.txt` | **rejection** — nobody schemes in order to have covered their tracks         |
| `no-objective.txt`        | rejection — the objective is chosen, never inferred                           |
| `plan-ends-in-conceal.txt` | **rejection** — the objective is appended after the plan, so a trailing `conceal` would hide something that has not happened yet |

## The two that exist because of a real failure

`objective-is-conceal.txt` and `no-objective.txt` were added after the first in-game run produced three plots
in a row titled *"Cover Tracks Sybille Stentor"*, *"Cover Tracks Uaile"* and *"Cover Tracks
Scouts-Many-Marshes"*. The objective was being inferred from whichever step happened to be last, `conceal` is
a natural closing step, and so concealment kept becoming the thing the scheme was *for*.

The objective is now its own field, chosen before the route rather than falling out of it, and `conceal` is
refused as an objective outright. Both fixtures pin that: one supplies a `conceal` objective, the other omits
the objective entirely to confirm it is never quietly reconstructed from the plan.

`plan-ends-in-conceal.txt` came out of the run after that one, which produced four plots reading *Watch,
Suborn, Cover Tracks, Acquire* — concealment sitting one place before the thing it was meant to hide. The
model had written `plan` as the complete arc, not knowing the objective is appended after it. The prompt no
longer invites that (it used to say `conceal` "can end a route", and `plan` is not the route), and a plan
whose last entry is `conceal` is now refused rather than shuffled or silently trimmed: there is no other
position to move it to, because the objective is terminal by construction.

`well-formed.txt` also carries the ambition in its corrected sense — the **agenda**, the larger thing the
scheme ostensibly serves and which no scheme can finish. *"She means to own everyone in the Rift who matters"*
is not completed by acquiring one contract, and that is the test: if the sentence would be finished by
completing the objective, it is the wrong sentence.

## The sanitizer case

`dirty-text.txt` is the one worth keeping honest, and it corrected a wrong assumption while being written. It
carries several classes from the substitution table at `docs/LLM_RESPONSE_HANDLING.md` at once, because the
sanitizer is applied at a single point and a fixture testing one class would pass just as well if the others
were dropped.

It also carries `café`, which must come out **unchanged**. The first version of the probe asserted the
ambition was pure ASCII afterwards; that is the opposite of this project's policy, which passes accented Latin
through on purpose so non-English players keep their language's letters — see *Why not romanize accented Latin
/ non-ASCII letters* in that document.

`markdown-fenced.txt` deliberately puts a line of prose *before* the fence. `StripMarkdownFences` originally
required the response to begin with one, so a model that says "Here is the JSON you asked for:" first had its
whole answer discarded.
