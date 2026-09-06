# Commit messages: a summary line, then one bullet per unit of change

Applies to every commit made in this repository.

## Subject line

One summary of everything in the commit, **80 characters or less**, in the conventional-commit form
the history already uses (`fix(plots):`, `feat(dashboard):`, `docs:`, `chore:`).

It has to cover the whole commit rather than its largest part. If no 80-character sentence honestly
does, that is a sign the commit is two commits.

## Body

A bulleted list and nothing else: no prose paragraphs, no headings, no sub-bullets, no closing
notes. Blank line after the subject, blank line before the attribution trailer.

**Each bullet is one line of 140 characters or less** — never wrapped onto a continuation line — and
describes **one indivisible unit of changes**.

Indivisible means the changes stand or fall together. A single bullet may cover hundreds of lines
across dozens of files, as long as describing any part of it on its own would describe something that
doesn't work or doesn't make sense alone: a parser field and the prompt that produces it, a rename
and every call site, a new rejection and the census counter that reports it. The converse holds too
— two changes that could have been committed separately and each still made sense are two bullets,
even when they sit in the same function.

Test every bullet in both directions:

- Could it be split, with each half still describing a coherent change? Then split it.
- Could it be merged with a neighbour without either one losing what it is for? Then merge them.

A bullet that won't fit in 140 characters is usually not too long — it is two units, or it is
explaining rather than describing. Split it, or cut the explanation.

## What a bullet says

**What changed and why, not which files were touched.** `git show` already lists the files; the
bullet exists for what the diff can't tell the reader — the reason, the consequence, the number that
decided it. Name a symbol or a value when that is the shortest way to be precise, not as a
substitute for being precise.

Order the bullets by weight: the change the subject line is about comes first, incidental fixes
found along the way come last.

## Example

```text
fix(plots): keep the player and the dead out of the simulation

- Both plot prompts forbid the player, and PlotStepParse::NamesThePlayer rejects any field naming the Dragonborn on either parse path.
- The ambition prompt refuses schemes that restage a quest the game already tells, which duplicates or contradicts what the player saw.
- Actors whose display name ends in the standalone word "Ghost" join the IsGhost rejection, catching apparitions that carry no flag.

Co-Authored-By: ...
```
