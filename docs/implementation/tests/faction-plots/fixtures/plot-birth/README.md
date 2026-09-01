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
| `well-formed.txt`        | a plot; every target resolves to the menu entry it named                     |
| `target-out-of-range.txt` | rejection naming the step and the index                                     |
| `step-type-unknown.txt`  | rejection — `assassinate` is not in the manifest                             |
| `malformed-json.txt`     | rejection, and no partially built plot                                       |
| `markdown-fenced.txt`    | a plot — the fence *and the prose before it* are stripped                     |
| `dirty-text.txt`         | a plot whose ambition has lost its smart quotes, em-dash, en-dash, NBSP and zero-width space — and **kept** its accented Latin |
| `empty-plan.txt`         | rejection — a plan of no steps is not a plan                                  |
| `index-as-string.txt`    | a plot — `"1"` and `1` mean the same thing                                    |

`dirty-text.txt` is the one worth keeping honest, and it is the one that corrected a wrong assumption while
being written. It carries several classes from the substitution table at `docs/LLM_RESPONSE_HANDLING.md` at
once, because the sanitizer is applied at a single point and a fixture testing one class would pass just as
well if the others were dropped.

It also carries `café`, which must come out **unchanged**. The first version of the probe asserted the
ambition was pure ASCII afterwards; that is the opposite of this project's policy, which passes accented Latin
through on purpose so non-English players keep their language's letters — see *Why not romanize accented Latin
/ non-ASCII letters* in that document. The fixture now pins both halves: the typographic noise goes, the
letters stay.

`markdown-fenced.txt` deliberately puts a line of prose *before* the fence. `StripMarkdownFences` originally
required the response to begin with one, so a model that says "Here is the JSON you asked for:" first had its
whole answer discarded. That is fixed, and this fixture is what keeps it fixed.
