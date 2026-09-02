# Plot composition fixtures

One directory per LLM call, one file per way a response can arrive. Committed rather than fabricated in a
probe so the cases are readable, editable, and stay put when the probe that reads them is deleted.

Composition runs as three calls — **who**, **why and what**, **how** — because it ran as one and produced the
same plot every time. See `include/PlotBirth.h` for that history; what matters here is that each call has its
own parser, its own failure modes, and its own directory.

Every one of these is a shape a model actually produces. The markdown fence and the string-typed index are
not hypothetical failures invented to pad a test matrix — gossip lost a whole seed to a fence before
`StripMarkdownFences` existed, and a model that has been asked for a number will hand back `"1"` often enough
that rejecting it would mean throwing away a good answer over a formatting detail.

## `plot-cast/` — which of the shortlist is scheming

Driven against a five-person shortlist. The FormIDs are real ones, looked up in the Spriggit export rather
than recalled, because the fixtures name the people and a reader will take them for real:

| Form id     | Who               |
| ----------- | ----------------- |
| `0x1B07D`   | Brynjolf          |
| `0x1336A`   | Maven Black-Briar |
| `0x13349`   | Anuriel           |
| `0x371D6`   | Maul              |
| `0x13380`   | Vekel             |

| Fixture                   | Must produce                                                                  |
| ------------------------- | ------------------------------------------------------------------------------ |
| `well-formed.txt`         | Anuriel                                                                         |
| `bare-hex.txt`            | Brynjolf — `"1B07D"` without the `0x` still resolves                             |
| `padded.txt`              | Maven — surrounding whitespace is trimmed                                       |
| `number-not-string.txt`   | Maul — a bare JSON number is an answer too                                      |
| `markdown-fenced.txt`     | Brynjolf — the fence *and the prose before it* are stripped                     |
| `not-a-candidate.txt`     | **rejection** — Nazeem's real form id, and not on this list                     |
| `name-not-form-id.txt`    | **rejection** — "Maven Black-Briar" is the display name, not a form id          |
| `no-choice.txt`           | rejection — a `because` with no `choice` decides nothing                        |

**The model answers with a `form_id`, not a position.** That is the pattern
`narrative_engine_action_select` already uses for choosing a letter or visit sender, and it is the better one
here for a specific reason: an off-by-one in an index silently hands the scheme to the wrong person and
nothing downstream ever looks incorrect, whereas a form id either is one of the five it was given or it is
not. `not-a-candidate.txt` is the case that matters — a real NPC the model knows and we did not offer — and it
is refused rather than resolved to a nearest match; see `docs/prior-art/NAME_RESOLUTION_FAILURE_MODES.md` for
what fuzzy resolution cost the project this one learned from.

`bare-hex.txt`, `padded.txt` and `number-not-string.txt` are the deliberate leniencies, and they are safe
*because* the membership test follows: a mis-read cannot select the wrong candidate, only fail to select any.
One of them hides a trap worth knowing about. `std::stoul` will happily read the `13` out of `13BB3` and
stop, so a bare hex id would silently become the decimal 13 — the parser requires the whole string to be
consumed, and the probe pins that against a shortlist deliberately containing both `0xD` and `0x13BB3`.

The five candidates the prompt sees are not simply the five strongest draws. The plugin draws twenty-five,
keeps the first five SkyrimNet can resolve a profile for, and shuffles them. Both halves matter: an
unresolvable candidate renders as a name and a hold beside others with a paragraph each — which is a choice
between the one the model was told about and four it was not, and the first run under this pipeline picked
the described candidate three times out of four — and the shuffle is there so that weight decides who is
*on* the list without also deciding where they sit in it.

## `plot-ambition/` — the lifetime thing, and this scheme

| Fixture                        | Must produce                                                     |
| ------------------------------ | ----------------------------------------------------------------- |
| `well-formed.txt`              | both sentences                                                    |
| `dirty-text.txt`               | both sentences, sanitized (see below)                             |
| `scheme-restates-ambition.txt` | **rejection** — a scheme is a step toward the agenda, not a copy  |
| `no-scheme.txt`                | rejection — an ambition alone is a motive with nothing under it   |
| `malformed-json.txt`           | rejection, and nothing partially built                            |

`scheme-restates-ambition.txt` pins the failure the three-call split exists to prevent. When one call was
asked for both, it routinely answered the second with a paraphrase of the first — an "objective" that was
really just the motive again, with nothing pulling it toward being a step *toward* anything. The check is a
literal string comparison rather than a similarity measure, deliberately: a real one would need a threshold
nobody has measured, and this catches the case that actually happens.

## `plot-steps/` — how it gets done

| Fixture                 | Must produce                                                                  |
| ----------------------- | ------------------------------------------------------------------------------ |
| `well-formed.txt`       | three steps, each with its sentence                                             |
| `markdown-fenced.txt`   | two steps — the fence and the preamble are stripped                             |
| `ends-in-conceal.txt`   | **rejection** — the last step accomplishes the scheme, and concealment does not |
| `step-type-unknown.txt` | rejection — `assassinate` is not in the manifest                                |
| `no-description.txt`    | rejection — a step type with no sentence is a blank node on the dashboard       |
| `single-step.txt`       | rejection — one step is an errand, not a scheme                                 |
| `over-the-cap.txt`      | rejection — eleven steps; the ceiling is ten                                   |

`no-description.txt` matters more than it looks. Composition is currently handed no menu of people, objects
or factions, so a step is a type and a sentence and nothing else; without the sentence the whole plan renders
as *Watch / Recruit / Acquire* and every plot looks like every other one, which is the failure this pipeline
was rebuilt to fix.

`ends-in-conceal.txt` has outlived two different justifications. It was added when the objective was appended
to the plan as a final step, so a trailing `conceal` covered the tracks of something that had not happened
yet. The append is gone; the rule survives on its own terms.

A seventh step and a trailing `conceal` were the two rejections a real run actually produced, out of four
births. Both cost all three calls, and both were the kind of mistake a model fixes immediately when told —
which is why each call is now asked twice at most, with the rejection reason handed back.

**Neither bound is stated in the prompt, and both have been.** A range anchors hardest: offering "2 to 6"
produced 4, 4, 4, the midpoint every time. A bare ceiling anchors too — naming six produced 6, 6, 4 on the
next run. The prompt now says only *take as many steps as this scheme needs*, and the bounds here exist to
catch an answer that has gone wrong rather than to shape one that has not. The ceiling is correspondingly
generous at ten: not a target, just the point past which a plan has stopped being a scheme and become a list.
Adaptation's ceiling tracks it, since a revision that cannot be as long as the plan it replaces is a strange
rule.

## The sanitizer case

`plot-ambition/dirty-text.txt` is the one worth keeping honest, and it corrected a wrong assumption while
being written. It carries several classes from the substitution table at `docs/LLM_RESPONSE_HANDLING.md` at
once, because the sanitizer is applied at a single point and a fixture testing one class would pass just as
well if the others were dropped.

It also carries `café`, which must come out **unchanged**. The first version of the probe asserted the
ambition was pure ASCII afterwards; that is the opposite of this project's policy, which passes accented
Latin through on purpose so non-English players keep their language's letters — see *Why not romanize
accented Latin / non-ASCII letters* in that document.

These strings travel further than most. An ambition written at call 2 is rendered into call 3's prompt, and
again into every adaptation prompt for the life of the plot, so a zero-width character here rides through the
whole chain.
