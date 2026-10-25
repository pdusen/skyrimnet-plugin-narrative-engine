# Phase 18 — Internationalization

NarrativeEngine plays in the language Skyrim is running in. It detects the game's language at startup, renders
every string a player can see from a plain-text language file, and asks the LLM to write player-facing content in
that language. Nothing about which language is active changes how the simulations decide what happens.

> **Doc status: planned, not started.** The design is settled and the implementation plan is written: Steps 1–30.
> Twenty-eight are `[CLAUDE]`; the two `[USER]` steps come last and each is one button press. The questions the first
> draft left open have all been answered by investigation; what each one found is recorded under **Resolved
> questions**, and the design already reflects them.

---

## Why this phase exists

The README currently tells players not to edit our prompts to get another language, and that the dashboard and
some event text are English-only. Both are true, and the first is the bigger problem. The plot, gossip and beat
simulations read the model's output back and act on it: they match names against menus, check that a scheme opens
with a verb, refuse text naming the Dragonborn, and slot LLM phrases into English sentence templates. A player who
translates the prompts gets German text spliced into English templates, validators that reject correct German, and
no way to tell which is happening.

Doing this properly takes two kinds of work, and the cleanup has to come first:

1. **Cleanup.** Every sentence C++ composes for an LLM moves into a prompt file, and every sentence C++ composes for
   a player or for persistence moves into a language file. This is worth doing even in English: prompt wording
   becomes editable without a rebuild, and text that is stored becomes text a translator can reach.
2. **Features.** Detect the language, load the matching file, render through it, and add an output-language line to
   the prompts whose output a player reads.

---

## Scope

### In scope

- Detecting the game's language through CommonLibSSE-NG, plus a settings override.
- One UTF-8 plain-text language file per language. English is the reference and the fallback for any missing key.
- A template formatter with placeholders, gender select, plural select and list joining.
- Moving every C++-composed sentence out of C++. LLM-bound prose goes to prompt files, and player-visible or
  persisted prose goes to language files.
- An output-language line in every prompt that produces content.
- Making the validators that read content language-aware: word lists move to the language file, and byte-level
  character tests are replaced with UTF-8-aware ones.
- Every player-visible surface: SkyrimNet memories and events, letter titles and item text, debug notifications,
  the MCM page, and the PrismaUI dashboard.
- Machine-drafted language files for every official Skyrim SE language, shipped and marked unreviewed.
- Declaring the shipped languages in `manifest.json`, so SkyrimNet's plugin hub lists them.

### Out of scope

- **Translating prompt instructions.** Prompts stay English (see **Prompts stay English** below).
- **Our own log files.** `NarrativeEngine.log`, the plot, gossip and visitor traces, and
  `NarrativeEngine_EventHistory.log` are developer diagnostics, and they stay English.
- **SkyrimNet's rendering of our `manifest.json` and `settings/NarrativeEngine.yaml`.** SkyrimNet shows those
  strings verbatim in its own UI and offers no locale hook. This is a known gap, and the README should say so.
- **Retranslating history.** Memories and events already written stay in the language they were written in.
- **Noun declension.** Names are inserted in their nominative form. Translators phrase templates so that the
  name never needs a case ending.
- **Fonts.** Which glyphs render is decided by the game's font configuration, not by us. We detect a mismatch and
  say so (see **Fonts** under **Encoding**), but we do not ship or switch fonts.

---

## Design

### Every string belongs to exactly one of three audiences

The whole design rests on one classification. Each string the plugin produces, and each field an LLM returns, is
exactly one of the following:

| Class          | Who reads it                                         | Lives in                | Language                          |
| -------------- | ---------------------------------------------------- | ----------------------- | --------------------------------- |
| **Prompt**     | Only an LLM, never shown and never persisted         | `.prompt` files         | English                           |
| **Content**    | A player can read it, or it is persisted             | Language file           | The active language               |
| **Token**      | Only code: ids, enum values, keys, EditorIDs         | C++ or data INIs        | None; never shown raw             |

Three rules settle the cases on the boundaries:

- **Persisted counts as visible.** Anything written to SkyrimNet's memories or event history is content: the
  player can open it in SkyrimNet's UI, and NPC dialogue quotes it back to them. Anything written to the co-save
  that a surface later shows is content.
- **An LLM output field takes the class of where it ends up.** For example, a letter's `body` is content, its
  `mood` is a token (it is checked against a closed set), and a visit's `briefing` is a prompt if nothing but the
  conclusion poll reads it. The table that classifies each field is the contract the output-language line is
  written from.
- **Tokens are never shown raw.** If the dashboard currently shows `RisingAction` or a beat id, the label comes from
  the language file and the id is only the key.

This classification also answers the README's warning. Code that parses LLM output only ever parses tokens, and
tokens are not translated. The only thing the active language changes is the content that reaches the player.

### Language detection and selection

**Detection.** The game's language is `sLanguage:General`, read through
`RE::INISettingCollection::GetSingleton()->GetSetting("sLanguage:General")` on the main thread at `kDataLoaded`.
The SE launcher knows exactly nine tokens: `english`, `french`, `german`, `italian`, `spanish`, `polish`, `russian`,
`japanese` and `chinese`. Chinese is Traditional; there is no Simplified variant. Case varies in the wild
(`Skyrim_Default.ini` ships `ENGLISH`, and hand-edited `polish` works), and the game's own file lookups ignore it, so
we compare case-insensitively. Our file stem is the token in lowercase.

**Override.** A new setting, `[Localization] sLanguage`, empty by default. When it is set, it names a language file
stem, whether or not Skyrim ships that language. That covers languages such as Portuguese or Czech, where the game
itself usually runs in English. It lives in the plugin INI and the MCM overlay like every other setting. The MCM
offers *Auto* plus the shipped languages as a fixed dropdown, and the INI accepts any stem.

**Resolution** goes override, then detected language, then `english`, and the first one with a loadable file wins.
The log records the detected language, the override, and the result.

**Fixed for the session.** The language is resolved once at `kDataLoaded`. A changed override takes effect at the
next game launch. Switching mid-session would split one session's content between two languages for no benefit,
since history is frozen in whatever language wrote it anyway.

### Language files

**Location and format.** The files live at `Data/SKSE/Plugins/NarrativeEngine/Languages/<stem>.ini`, encoded in
UTF-8, and are read with SimpleIni. SimpleIni is already a dependency, its sections map well onto subsystems, and
its `<<<END` heredoc syntax handles the few multi-line templates. INI is also what our other data files use, so a
player who has edited one has edited them all.

**Header.**

```ini
[Language]
Name = German          ; the English name, used in the output-language line
NativeName = Deutsch   ; shown in the MCM and the dashboard
Code = de              ; bare ISO 639-1, the form SkyrimNet uses for every language setting
Status = draft         ; draft | reviewed
PluralRule = one_other ; selects a built-in CLDR plural-rule family
Fonts = en             ; the vanilla font sets whose glyphs cover this language (see Fonts, under Encoding)
```

**Two names for one language, for two systems.** The file stem is Skyrim's `sLanguage` token, because that is what
detection reads. `Code` is the ISO 639-1 code, because that is what SkyrimNet uses: in its plugin manifests, its
dashboard's UI language, and its TTS and speech-recognition settings. SkyrimNet normalizes regional forms
(`en-US`) down to the bare code, and so do we. Carrying the code now costs one line per file. It gives us a ready
answer if SkyrimNet ever exposes a language setting of its own, and the manifest needs it (see **Draft
translations**).

**Grammar settings.** Everything English-specific that the current helpers hard-code becomes a per-language value:

```ini
[Grammar]
ListSeparator = ", "
ListPairSeparator = " und "
ListFinalSeparator = " und "
CapitalizeSentenceStart = true
PhraseLowercaseFirst = false   ; English lowercases a scheme's first letter; German must not
IndefiniteArticle =            ; empty: no automatic article; templates phrase around it
IndefiniteArticleBeforeVowel =
```

**Keys** are `[Subsystem.Area] key = template`, for example `[Plot.Memory] watch.agent_dispatched = …`. Code
never names a key with a string literal. Each key is declared once in a C++ table, together with its placeholder
signature, so a mistyped key fails to compile and the formatter knows which arguments each template takes.

**Fallback is per key.** When the active file is missing a key, or the key fails validation, that key renders from
English. The file still loads. A load census logs, once per session, how many keys fell back and why, the way the
plot census reports rejections.

**Player edits live in an overlay.** `Languages/<stem>.custom.ini` is never shipped. When it exists, it is read after
the shipped file and overrides it key by key, under the same validation. A rejected overlay key falls back to the
shipped key, not straight to English. A mod update replaces the shipped file and never touches the overlay, so a
player who rewords one template keeps that one line through every update, and still receives every other fix. The
census reports how many keys the overlay supplied, and names any overlay key that matches no declared key, since
that is almost always a typo that would otherwise look like a rule in force.

**Validation at load.** A translated template that references a placeholder its English counterpart does not have,
or whose select or plural is malformed, or that lacks an `other` branch, is rejected and falls back to English.
English itself is validated by a unit test: every declared key is present, and every placeholder matches its
declaration. Every shipped file is loaded by the same test, which asserts zero rejections.

### The template formatter

The formatter replaces `PlotVerbs::Render`, generalizing it. It is pure, has no engine dependency, and is
unit-tested like `CharacterBioResolver`. Its syntax is a small subset of ICU MessageFormat, so anyone who has
localized software before will recognize it:

| Form                                           | Meaning                                                       |
| ---------------------------------------------- | ------------------------------------------------------------- |
| `{agent}`                                      | The argument's text; a person argument renders as their name  |
| `{agent, select, female {…} male {…} other {…}}` | On a person, selects on their sex; on text, on its value      |
| `{count, plural, one {# Tag} other {# Tage}}`  | CLDR plural category of a number; `#` is the number           |
| `{names, list}`                                | Joins a list with the language's separators                   |
| `{scheme, phrase}`                             | Applies the language's phrase rules (e.g. `PhraseLowercaseFirst`) |

Arguments are typed: **text**, **person** (a name plus a sex from `TESNPC`), **number**, or **list**. Unfilled
arguments render the existing fallbacks ("someone", "somewhere", "something"), which become language-file keys
themselves.

**Plural rules are code, not text.** CLDR's cardinal rules are arithmetic, and a file should not have to express
them. The formatter carries the rule families the shipped languages need: none for Japanese and Chinese,
one/other for English, German, Italian and Spanish, the French 0-or-1 rule, and the Russian and Polish
one/few/many rules. A language file selects one by name. A new language picks the family that matches it.

**Character handling comes from utf8proc.** Sentence capitalization and `PhraseLowercaseFirst` currently use
byte-level `toupper`/`tolower`. Those silently do nothing to `é` or `ж`. Case mapping, case folding, UTF-8
validation and the character categories word splitting needs come from
[utf8proc](https://github.com/JuliaStrings/utf8proc). It is a small C library (MIT, in vcpkg, maintained by the
Julia project) covering all of Unicode, not a hand-built table for a few blocks. Scripts without case pass through.

A full MessageFormat implementation was considered and rejected. ICU is the only serious one. vcpkg's ICU carries
about 30 MB of data. Windows' own `icu.dll` costs nothing to ship, but it exposes only ICU's C API, whose message
formatting takes numbered arguments (`{0}`), not named ones. And Wine did not provide it until recently, which
matters for players on Proton. The subset above is a few hundred lines, and its plural families are thirty lines of
arithmetic.

**No escaping.** A literal brace in a template is a load error. None of the English text needs one, and a syntax
for escaping is complexity a translator should not have to learn.

**Threading.** The table is loaded once on the main thread at `kDataLoaded`, before any worker thread starts, and is
immutable afterwards. Reads take no token and no lock. This is the precedent `Settings::Get()` already sets for
configuration that every thread role reads.

### Prompts stay English

The prompt instructions are not translated. Models follow English instructions most reliably, and our prompts are
tuned against English behavior. Keeping one version of each prompt means a fix lands once instead of nine times.
The prompts stop containing anything a translator has to touch. What changes is where their prose lives.

**No LLM-bound prose in C++.** C++ hands prompts structured data, and the prompt file renders it. The current
offenders, grouped by what replaces them:

| Today in C++                                                               | After                                                                                        |
| -------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------- |
| Beat `Description()` strings (ambush, letter, visit)                       | One prompt file per beat under `components/narrative_engine/beats/`, rendered by name        |
| Rejection and retry sentences (`PlotBirthParse`, `PlotCandidates`)         | `retry: {code, args}`; each prompt renders the codes it can receive                          |
| Decision option lines and purposes (`PlotPlanning`)                         | Structured fields (step number, verb, slots); the decision template phrases them             |
| `GossipContent::BandLengthSpec`                                            | `lo`/`hi` integers; the seed prompt phrases the range                                        |
| The event timeline (`SkyrimNetEvents`, combat, weather and travel renderers) | Structured events (`kind` plus fields); a shared prompt include renders each kind           |
| `FormatRelativeGameTime`                                                    | A pre-bucketed `{value, unit}`; the include phrases it                                       |
| `PlotVerbs` menu lines and plan sketches fed to plot prompts               | Verb id plus filled slots; a shared plot-verbs include phrases each verb                     |
| `PlotAwareness` paragraph from the `ne_plot_awareness` decorator            | The decorator returns JSON; the `0300_ne_plot_awareness` submodule words it                  |
| `DecisionProbe` fixed sentences                                             | Its prompt file                                                                              |

Aggregation stays in C++. Condensing a run of hits into "A and B trade blows" is a decision about structure, so
C++ emits a condensed record, and the prompt decides how to word it.

**Two SkyrimNet mechanisms make this possible, and both are established, not assumed:**

- **A decorator can return structure.** SkyrimNet parses a C++ decorator's returned string as JSON, and falls back
  to the plain string when it does not parse. That fallback is why today's prose-returning `ne_plot_awareness` works.
  So the decorator returns the mastermind's or agent's plot as fields, and the submodule indexes into them
  (`{% set pa = ne_plot_awareness(npc.UUID) %}` … `pa.scheme`) and writes the sentences.
- **A template name can be computed.** SkyrimNet's Inja evaluates function arguments as expressions, and `+` joins
  strings inside them, so `action_select` renders each beat's description with
  `render_template("components\\narrative_engine\\beats\\" + cand.name)`, guarded by `prompt_file_exists`. Paths
  resolve through the layer stack like any other template path. The beat files must **not** live under
  `submodules/`, because SkyrimNet injects every file there into every assembled prompt.

The beat descriptions get an incidental improvement from the move. They can now use `{{ player.name }}`, which
[`CUSTOM_PROMPTS.md`](../CUSTOM_PROMPTS.md) currently says a C++ string constant cannot.

**Data files are not hard-coding.** Text a prompt reads from a plain-text data file, such as `AttackerGroups.ini`'s
`Flavor`, already satisfies the goal of nothing hard-coded, and it stays where it is. So does memory text read back
from SkyrimNet, and character bios.

**Rejection text in our logs.** Once retry sentences move to prompts, `PlotLog` records the rejection code and its
arguments instead of an English sentence. That avoids keeping a second English copy of every rejection.

### The output-language line

Every prompt with a content field gets a line naming those fields and the language, from a shared include:

```jinja
{% if output_language != "English" %}
Write {{ content_fields }} in {{ output_language }}. Every other field keeps the exact values described above.
{% endif %}
```

C++ sets `output_language` from the header's `Name` in every content-producing context. The line is omitted for
English, so English prompts stay byte-identical to today's and need no re-tuning. Naming the token fields as
unchanged matters as much as naming the content fields. A model told only "write in German" will translate `mood`
too.

Provisional map of prompts to content fields. The first implementation step confirms each field against where it
actually ends up:

| Prompt                    | Content fields                                       | Token fields stay                    |
| ------------------------- | ---------------------------------------------------- | ------------------------------------ |
| `letter_compose`          | `body`, `letter_label`                               | `mood`, `topic_tag`, `tags`          |
| `visit_compose`           | `narration_text` (spoken via `DirectNarration`)      | `mood`, `topic_tag`                  |
| `gossip_seed`             | every band                                           | —                                    |
| `plot_ambition`           | `ambition`, `scheme`, `finish`                       | `goal`, every `roles` key            |
| `action_select`           | ambush `narration_prose`                             | beat name, every id parameter        |
| `story_eval`              | `narrative_note` (shown on the dashboard)            | phase, scores                        |
| Decisions, `plot_cast`, `gossip_eval`, `visit_conclusion_poll` | none                    | all                                  |

**Memory tags are tokens, LLM-written or not.** SkyrimNet embeds only a memory's `content` for retrieval. Its
keyword score reads `content` too. Tags are used for exact include/exclude filtering and are printed verbatim beside
each retrieved memory, but they never reach the embedding. So the letter's facet tags stay English like our own
`ne_gossip` and `narrative_engine_plot`, and the language that retrieval actually depends on is the language of
`content`, which is the player's.

**Templates constrain the content's shape, so the language file states the shape.** `scheme` is spliced into
"my plan to {scheme}". English needs a bare verb phrase there ("steal the amulet"), and German needs a *zu*-clause
("das Amulett zu stehlen"). So the language file carries the form each spliced field must take, and C++ passes it
into the context:

```ini
; german.ini
[Prompt.Form]
scheme = an infinitive clause ending in "zu" plus the verb, e.g. "das Amulett zu stehlen"

; japanese.ini
[Prompt.Form]
scheme = a verb phrase ending in the plain dictionary form, e.g. "アミュレットを盗む"
```

This is the one place language-file text reaches a prompt. It belongs in the language file because the template it
has to fit is in the language file. A translator who rewrites the template is the person who knows what shape its
slot needs.

### Validators that read content

Five validators inspect content, and each is English in two ways. One is its word lists. The other is *where* it
looks, which is English word order. Moving the lists to the language file is not enough by itself:

| Validator                        | What it guards                                                  | Where it looks, in English                                                     |
| -------------------------------- | --------------------------------------------------------------- | ------------------------------------------------------------------------------ |
| `PlotBirth::SchemeOpensWithVerb` | A scheme reads as an instruction inside "my plan to {scheme}"   | The start: a pronoun, article or "to" first, or a modal ("is", "plans") second |
| `PlotStepParse::NamesThePlayer`  | No plot text names the player                                   | Anywhere: "dragonborn" as a whole word; the player's name split on spaces      |
| `EngineUtils::IsGhostRecord`     | Apparitions the ghost flag misses stay out of plots             | The end: "ghost" as a display name's last word ("Dunmer Ghost")                |
| `LetterComposer` title stripping | A sender's name fits the 24-character fallback title            | The start for titles ("Jarl", "Thane", …); after the last " the " for epithets |
| `PlotBirthParse::SameLine`       | A scheme is not a repeat, or a restatement of the ambition      | Whole line, folding case on ASCII only                                         |

The first check is worth reading closely. It never detects a verb, even in English. It rejects openings that are
known not to be verbs. What it actually guards is the *shape* the template slot needs, and every language has that
shape. What changes is where the giveaway sits: at the start in English, at the end in Japanese, and at the end in
German as well, where a *zu*-clause puts the verb last.

**So each check is described per language, in one shared vocabulary.** Every check in `[Matching]` takes up to four
lists, keyed by position:

| Key suffix       | Passes when                                                |
| ---------------- | ---------------------------------------------------------- |
| `.Reject.Start`  | the text does not begin with any entry                     |
| `.Reject.End`    | the text does not end with any entry                       |
| `.Require.Start` | the text begins with at least one entry, if any are listed |
| `.Require.End`   | the text ends with at least one entry, if any are listed   |

A detector such as the ghost test or the player-title test uses `.Match.Start`, `.Match.End` and `.Match.Anywhere`
instead, and fires when any entry matches.

An entry is a sequence of word patterns separated by spaces, and `*` inside a pattern stands for any letters. So
`* is` means "any word, then *is*", which is how the English second-word modal rule is written without a separate
"second word" position. `drachenblut*` matches the inflected *Drachenbluts* too. A language whose script does not
separate words with spaces (`[Grammar] WordSeparated = false`, which applies to Japanese and Chinese) matches each
entry as a character sequence at that position.

The scheme check in three languages:

```ini
; english.ini
Scheme.Reject.Start = he, she, they, it, i, we, you, his, her, the, a, an, this, that, to, * is, * plans, * wants

; german.ini -- "das Amulett zu stehlen", or "das Paket abzuholen" with zu inside a separable verb
Scheme.Require.End = zu *, *zu*

; japanese.ini -- a plain-form verb phrase such as "アミュレットを盗む"
Scheme.Require.End = う, く, ぐ, す, つ, ぬ, ぶ, む, る
Scheme.Reject.End = ます, です, た, だ
Scheme.Reject.Start = 彼は, 彼女は, 彼らは, 私は
```

The other checks need the same treatment:

- **The player's title** lists that language's own term: `ドラゴンボーン` matches anywhere in unseparated text, and
  `drachenblut*` covers German's case endings. **The player's name** splits on the language's name separators:
  a space, plus `・` for Japanese. The three-character minimum counts code points, not bytes. A two-letter Cyrillic
  name is four bytes and passes today when it should not.
- **The ghost test** looks where the language puts the noun: `.Match.End = ghost` for English, but
  `.Match.Start = fantôme, spectre` for French, and `.Match.Anywhere = *geist` for German, where it compounds.
- **Title stripping** keeps its prefix list (`Title.Strip.Start`), and its epithet separator becomes a list
  (`Title.Strip.After = the` for English, `le, la` for French, `der, die, das` for German). The 24-character budget
  is computed from the localized "Note from" prefix, not from a constant derived from the English one.
- **`SameLine`** needs no language data, only the formatter's UTF-8 case folding.

**These are heuristics, not grammars.** `*zu*` also accepts "das Geld zurück", which has no verb. The rules exist
to catch the malformations models actually produce, and the plot census is what judges whether a language's rules
are good enough. That is also how the drafts get tuned: the language trial (see **The language trial**) reports
rejection rates per check, for each language, against an English baseline.

**The prompt form and its check are a pair.** `[Prompt.Form]` tells the model what shape a slot needs, and
`[Matching]` verifies that it got that shape. A translator who rewrites "my plan to {scheme}" updates both together,
and the two keys sit side by side in each file so that one is not changed without the other.

**Turning a check off is a deliberate act.** If no list can express a check's shape in some language, the file says
so explicitly, with a reason: `Scheme.Disabled = <why>`. The load census names every disabled check. A unit test
fails if a shipped file leaves a check with neither rules nor a `Disabled` entry, so a gap is never silent.

`[Matching]` uses the game's own terms for its language, and those terms can be looked up. The Spriggit export
carries every localized string in all nine languages, not only English. `Book3ValuableDragonborn`'s name alone gives
the player's title in each: *Drachenblut*, *Enfant de dragon*, *Sangre de Dragón*, *Sangue di Drago*, *Smocze
Dziecię* (inflected *Dziecięcia*), *Драконорожденный* (inflected), *ドラゴンボーン* and *龍裔*. Several are multi-word
and two inflect, which is why entries are word sequences with stem patterns. Every `[Matching]` term in a draft
cites the record it was taken from, so a reviewer can check it, and no draft invents a term the game does not use.

### Names are tokens in the player's script

When the model fills a plot role, it writes a person's name, and code matches that name against the population.
The classification makes those names tokens: they come from the game, and the model copies them. They are not
translated. But they are written in the player's script, and the two matchers that read them assume ASCII. This
is the one class of misparse the classification does not prevent by itself:

- **`PlotCandidates::Closest`** splits text into words on ASCII `isalnum`, and counts only words whose first byte
  is an ASCII capital. Every byte of a Cyrillic, Japanese or Chinese name is non-ASCII, so such a name splits into
  no words at all. Every role in a Russian, Japanese or Chinese plot would resolve to "names nobody in Skyrim", and
  every plot birth would be rejected. Accented Latin degrades rather than fails: *Jörgen* splits into *J* and *rgen*.
- **`PlotCasting::ContainsWord`** decides whether a step's text names a person, so that the person a step is
  *about* is not cast to carry it out. It allows for one inflection, the English possessive (*Veleth's*). German
  writes the genitive without an apostrophe (*Veleths Bewegungen*), and Russian and Polish change the ending
  (*Ульфрика*). In those languages the exclusion misses, and the bug this check was written to fix (Captain Veleth
  cast to shadow Captain Veleth) comes back.

The fix is the same UTF-8 handling the formatter needs. Words split on Unicode whitespace and punctuation. "Begins
with a capital" applies in cased scripts, and every word counts in caseless ones. In a language with
`WordSeparated = false`, a whole menu name is matched as a substring, since there are no word boundaries to match
on. The endings a name may carry and still be the same person become a `[Matching]` list
(`Name.Suffixes = 's` in English; `s` in German; the case endings in Russian and Polish). `NamesThePlayer`'s
player-name half uses the same splitter and the same suffix list.

These matchers resolve EditorID-keyed data elsewhere (`GossipFactions.ini` matches EditorIDs, which are never
localized), so this concerns only text the model wrote.

### Persisted text

**Rendered when written, then frozen.** A SkyrimNet memory or event is rendered once, in the active language, and
never changes. Changing language mid-playthrough leaves earlier history in the earlier language. That is
acceptable: NPC dialogue copes with mixed-language memories better than it would cope with memories that had been
rewritten.

**The co-save stores structure where the structure already exists.** C++-composed text that is saved today and
could be re-rendered from saved fields stops being saved:

- A plot's `concession` sentence is rendered from its scheme when shown.
- `DecisionLog` failure reasons become a code plus arguments.
- `CombatEventLog`'s damage-source labels ("poison", "a trap") become an enum.
- `TravelEventLog`'s "The Player" party fallback becomes a flag.

LLM-written text stays saved as text, since it has no structure to re-render from. The affected records bump their
versions. Per the save-compatibility rule, nothing migrates until 1.0.0.

**Content that is persisted moves to the language file**, including the debug and test-plot literals the dashboard
seeds (`PlotTestPlots`, the debug plot in `PlotState`). They are saved and written to memories like any other plot.

### The `PlotVerbs` table splits by audience

The 24-verb table currently serves three audiences from one struct. Under the classification it splits:

- **Memory templates** (the nine first-person memories per verb, each slot's `remembers` lines, and the plot-level
  succeeded, failed and conceded lines) are content, and move to `[Plot.Memory]`.
- **The dashboard line and caption** are content, and move to `[Plot.Dashboard]`.
- **Menu lines, plan sketches, slot `says` text and the task phrases** fed to plot prompts are prompt text, and
  move to the plot-verbs prompt include.

What stays in C++ is the part that was never text: the verb ids, slot names, slot kinds, and per-verb traits.

### Data INIs keep their text as the default

`PlotFactions.ini`'s `DisplayName`, `PlotItems.ini`'s phrases and `AttackerGroups.ini`'s `DisplayName` are content,
because they reach plot memories and the dashboard. They stay in their data files as the default text. A language
file can override any of them by id (`[Data.Faction] college_of_winterhold = das College von Winterfeste`). Modded
factions and items are first-class, so a modder who adds a row should not have to touch nine language files for it
to work. The row works immediately, in its own words, and translators can catch up when they get to it.

`PlotRecipes::Phrase`'s "a sheaf of {}s" and its a/an choice become `[Plot.Item]` templates. Articles come from
`[Grammar]`, and languages that cannot pick an article without a noun's gender leave it empty.

### Player-visible surfaces

**SkyrimNet memories and events.** These are the letter-sender memory, gossip's heard and told lines, every plot
memory, and the visit walk-out, valediction and re-engage narrations. Each becomes a language-file template.

**Letters.** The fallback title "Note from {sender}" and its stripping lists move to the language file. A book
title is capped by the engine at **24 characters**. Today's code counts that cap in bytes, which is the same thing
only for ASCII: it would hold a Russian title to 12 letters and a Japanese one to 8. The cap is counted in code
points, both where the LLM's `letter_label` is checked and in the fallback, and truncation lands on a code-point
boundary. The prompt states the limit in characters, as it already does. The ESP's item-card text, "A sealed
letter.", is served from the language file through the same description hook that already supplies the body. The
ESP's other book text is placeholder that never displays.

**Debug notifications.** The four `[NE]` notices become `[Notify]` templates.

**MCM.** `config.json` switches to `$NE_…` keys. MCM Helper resolves those through
`Interface/Translations/NarrativeEngine_<LANGUAGE>.txt` (UTF-16 LE, `$KEY<TAB>text`), which is the format it
requires. Those files are the MCM's language files, shipped for every language. There are about fourteen strings
in each, and nothing generates them. Skyrim picks that file by the *game's* language, not by our override, so a
player who overrides Portuguese on an English install gets an English MCM. That is a documented limitation.

**Dashboard.** The TSX holds keys, not English. C++ sends the `[Dashboard]` section as a string table with the
initial state. The dashboard formats through a TypeScript port of the same formatter subset, and both
implementations run a shared fixture of template, arguments and expected output, so they cannot drift. The port
takes plural categories from the browser's own `Intl.PluralRules`. Ultralight, which PrismaUI runs on, bundles ICU
data and its `WebCore.dll` carries `Intl.PluralRules`. If `Intl` turns out to be missing at runtime, every plural
renders its `other` branch and the status banner says so, so the dashboard never fails to render. List joining does
**not** use `Intl.ListFormat`, because the separators must be the language file's own, overlay edits included. C++ sends
ids for anything the dashboard labels itself (phases, beat names, outcomes), and pre-rendered text for content it
already rendered (plot step lines, gossip bands, letter bodies). `<html lang>` follows the active language.

### Encoding

**Write UTF-8 everywhere.** Every official SE string table is UTF-8, in all nine languages and English included.
That was measured by decoding every `STRINGS`, `DLSTRINGS` and `ILSTRINGS` file in the base Interface archive, which
an English install ships for all nine languages, and found zero invalid strings. The SE Polish tables are not the
LE-era custom 1250 encoding UESP describes. The engine keeps a string's bytes as given, and Scaleform reads `char*`
text as UTF-8 (CommonLibSSE-NG's `GFxMovie::SetVariable` documents it). So `SetFullName`, book text, Papyrus
strings, notifications and the dashboard all take UTF-8. Language files and SkyrimNet's API are UTF-8 already.

**Validate what we read back.** A form name read from the engine is usually UTF-8 but need not be. Non-localized
plugins written by ANSI tools can carry Windows-1252 bytes, which the engine hands back unchanged; that is what
`CharacterBioResolver::RepairWindows1252` (mirrored from SkyrimNet) repairs. Any name that enters a template or a
matcher goes through UTF-8 validation and that repair first. Our own output never needs it, because the sanitizer
already drops invalid sequences.

**Fonts.** The glyphs available are those of the font set named by `sFontConfigFile` in `[Fonts]`. The launcher
writes it alongside `sLanguage`, and an English install ships every language's font set:

| Font set   | Covers                                                                 | Used by                                   |
| ---------- | ---------------------------------------------------------------------- | ----------------------------------------- |
| `fonts_en` | Latin-1, plus œ and cp1252 punctuation; no Polish letters, no Cyrillic | English, French, German, Italian, Spanish |
| `fonts_pl` | All Polish letters, plus Czech and Slovak                              | Polish                                    |
| `fonts_ru` | Full Cyrillic, plus Polish                                             | Russian                                   |
| `fonts_ja` | Kana, about 6,700 CJK ideographs, Cyrillic                             | Japanese                                  |
| `fonts_cn` | About 17,500 Traditional Chinese glyphs                                | Chinese                                   |

Detecting the game's language therefore always gets matching fonts. The override is where it can go wrong: Polish
or Czech on an English font configuration renders letters as blanks. Each language file's header names the font
sets that cover it (`Fonts = pl, ru` for Polish). At startup the plugin compares that against `sFontConfigFile`,
and on a mismatch it logs a warning and shows it in the dashboard's status banner. The README gives the one-line
INI fix. This needs no font work of ours, and it turns "my letters have holes in them" into a stated cause.

### Draft translations

Every official Skyrim SE language ships a file with `Status = draft`, with matching MCM translation files. The
drafts are machine-written. They pass the same load validation as English, and they are listed as unreviewed in
the README and the dashboard. The drafts include the `[Matching]` lists and the `[Prompt.Form]` shapes, not only
the templates. Both are the parts a native speaker most needs to check, and the README asks reviewers to check
them first.

**The manifest declares what ships.** SkyrimNet's plugin manifests carry a `languages` array of ISO 639-1 codes,
which its plugin hub filters on. Ours lists the `Code` of every shipped language file, draft or reviewed, since a
draft still produces content in its language. The list is written by hand in `manifest.json`, like the rest of that
file. The configure step already fails when the manifest's id or version disagrees with the repo, and it gains one
more check of the same kind: the `languages` array must match the set of `Code` values under `Languages/` exactly.

### The language trial

Whether a draft's rules are good enough can only be measured against a real model, a real population and SkyrimNet's
real prompt rendering, which means the running game. But changing the game's language means editing an INI and
relaunching, and the language is fixed for the session by design. So the measurement does not change it.

The **language trial** is a dashboard button. It runs plot birth a fixed number of times under each of English,
German, Russian and Japanese, inside an ordinary English session. Each run uses that language's table for everything
plot birth reads: the output-language line, `[Prompt.Form]`, `[Matching]` and the name matchers. Nothing a trial
produces is kept. Its plots are discarded before they reach `PlotState`, so nothing is saved, written to memory or
spent from the plot budget. The dashboard shows, per language: births attempted and accepted, rejections by code and
by check, how many distinct masterminds were cast, and a few of the accepted plots' memories rendered in that
language.

It covers German for the *zu*-clause and the genitive, Russian for Cyrillic and case endings, and Japanese for
unseparated, verb-final text. Those three exercise every rule shape in `[Matching]`. English runs in the same trial
as the baseline, so the comparison is between runs made minutes apart against one model, not against a census from a
different build.

This needs exactly one thing from the design: anything plot birth reads from a language table takes the table as a
parameter, instead of reaching for the active one. `Localization` can load a second, read-only table on demand for
the trial. The session's own table never changes.

### Provisional module structure

| Module                    | Role                                                                                                 |
| ------------------------- | ---------------------------------------------------------------------------------------------------- |
| `TextUtf8`                | **New, pure.** utf8proc wrapped: validation, repair, case, folding, code points, word splitting.     |
| `TextTemplate`            | **New, pure.** The ICU subset and the plural families.                                               |
| `TextKeys`                | **New, pure.** The declared key table and each key's placeholder signature.                          |
| `LanguageTable`           | **New, pure.** One language's table from INI text: header, grammar, per-key fallback, overlay, census. |
| `Localization`            | **New.** Detection, override and font check; reads the files and publishes the session's table.      |
| `PlotVerbs`               | Loses its templates and `Render`; keeps ids, slots and traits.                                       |
| Event renderers           | Emit structured events; keep condensation logic.                                                     |
| `DashboardUIManager`      | Sends the string table; sends ids where the dashboard labels things itself.                          |
| `dashboard/src`           | Gains the formatter port and a `t()` lookup; every literal becomes a key.                            |

---

## Resolved questions

The first draft of this design left six questions open. Each was settled as recorded here, and the sections above
already describe the answer. The reverse-engineering, measurement and audit work was throwaway and is not in the repo;
what it established is.

1. **How `PlotAwareness` reaches dialogue prompts without C++ prose.** SkyrimNet's public API has no function that
   renders a template from C++, but it does not need one. Disassembling `SkyrimNet.dll`, the wrapper around every
   C++-registered decorator calls into nlohmann's JSON lexer, and a catch block logging "Error parsing result to JSON"
   falls back to the raw string. SkyrimNet's own docs show the same for Papyrus decorators (`.mudcrabName` on a
   decorator's result). So `ne_plot_awareness` returns JSON, and its submodule writes the sentences. See **Prompts
   stay English**.
2. **Whether `render_template` accepts a computed name.** Yes. SkyrimNet's Inja evaluates function arguments as
   expressions, and installed prompts already build arguments with `+` (`replace(…, "with " + default(…) + ": ", "")`).
   Template paths resolve through the layer stack (see
   [`skyrimnet-character-profile-lookup.md`](../engine-findings/skyrimnet-character-profile-lookup.md)). The one
   constraint found is placement: nothing under `submodules/`. Step 29's render check confirms it in the running
   game, and an `if` chain over the fixed beat set remains the fallback if it fails.
3. **What encoding the engine expects.** UTF-8, in every language. The string tables were decoded and validated, the
   launcher's language table was read out of its binary, and the shipped font SWFs were parsed for coverage. The one
   hazard is fonts under the override, which is now detected and reported. See **Encoding**. One behaviour stays
   unmeasured, because nothing in the design depends on it: whether the renderer falls back to Windows-1252 for a
   name that is not valid UTF-8, or drops it. We validate and repair names on read, and never emit invalid UTF-8.
4. **What language memory tags should be in.** English, as tokens. SkyrimNet's embedding model is
   `all-MiniLM-L6-v2`, an English-only model with an English WordPiece vocabulary, and it embeds only a memory's
   `content`. Running that model over stored memories reproduced their vectors from `content` alone, and adding the
   tags made every match worse. Tags are used only for exact filtering and for display. See **The output-language
   line**. The same finding means non-English retrieval will be weaker than English retrieval whatever we do, since
   the model is English-only. That is SkyrimNet's to fix, and the README should say so.
5. **Whether the parse-back holds.** Partly answered in advance, by auditing every byte-level character test on the
   paths that read model output. That audit found the two name matchers that break outright on non-Latin scripts,
   and the inflection gap in the exclusion check; see **Names are tokens in the player's script**. Everything else
   that classifies bytes reads EditorIDs or our own data. What an audit cannot settle is whether the drafts' rules
   are *good*. That is the language trial's job (see **The language trial**), run once in Step 30: plot birth under
   German, Russian and Japanese, each compared against an English baseline from the same run.
6. **Whether to support a player overlay file.** Decided: yes. `Languages/<stem>.custom.ini` layers over the shipped
   file key by key. See **Language files**.
7. **How a shared prompt file words one item of a list.** Found in Step 12. `render_template` passes no arguments,
   so a file it renders cannot be handed the item. SkyrimNet's devkit documents `{% include "name" %}` as well, and
   its Inja installs an include callback that resolves the path at parse time. Disassembling `SkyrimNet.dll`'s
   include visitor (at `0x3651b0`) shows it renders the included file with the includer's data **and** its
   `additional_data`, which is where Inja keeps loop and `{% set %}` variables. So
   `{% set step = o.step %}{% include "components/narrative_engine/plot_step.prompt" %}` words `o`. OStimNet's
   `{% include "submodules/character_bio/7100_memories.prompt" %}` is a working instance of the path form. Step 29's
   render check confirms the variables reach the include in the running game.

---

## Implementation plan

Ordered so that **English never changes until it is meant to**, and so that the two steps needing the running game
come last.

- Phases A–E move every sentence out of C++ and make the matchers language-aware, while the plugin keeps producing
  byte-for-byte the English it produces today. Each move is pinned by a test before it is made.
- Phase F builds the player-facing surfaces, and Phase G writes the other eight languages.
- Phase H builds the two in-game checks and hands them over.

Every step from 1 onward leaves the plugin building and runnable.

### How steps are verified

Every step is a standalone unit with its own verification, and every step is owned by exactly one party:

- **`[CLAUDE]`** — implemented and verified without launching Skyrim. Verification is a clean
  `pwsh -File build.ps1 build`, a clean `pwsh -File format.ps1`, and, where a step says so, one or more of:
  - **Committed Catch2 tests** in `NarrativeEngineTests`, run by `pwsh -File build.ps1 test`. Pure modules get
    `*.test.cpp`, engine-coupled ones `*.engine.test.cpp`, following the `/unit-test` skill's conventions.
  - **A characterization test**, which is how every move of existing text is verified. *Before* touching the code,
    the step adds a test pinning today's output for a fixed set of inputs, and runs it against the unmodified code
    to show it passes. The move must then leave it passing unchanged. A characterization test that was only ever
    run after the change proves nothing, and the write-up says which commit it first passed against.
  - **An offline render comparison**, for every step that changes what a prompt receives. See below.
  - **A throwaway probe**: a translation unit compiled against the real build flags, a script over the Spriggit
    export, or a check over the shipped files. It lives in the scratchpad, never in the repo. Its result is
    recorded in this document, and it is deleted when the step closes.
- **`[USER]`** — verified in the running game. There are two, Steps 29 and 30, and each is the same shape: **load
  a save, open the dashboard, press one button, and wait for it to finish.** Neither asks for play, navigation,
  console commands or a language change. Anything that would need more than that is built into the button instead.

**The offline render comparison.** SkyrimNet renders prompts with Inja, so a prompt's rendering can be compared
outside the game. The harness is rebuilt in the scratchpad whenever a session needs it, and is never committed. It
is Inja 3.4, header-only, with stub callbacks for what SkyrimNet adds:

- `render_template` and `prompt_file_exists` resolve against our `prompts/` tree;
- decorators return fixture strings through SkyrimNet's parse-as-JSON-else-string rule;
- `player.name`, `is_player`, `render_character_profile` and `render_subcomponent` return fixed markers.

A step renders each affected prompt twice. "Before" uses the parent commit's prompt file and the context its C++
built. "After" uses the working tree's prompt file and its structured context. Contexts come from the real context
builders, called by a throwaway probe over synthetic inputs and dumped as JSON. The two renderings must be
identical, apart from whitespace the step names and justifies. Where a builder cannot run outside the engine, the
step says so and hand-builds its context from the same synthetic inputs.

### How steps are executed

**Closing out a step.** A step is not finished when the code is written. Once both the implementation *and* the
step's own verification are done:

1. **Mark the step complete in this document.** Tick its `- [ ]` to `- [x]`, and write up underneath it what the
   verification actually showed, including anything that turned out differently from what the step predicted.
2. **Commit**, on the checked-out branch. One commit per completed step, holding the implementation, its committed
   tests, and the doc update. Before staging, look at what is staged: probes, harnesses, dumps and scratch data stay
   in the scratchpad.

**Working through the plan.** When told to implement, work **one step at a time, in order**, fully completing each
before starting the next. Keep going until one of these stops you:

- **The next step is `[USER]`.** Stop and hand it over, saying which step is waiting and what it asks the user to
  judge.
- **The plan as written is wrong or impossible**, in a way this document and the design above cannot resolve. Stop
  and ask. Do not silently redesign around it.

A step whose verification *fails* is not a stopping condition: fix it and finish the step.

---

### Phase A — Foundations

#### Step 1 — UTF-8 text primitives

- [x] Complete

**[CLAUDE]**

**Goal:** One pure module, `TextUtf8`, that every later step uses for anything that looks at characters rather than
bytes. It wraps utf8proc, added to `vcpkg.json` and linked into both the plugin and the test targets.

- Code-point iteration, UTF-8 validation, code-point length, and truncation to a code-point count on a code-point
  boundary.
- `RepairWindows1252` moves here from `CharacterBioResolver`, which then calls it, so that "validate and repair
  what we read back" has one implementation.
- Case mapping, first-letter capitalization and lowercasing, and case-folded comparison, for all of Unicode through
  utf8proc. Caseless scripts pass through.
- Word splitting on Unicode whitespace and punctuation (utf8proc's categories), keeping the existing rule that
  hyphens and apostrophes belong to a word (with `PlotCasting`'s asymmetric trailing-apostrophe boundary preserved as
  an option).

No caller changes in this step.

**Verification:** `*.test.cpp` cases covering each function. They include Cyrillic, Greek, Polish, Japanese and
mixed-script strings; invalid sequences; a truncation that would split a multi-byte character; and folding pairs
such as `Ä`/`ä`, `Ж`/`ж` and `Ł`/`ł`. `CharacterBioResolver`'s existing tests pass unchanged.

Done. `include/TextUtf8.h`, `src/TextUtf8.cpp` (a core source) and `src/TextUtf8.test.cpp`, with utf8proc 2.11.3
from vcpkg linked privately into `NarrativeEngineCore`. The vcpkg target sets `UTF8PROC_STATIC` itself, so nothing
else in the build had to know.

`RepairWindows1252` moved together with the three helpers only it used (`SequenceLength`, the UTF-8 encoder and the
code-page table). Its test case moved with it, section for section and byte for byte; only the `TEST_CASE` name
changed, from `CharacterBioResolver::` to `TextUtf8::`. Every other `CharacterBioResolver` test runs unchanged. No
other caller was touched.

`pwsh -File build.ps1 test`: 528 of 528 passed, eleven of them new. The cases worth recording:

- **Folding is full, and normalizing.** `Straße` equals `STRASSE`, and a precomposed `é` equals `e` plus a combining
  acute. Today's ASCII folding treats `Ä` and `ä` as different letters.
- **Ulfric's Russian name is seven code points and fourteen bytes.** `TruncateToCodePoints` cuts it by letters, and
  never splits a four-byte character.
- **`Words` splits Japanese on `・` and the ideographic space, and nowhere else.** An unseparated run comes back as
  one word. A test case records that `StandsAlone` can therefore never match a name inside such a run, which is why
  Step 18 needs substring matching for `WordSeparated = false`.
- **The typographic apostrophe `’` separates words.** The ASCII one does not. This matches today's splitters, which
  only ever saw ASCII apostrophes, because the sanitizer turns the typographic one into `'` in LLM text.

#### Step 2 — The `TextTemplate` formatter

- [x] Complete

**[CLAUDE]**

**Goal:** Parse and render the ICU MessageFormat subset described under **The template formatter**.

- Placeholders, `select` (on a person's sex, or on text), `plural` with `#`, `list`, and `phrase`.
- Typed arguments: text, person, number and list.
- The CLDR plural families: none, one/other, French, Russian and Polish.
- Grammar settings passed in as a plain struct, so the formatter never reads a file.
- A validation entry point that checks a template against a declared placeholder signature and reports every
  problem: an unknown placeholder, a malformed `select` or `plural`, a missing `other` branch, or a literal brace.

**Verification:** `*.test.cpp`. Each form renders correctly. Each plural family is checked at its boundary numbers
(Russian 1, 2, 5, 11, 21, 22, 25; Polish 1, 2, 5, 12, 22; French 0, 1, 2). Every validation error is reported.
Today's `PlotVerbs::Render` behaviour, including the unfilled-slot fallbacks and first-letter capitalization, is
reproduced by the English grammar settings. That last point is what lets Step 5 replace `Render` without changing a
byte.

Done. `include/TextTemplate.h`, `src/TextTemplate.cpp` (a core source) and `src/TextTemplate.test.cpp`.

**The `PlotVerbs` reproduction was checked exhaustively, not sampled.** A throwaway engine test, compiled into the
mocked-engine target against the real `PlotVerbs` and then deleted, ran every verb's every template through today's
`RenderStep`, `RenderPhrase`, `RenderTarget`, `Generic` and `RenderPlotLine`. It covered three slot fills (none, the
first slot only, all) and a scheme both present (with trailing punctuation and space) and missing. It rendered the
same templates through `TextTemplate`, with `{scheme}` written as `{scheme, phrase}` and the same unfilled-slot
fallbacks. **2,729 comparisons, 0 mismatches.** Step 5 can therefore replace `Render` by rewriting `{scheme}` and
nothing else.

`pwsh -File build.ps1 test`: 535 of 535 passed, eight of them new. One thing turned out differently from what the
step assumed. A first test expected `#` inside a `select` nested in a `plural` to be the plural's number. ICU says
otherwise: `#` is the number only directly inside a plural's own branch. The formatter follows ICU, since that is
what a translator who knows it will expect, and the test now writes `{n}` there and asserts the literal `#`. A plural
nested in a plural gets its own `#`.

#### Step 3 — Language tables: keys, loading, fallback and overlay

- [x] Complete

**[CLAUDE]**

**Goal:** Turn INI text into a validated, immutable table.

- `TextKeys`: the mechanism for declaring a key once, with its section, name and placeholder signature, so that code
  refers to keys by symbol.
- The loader parses the `[Language]` header (`Name`, `NativeName`, `Code`, `Status`, `PluralRule`, `Fonts`) and
  `[Grammar]`.
- It validates every key against its declaration and falls back per key: overlay → shipped file → English.
- It reads `<stem>.custom.ini` when present.
- It produces the census: keys loaded, keys fallen back and why, overlay keys used, and overlay keys that match
  nothing.
- `statics/SKSE/Plugins/NarrativeEngine/Languages/english.ini` ships with its header and `[Grammar]`. No content keys
  are declared yet; each later step declares the keys it moves.

**Verification:** `*.test.cpp`, using INI text held inside the tests:

- a missing key falls back;
- an invalid translated key falls back to English;
- an invalid overlay key falls back to the shipped key;
- an unknown overlay key is reported.

A committed shipped-files test loads every file under `Languages/`. It asserts that English defines every declared
key and that every file loads with zero rejections. It is the test every later step's new keys must keep passing.

Done. `TextKeys` (`include/TextKeys.h`, `src/TextKeys.cpp`) declares keys. `LanguageTable` (`include/LanguageTable.h`,
`src/LanguageTable.cpp`) builds a table from INI text. `statics/SKSE/Plugins/NarrativeEngine/Languages/english.ini`
ships with its header and grammar. All three sources are core, and SimpleIni's include path joins the core library.
The tests get `NARRATIVEENGINE_STATICS_DIR`, so the shipped-files case reads the real files.

**One difference from the plan:** the pure loader is its own module, `LanguageTable`, not part of `Localization`.
`Localization` (Step 4) keeps what touches the engine and the disk. The module table above is updated to match.

Three choices the step had to make, now part of the contract:

- **What counts as a rejection.** A template that fails to parse or validate, a header or grammar value that means
  nothing, a header field the language's own file lacks, and any key no part of the plugin declares. Missing keys
  are *not* rejections, because a draft is allowed to be incomplete. They are fallbacks, listed separately. The
  shipped-files test asserts zero rejections for every file, and zero unavailable keys for English.
- **Quoting.** SimpleIni trims values and has no inline comments. So a `[Grammar]` value wrapped in double quotes
  keeps its spaces (`ListSeparator = ", "`), and `english.ini`'s own comment says a `;` after a value is part of it.
  Templates are never unquoted: a sentence that is a quotation should stay one.
- **The overlay changes words, not identity.** `[Grammar]` and any declared key may be overridden. `[Language]`
  lines in an overlay are reported as unmatched and ignored.

`pwsh -File build.ps1 test`: 542 of 542 passed, seven of them new. The cases cover English alone; a shipped language
with its own grammar and a missing key; three kinds of bad translation, each falling back to English; the overlay
over the shipped file, over English, with a rejected line falling back to the *shipped* text, with grammar, and with
typos; header and UTF-8 rejections; a multi-line heredoc template; and the real shipped files.

#### Step 4 — Detection, override, font check, and session wiring

- [x] Complete

**[CLAUDE]**

**Goal:** The plugin knows its language at `kDataLoaded`, and every thread can read it.

- `Localization::Resolve` is a pure function: detected token, override and available stems in; chosen stem and
  reason out. It compares case-insensitively, and resolves override, then detected, then `english`.
- `Localization::FontsCover` is a pure function: the `sFontConfigFile` value and the header's `Fonts` list in; a
  verdict out.
- The engine glue reads `sLanguage:General` and `sFontConfigFile:Fonts` through `INISettingCollection` on the main
  thread. It then loads the chosen table (plus English, always) once, and publishes it immutable, following
  `Settings::Get()`'s pattern.
- A new `[Localization] sLanguage` setting, empty by default.
- The census and the resolution are logged once per session. A font mismatch is logged as a warning (the dashboard
  banner comes in Step 21).

**Verification:**

- `*.test.cpp` for `Resolve`: every token in mixed case, an override naming a missing file, and an override naming
  an unofficial language.
- `*.test.cpp` for `FontsCover`, over the five vanilla font configurations.
- The build. The engine glue is a handful of lines with no logic of its own.

Done. `include/Localization.h` declares both halves. `src/LocalizationRules.cpp` (core) holds `Resolve` and
`FontsCover`, and `src/Localization.cpp` holds the glue: it reads `sLanguage:General` and `sFontConfigFile:Fonts`,
lists the language files, reads the chosen file and its overlay, builds the session table and English, logs, and
publishes. `Plugin.cpp` calls `Localization::Load()` straight after `Settings::Load()` at `kDataLoaded`.
`[Localization] sLanguage` joins `Settings` and the shipped `NarrativeEngine.ini`. It is read by `ReadIniInto`, so the
MCM overlay file can set it too, ahead of Step 22's dropdown.

Three things the step settled that the plan did not spell out:

- **`Load` takes no token.** No dispatcher mints one inside SKSE's message handler. Every other `kDataLoaded` loader
  (`Settings::Load`, `AmbushAttackerGroups::Load`, `GossipGraph::Initialize`) is token-free for the same reason, and
  `Localization::Load` follows them. The accessors are token-free by design, like `Settings::Get`. Publication goes
  through release/acquire atomics, so a reader on any thread sees a complete table or the empty one, never half of
  one.
- **The English table carries no overlay.** It exists for output that stays English whatever the session's
  language, such as our logs, so a player's rewording should not reach it. When the session itself is English, the
  session table does include `english.custom.ini`.
- **The mocked-engine target compiles `Plugin.cpp`, so it now compiles `Localization.cpp` too.** That needed four
  mocks in `testsupport/EngineMock.cpp`. The game's INI collection is not modelled, so `GetSingleton` returns null,
  which reads as "not set". `GetSetting`, `Setting::GetType` and `Setting::GetString` are only reachable through a
  collection, so they abort by name, following that file's existing rule for engine calls nothing stands in for.

`pwsh -File build.ps1 test`: 544 of 544 passed, two of them new. `Resolve` was tested with all nine launcher tokens
in upper and lower case, an override winning, an unofficial language (`portuguese`), an override with no file
falling through and saying so, no language at all, and mixed-case file names. `FontsCover` was tested over all five
vanilla configurations, the empty default, forward and back slashes, Polish on an English install (`NotCovered`),
and a font mod's own file (`Unknown`).

### Phase B — Content out of C++

#### Step 5 — Plot memory and dashboard templates

- [x] Complete

**[CLAUDE]**

**Goal:** Every plot sentence a player can read comes from `[Plot.Memory]`, `[Plot.Dashboard]` and `[Plot.Item]`.

- `kVerbs`' content fields move to the language file:
  - per verb: the dashboard line and caption, and the nine first-person memories;
  - per slot: the `remembers` lines.
- Also moving: the succeeded, failed and conceded plot lines and the "found another way" clause, the
  unfilled-slot fallbacks, and `PlotRecipes::Phrase`'s item phrasing (articles via `[Grammar]`).
- `PlotVerbs::Render` is replaced by `TextTemplate`. The prompt-facing fields (menu lines, sketches, `says`, task
  phrases) stay in C++ until Step 12.
- Person arguments carry sex. Plot population facts gathered at `OnSessionStart` gain the base NPC's `IsFemale`, so
  the plot worker never touches the engine for it.

**Verification:** a characterization test, written and passed against the unmodified code first. It pins every
verb's every template under three slot sets: all filled, some unfilled, and a scheme needing `SchemePhrase`'s
lowercasing and trimming. It also pins every plot-level line and a spread of recipe phrases. It passes unchanged
after the move. The shipped-files test passes with the new keys.

Done. English's `[Plot.Dashboard]`, `[Plot.Memory]`, `[Plot.Remembered]`, `[Plot.Outcome]`, `[Plot.Unfilled]` and
`[Plot.Item]` hold 290 keys. The new `PlotText` module (`include/PlotText.h`, `src/PlotText.cpp`) generates their
keys from the verb table and renders them. `kVerbs` keeps only what was never text, plus the prompt-facing strings:
each verb's three-string block went from twelve strings to three, and each slot's remembers strings became flags.

**The characterization test was written and passed first.** `src/PlotText.engine.test.cpp` drives the real
consumers: `PlotMemories`' six entry points, for every verb under three slot fills, delegated and by the
mastermind's own hand, all three outcomes, with and without going on, and with and without a scheme. It also covers
the dashboard's line and caption, and eight recipe phrases. That is **3,110 renderings**, including each memory's
keeper, source, importance, type, location and related actors, so the move could not quietly change who remembers
what. Their digest, `0xc2fc18bbda4005bc`, was pinned and passed against the unmodified code at `ec54730`. After the
move it passes unchanged, and a line-by-line diff of the transcript it writes is identical.

How the pieces moved, and the choices that went with them:

- **The table is a parameter.** `PlotMemories::On*` take the table first; `PlotTick` passes `Localization::Get()`,
  and so do the dashboard and recipe loading. The language trial (Step 28) needs this anyway.
- **Who a memory relates is who the rendered template names.** `NamedIn` used to scan the C++ template's braces.
  It now asks the table's parsed template, so a translation that drops a person from a sentence also drops them
  from the memory's related actors.
- **Prompt-facing text stays in C++ until Step 12**, and two pieces of it are therefore in two places: each verb's
  dashboard line and caption are in `kVerbs`, for the scheme call's menu, the plan sketch and `Words`, and also in
  `[Plot.Dashboard]` for the dashboard. A new test asserts the English copies are identical, so they cannot drift
  before Step 12 removes the C++ ones. `PlotModel::Label` keeps the C++ line for the log and the prompts.
- **Recipe phrases render through `PlotText::ItemPhrase`.** `PlotRecipes::Phrase` is gone, and `[Grammar]` gained
  `IndefiniteArticle` and `IndefiniteArticleBeforeVowel`. A language whose article follows the noun's gender leaves
  both empty.
- **People carry their sex.** `PlotCasting::Member` gained `female`, read off the base record at population build,
  so the plot worker never asks the engine. English uses no `select`, so nothing it renders changed.
- **`TextKeys::All()` left the core library** for `src/TextKeysAll.cpp`, because the list now includes keys generated
  from an engine-coupled table. The shipped-files test went with it, to `src/LanguageFiles.engine.test.cpp`. That test
  is what proves the remembers flags match the text: English must define every generated key and nothing else.
  `testsupport/ShippedLanguage` builds the shipped English for any engine test, and `LanguageTable::Source` returns a
  template as written.
- **`PlotVerbs`' manifest tests now check `english.ini`.** Each of their English-language rules (which voice names
  the mastermind, that a task phrase appears inside its dispatch, that a noticed memory names nobody, and the rest)
  reads the memory text from the shipped file. That file is what an NPC will actually remember.

`pwsh -File build.ps1 test`: 546 of 546 passed.

#### Step 6 — Gossip, letter-memory and visit narration templates

- [x] Complete

**[CLAUDE]**

**Goal:** The remaining SkyrimNet memories and events come from the language file.

The step moves:

- `GossipContent::ComposeHeard`'s five forms and `ComposeTold`, with listener joining via `{names, list}`;
- the letter sender's memory;
- the visit walk-out, valediction (both variants) and re-engage narrations;
- every fallback name these use ("someone", "the sender", "their visitor", "The player").

Persons carry sex from where each subsystem already gathers its engine facts.

**Verification:** a characterization test over each composer, first passed against unmodified code. Gossip covers
every kinship and source form, and one to four listeners. Visits cover every narration with real and missing names.
It passes unchanged after the move.

Done. The new core module `NarrativeText` (`include/NarrativeText.h`, `src/NarrativeText.cpp`) renders all of these
from English's `[Gossip.Memory]`, `[Letter.Memory]` and `[Visit.Narration]`. `GossipContent::ComposeHeard` and
`ComposeTold` keep their API and now delegate to it, choosing only which framing applies. The letter's memory and
the visit's three narrations are one call each in `LetterPool` and `NPCVisitBeat`.

**Two passes, because three of the four composers had nothing to pin.** The visit narrations and the letter memory
were string concatenation inline in engine-coupled functions. Pass (a) extracted every one into `NarrativeText` with
its English literal unchanged, rewired the callers, and added `src/NarrativeText.test.cpp`, which pins every sentence
and fallback exactly. That includes two edge cases a careless move would change: a lower-case player name is *not*
capitalised for opening a sentence, and an empty listener list renders as it did. All 550 tests passed against
pass (a). Gossip's existing exact-string engine tests (all five framings, one to three listeners, an unknown name)
were already a characterization, and passed there too. Pass (b) moved the literals into `english.ini` and changed
nothing else; the same 550 passed.

Choices the step made:

- **English lists drop the Oxford comma.** Gossip's "Ysolda, Saadia and Hulda" was deliberate ("spoken-voice
  memories, not prose"), so English's `ListFinalSeparator` is now `" and "`. Nothing rendered a list before this step,
  so no output changed. The travel log's party ("A, B, and C") is the one other English list; Step 11 moves it and
  records how it comes out.
- **Fallbacks are keyed by where the person stands.** Today's code says "The player" opening a sentence, "the player"
  inside one, and "their visitor" for the walk-out where the other narrations say "the sender". Each is its own key, so
  nothing had to be capitalised by rule, and a player's own name is never touched.
- **People carry their sex.** `EngineUtils::SexOf` reads it off the base record. That is static record data, already
  read by gossip's kinship lookup on the same worker, so it is safe from any thread. Kinship words still come from the
  game itself, already chosen for the right sex.
- **Engine tests see real English.** Production code now renders through `Localization::Get()`, which is empty until
  `Load` runs, and `Load` never runs in a test. `Localization::Publish` (which `Load` itself now calls) lets
  `testsupport/ShippedLanguage` install the shipped English before any test starts. Every engine test therefore
  renders exactly what an English game would. Without it, the existing gossip, letter and visit tests all failed, and
  they were right to.

#### Step 7 — Notifications and letters

- [x] Complete

**[CLAUDE]**

**Goal:** The plugin's remaining player-facing literals come from the language file.

- The four `[NE]` debug notifications become `[Notify]` templates.
- The letter's "Note from {sender}" fallback becomes a template, and its title and epithet lists move to
  `[Matching] Title.*`.
- The 24-character title cap counts code points, both in the `letter_label` check and in the fallback.
- The item card's "A sealed letter." is served from the language file by the description hook that already supplies
  the body.

**Verification:**

- A characterization test of `SynthesizeFallbackLabel` over today's English cases, first passed against unmodified
  code, then unchanged.
- New cases: a 20-letter Cyrillic name is no longer cut at 12 letters, and a 25-character label is rejected however
  few bytes it has.
- The build, for the hook and the notifications.

Done. The four notices live in English's `[Notify]` and render through `DebugNotify::Text`. The fallback title and
the item card live in `[Letter]`, and the title lists in `[Matching] Title.Strip.Start` and `Title.Strip.After`.
`LetterComposer::FitsTitle` and `SynthesizeFallbackLabel` count `kTitleCharacters` (24) in code points. The
description hook serves the item card from the session table.

**Two passes again.** The notices were format strings at their call sites, with nothing to pin. Pass (a) moved each
into a `DebugNotify::Text` function with its literal unchanged, and changed `PostActorNamed` to take a function that
composes the notice from the resolved name, in place of a `vformat` pattern. It then added exact pins for all four
notices and their fallbacks. It also pinned nine fallback labels exactly; the existing tests only checked loosely
for three of them. 552 tests passed against pass (a). Pass (b) moved the words; the same pins passed, and 553 tests
passed with the new character-count case.

What came out differently from the plan:

- **The fitting rule measures the rendered title, not the name.** The title is now a template, and a language may put
  the name first (`{sender}からの手紙`), so "the room left after the prefix" no longer exists. The last candidate
  shortens the name a code point at a time until the whole title fits. For English it lands exactly where the byte
  arithmetic did: every pinned label is unchanged.
- **The Cyrillic figure in the plan was wrong.** A twenty-letter Cyrillic name used to keep *seven* of its letters,
  because fourteen bytes is seven Cyrillic letters, not twelve. It now keeps fourteen. The new test asserts the exact
  title, and that a 24-letter, 48-byte title fits while a 25-letter ASCII one does not.
- **`[Matching]` holds plain comma lists for now.** The title lists are declared keys with no placeholders, read
  through `Table::Source` and split on commas. Step 16 gives `[Matching]` its own parser and rule vocabulary, and
  Step 17 moves title stripping onto it, along with its still-ASCII case folding.
- **The ambush notice keeps "(1 attackers)".** The step moves words; it does not fix them. A translator can use
  `{attackers, plural, ...}`, and English could adopt it too.

A process note, recorded because it cost a failing test: writing C++ escape sequences through a Python string turns
`"Ð"` into the character U+00D0 rather than the four characters of an escape. The test's first draft held
double-encoded Latin-1 and passed one check by coincidence. Escapes are now written from raw strings.

#### Step 8 — The co-save stores structure, not sentences

- [x] Complete

**[CLAUDE]**

**Goal:** No sentence C++ composed is saved where the fields to re-render it are already saved.

- A plot's `concession` is rendered from its scheme when shown, and no longer saved.
- `DecisionLog` failure reasons become a code plus arguments.
- `CombatEventLog` damage sources become an enum.
- `TravelEventLog`'s player-party fallback becomes a flag.
- The dashboard-seeded test plots and the debug plot take their text from the language file.
- `NEPL`, `DCLG`, `NECE` and `NETR` bump their record versions. Nothing migrates.

**Verification:**

- Round-trip `*.engine.test.cpp` cases for each changed record.
- A characterization test showing every re-rendered string (concession, failure reason, damage phrase, party name)
  matches what the old code saved, for the same inputs.

Done. Record versions: `NEPL` 14 → 15, `DCLG` 1 → 2, `NECE` 1 → 2, `NETR` 1 → 2.

- **Concession.** `Plot::concession` is gone from the struct and the co-save. The dashboard renders
  `PlotText::PlotOutcome(Conceded)` for any plot whose outcome is `Conceded`, which is exactly when the field used to
  be non-empty.
- **Decision log.** `DecisionRecord` gained `failure` (none, beat missing, LLM error, not JSON, not offered),
  `failureDetail`, and `note` (the model's own, invalid JSON, not an object, forced dispatch). `beatSelected` now holds
  only a beat id, and `narrativeNote` only the model's words. `DecisionLog::ActionText` and `NoteText` render the rest
  from English's `[Decision]`. The dashboard renders through the session table. `story_eval`'s `decision_log_tail`
  renders through English, so that prompt is unchanged byte for byte; Step 15 now also covers moving those words into
  the prompt.
- **Combat.** A hit's environmental source is a `Source` enum (actor, poison, spell trap, trap, a named activator,
  unknown). Only an activator keeps a name. The words ("poison", "a spell trap", "a trap", the activator's name
  lowercased) are `SourceWords`, in the renderer, until Step 11 moves the timeline's wording into a prompt.
- **Travel.** An unnamed player is saved as an empty name, and `RenderParty` calls them "The Player".
- **Seeded plots.** The two test plots' ambitions, schemes, finishes, place and item, and every line of the debug plot,
  are `[Plot.Test]` keys rendered at seed time.

**How the characterization was done, and where it could not be.** Before anything changed, the two decision-log
tests that only checked `starts_with("(failed")` were tightened to the exact strings the old code saved, captured by
running them. So were the two parse-failure notes, which were checked by prefix. Those four exact checks passed
against the unmodified code, and pass unchanged through `ActionText` and `NoteText` now. A new test renders every
failure and note code and pins each against the sentence the old code composed, and round-trips a failed record
through the co-save. A new travel test saves an unnamed player and finds "The Player" both before and after a load.
The concession sentence was already pinned by `PlotVerbs`' rendering test.

The combat source words could not be pinned before the change. Raising a hit from a trap or a poison in a test needs
hit events, magic items and activators modelled in `EngineMock`, and building that harness to move four words within
one function was out of proportion. They moved verbatim from `ResolveHitSource` to `SourceWords`, and the existing
save-and-load test passes on the new source byte.

The failure detail stays verbatim, beat validators' English messages included ("sender 0x1A6FF was not offered for
this beat"). It is diagnostic data, the same class as a log line, not something composed for a player.
`pwsh -File build.ps1 test`: 554 of 554 passed.

#### Step 9 — Data INI text, overridable by id

- [x] Complete

**[CLAUDE]**

**Goal:** A language file can reword a faction, an item or an attacker group without touching the data INIs.

- `[Data.Faction]`, `[Data.Item]` and `[Data.AttackerGroup]` are keyed by the data file's own id. An override replaces
  the INI's text, and a missing override keeps it.
- These keys are open-ended, not declared in `TextKeys`, because modded rows are first-class. The census reports any
  override whose id matches no loaded row.

**Verification:** `*.test.cpp` over an override, a missing override, and an unmatched override. With no overrides
present, each data file's loaded display text is identical to before the change.

Done. `LanguageTable` reads every `[Data.*]` section as open-ended raw text: never parsed as a template, never
rejected as an unknown key, and taken key by key from the first of overlay, shipped file and English that has it.
`Table::DataText(section, id)` returns an override or null, and `Table::UnmatchedData(section, ids)` lists the ones
that match no loaded row. Each loader applies overrides where it adopts its display text, and reports the unmatched
ones after loading:

| Section               | Id                                | Loader                                        |
| --------------------- | --------------------------------- | --------------------------------------------- |
| `[Data.Faction]`      | the section id (`college`)        | `PlotFactionRoster::Resolve`                  |
| `[Data.Item]`         | the line's EditorID               | `PlotItemPool::Load`                          |
| `[Data.AttackerGroup]`| the section id (`bandits`)        | `AmbushAttackerGroups`' `ParseGroup`          |

Only display text is overridable. An attacker group's `Flavor` stays in its data file, because it is read only by the
Director's prompt.

`pwsh -File build.ps1 test`: 555 of 555 passed, one new. The new case covers an override by id, case-insensitive ids,
the overlay winning over the shipped file, no override leaving the data file's text standing, an unmatched override
reported against the loaded ids, and `[Data.*]` keys raising neither a rejection nor an overlay complaint. English
ships no `[Data.*]` sections, so every loader's existing tests (rosters, items, attacker groups) ran unchanged. That
is the "identical with no overrides" half of the verification.

### Phase C — Prompt prose out of C++

#### Step 10 — Beat descriptions as prompt files

- [x] Complete

**[CLAUDE]**

**Goal:** `action_select` reads each beat's description from its own prompt file.

- `AmbushBeat`, `NPCLetterBeat` and `NPCVisitBeat` lose `Description()`'s prose. Each moves to
  `prompts/components/narrative_engine/beats/<name>.prompt`.
- `action_select` renders each with `render_template` on the candidate's name, guarded by `prompt_file_exists`.
- Where a description said "the player", it may now say `{{ player.name }}`. That is the one intended rendering
  difference.
- The dashboard stops showing description prose, and shows a `[Dashboard.Beat]` caption instead.

**Verification:** offline render comparison of `action_select` over each beat as the sole candidate and all three
together. The only allowed difference is "the player" → the player-name marker, and each occurrence is listed in the
write-up.

Done. The three descriptions are `prompts/components/narrative_engine/beats/ambush.prompt`, `npc_letter.prompt` and
`npc_visit.prompt`, each holding exactly the text `Description()` returned. `action_select` renders each through
`render_template` on the candidate's name, guarded by `prompt_file_exists`. `Description()` is gone from `IBeat`,
from the three beats and from the three test fakes, and `BeatSystem` no longer sends a description. The three engine
tests that checked a description names the parameters its beat reads now read the beat's prompt file, through a new
`testsupport` `ShippedPrompt`.

**The offline harness, built for this step and reused by the rest of Phase C.** It lives in the scratchpad, is
rebuilt per session, and is never committed. It is a vcpkg-manifest CMake project with `inja` and `nlohmann-json`,
plus `harness.cpp`, which renders one template against a context and a stubs file. vcpkg's baseline gives **Inja
3.5.0**, not the 3.4 the plan named; `CUSTOM_PROMPTS.md` already records renders agreeing across 3.3–3.5. Building it
surfaced one setting the harness must match: Inja treats a line opening with `##` as a statement by default, and every
Markdown heading in our prompts opens that way, so line statements are switched off.

**The comparison.** `BuildBeatSelectPromptContext` sits in an anonymous namespace, so no probe could call it. As the
plan allows, the contexts were hand-built: four of them (each beat alone, and all three), with sender candidates,
memories, attacker groups, recent events and the player context. They carry the real `Name()` and `Description()`
strings, which a throwaway probe constructed the three beats to dump. The "before" side rendered the prompt tree
snapshotted before the change, with descriptions in the context. The "after" side rendered the working tree without
them. **Every rendering is identical except for one added newline per description**, and that is the one named
whitespace difference. A file rendered through `render_template` keeps its trailing newline, and Inja's `{%-` strips
only the spaces on its own line, not the newline before it; a test against the harness showed exactly that. Each
description was already followed by three to five blank lines, so a model sees no difference. `CUSTOM_PROMPTS.md`
now records the behaviour.

**What was not done, and why.** The plan allowed the descriptions to say `{{ player.name }}` where they say "the
player". They keep their words, so that this step's comparison stays exact; `CUSTOM_PROMPTS.md` notes that the old
exception no longer applies to them. The plan's `[Dashboard.Beat]` caption was not needed: the dashboard never showed
beat descriptions, and only the beat-select context read them.

`pwsh -File build.ps1 test`: 555 of 555 passed. The three prompt files deploy with the build.

#### Step 11 — The event timeline as structured events

- [x] Complete

**[CLAUDE]**

**Goal:** `story_eval` and `action_select` word the timeline themselves.

The event renderers emit structured records (a `kind` plus fields) instead of sentences. They are
`SkyrimNetEvents`, the condensed-combat logic, `CombatEventLog`, `WeatherEventLog` and `TravelEventLog`. A shared
include, `components/narrative_engine/event_line.prompt`, words each kind. The step also covers:

- **Relative time** becomes a pre-bucketed `{value, unit}`, which the include words. That covers the `[N minutes
  ago]` prefix, the idle marker, and the `age` on memories in the sender-candidate lists.
- **Condensation stays in C++**, and emits a condensed record.
- **The dashboard's event list and `EventHistoryWriter`** render from `[Event.*]` language templates. The log
  always uses the English table, because logs are out of scope.

**Verification:**

- Offline render comparison of `story_eval` and `action_select`, over a synthetic timeline that contains every event
  kind, both condensation shapes, an idle gap, and ages in every bucket.
- A characterization test over the `[Event.*]` rendering against today's rendered tail, for the dashboard and the
  log.

Done. Every timeline entry now carries a `line` record (a `kind` and that kind's fields) and, when it has a time, an
`age` bucket such as `{"value": 5, "unit": "minute"}`, in place of the `text` sentence. `include/EventText.h` lists
every kind, its fields and the age units; it is the contract between the C++ that builds the records and the two
places that word them. `SkyrimNetEvents::FormatEventsText` became `DescribeEvents`, and `FormatRelativeGameTime` and
`FormatRelativeGameDuration` became one `RelativeGameTime` that returns the bucket. `CombatEventLog`,
`WeatherEventLog` and `TravelEventLog` emit records, and so do the merge's condensed combat entry and idle marker.
Condensation stays in C++: a run of hits becomes a `combat_summary` record of fights and damage, and a run of moves a
`journey` or `visit` record.

**Who words the records.** The Director's prompts word them in English through the new
`components/narrative_engine/event_timeline.prompt`. The dashboard words them in the player's language through the
new core module `EventText`, which renders the `[Event]`, `[Event.Age]`, `[Event.Duration]`, `[Event.SkyrimNet]`,
`[Event.Combat]`, `[Event.Weather]` and `[Event.Travel]` sections of `english.ini`. The event history log uses the
same module with the English table. A line another plugin wrote (an unknown event type whose data is a sentence)
stays as it came.

**Two departures from the plan, both forced by `render_template` taking no arguments.** An included file renders
against its caller's whole context, so it cannot be handed one event to word.

- The include is `event_timeline.prompt`, not `event_line.prompt`. It renders the whole `recent_events` list, which
  both prompts already pass by that name, so each prompt replaced its own loop with one `render_template` call.
- Memory and dialogue ages are worded inline in each prompt that shows them: `action_select`'s two sender lists,
  and both compose prompts' memory and dialogue lists. The plan had the include word them, which no file can do for
  a row it is not given. The rows' `age` is now the same bucket. Sender dialogue's `age_str` became `age`, with the
  unit `unknown` where today's code wrote "recent".

**Verification, in two passes.** Pass 1 moved every renderer's wording, still as C++ literals, into `EventText`
reading the new records. The existing assertions on rendered text kept their expected strings unchanged, and they
passed. That covers every SkyrimNet kind, every condensation rule, the idle marker and every age bucket. A new pin
table was then added in `src/EventText.test.cpp`: 65 records, covering every kind, every shape a kind's fields can
take, both condensation shapes (fights and damage, alone and mixed) and every age bucket, each beside its line.
Pass 2 replaced the literals with the `[Event.*]` templates. The table then failed in exactly two cases, the named
difference: a list of three or more names (a travel party, or the places a visit took in) is joined as `[Grammar]`
joins every list, without a comma before "and". The two cases now hold the new line, plus a `prompt` field with the
old one, which the prompts keep.

**The render comparison.** The scratchpad harness rendered six prompts, before and after. `story_eval` and
`action_select` each received all 65 records as their timeline. `action_select` and both compose prompts, with and
without a motivating memory, received memory rows in every age bucket and dialogue rows in every bucket plus
`unknown`. The "before" side rendered the snapshotted prompt tree against the old sentences, taken from the pin
table's lines (or the `prompt` field where it has one). **All six renders are byte-identical**, so there is no
whitespace difference to name. The include's layout depends on two Inja rules, read from its lexer:

- `-%}` eats every space and newline after it.
- `{%-` clears only an all-blank final line, and keeps the newline before it.

So each branch's words sit on one line, and that line's newline ends the entry. Older Inja may evaluate both sides of
an `and`, so a field only some kinds carry is read under an `if` on the kind. `CUSTOM_PROMPTS.md` records both.

**Left for later, and why.**

- `TravelEventLog::FormatHoldName` still appends " Hold" to a hold's name unless the name already ends with it.
- `CombatEventLog` still lowercases an activator's name.

Both shape a name, not a sentence, and both are English matching rules, which is Phase D's subject. The Director
pipeline's drop of empty events now keys on the `no_data` kind instead of on text ending in "(no data)". The one
behaviour change is that a third-party sentence which merely ends with those words is no longer dropped.

`pwsh -File build.ps1 test`: 557 of 557 passed. `build.ps1 build` is clean, and the new include deploys with the
build.

#### Step 12 — Plot menus, plans and verb phrasing in prompts

- [x] Complete

**[CLAUDE]**

**Goal:** The plot prompts phrase their own menus, plans and verbs.

The prompt-facing half of `kVerbs` moves to `components/narrative_engine/plot_verbs.prompt`: menu lines, plan
sketches, slot `says` text and task phrases. Its callers pass verb ids and filled slots instead of text. So do:

- `PlotPlanning`'s option lines, purposes, outcome phrases, `StepsSoFar` and `PlanText`;
- `PlotCandidates::PersonLine` and its relation phrases;
- `PlotBirth::StandingPhrase`.

The decision templates and `plot_ambition` / `plot_cast` consume the structured fields. After this step `kVerbs`
holds only ids, slots and traits.

**Verification:** offline render comparison of every plot prompt and decision template, over a synthetic plot at
birth, mid-plan, and at adaptation, with every verb represented somewhere.

Done. Every step a plot prompt shows is now a record, and the prompts word it. The record is `{verb, fill, open,
agent_open}`:

- `fill` holds each filled placeholder.
- `open` says an unfilled slot shows as `[mark]`, for a step still being planned. Otherwise it reads as someone,
  somewhere or something, as a step that has happened, or a verb on a menu, does.
- `agent_open` says the same of the agent alone.

`PlotPlanning::StepRecord` builds it in three forms, `Planned`, `Done` and `Menu`, which are today's sketch, label
and generic sentence. One file words all three.

**The prompt files.** All are under `components/narrative_engine/`:

- `plot_step.prompt` words a step. It has one branch per verb, generated once from `kVerbs` by a scratchpad script.
- `plot_role.prompt` words what each role of a verb is for, which is the old slot `says`.
- `plot_done.prompt` words a step that has happened, with how it went.
- `plot_purpose.prompt` words what a slot's step is for in the plan.
- `plot_standing.prompt` words a faction standing token.

The templates reach them through `{% set %}` and `{% include %}`, the mechanism recorded as resolved question 7.

**What the callers pass now.**

- `PlotPlanning`'s option lines are records: a preparation carries its `before`, and the adapt options carry only
  their keys.
- Its purposes are a `kind` with fields, and its outcome phrases are `PlotModel::StepOutcomeId` tokens.
- `StepsSoFar` and `PlanText` are lists of records, which the templates number.
- `PlotCandidates::PersonLine` became a `Why` (occupation, a `RelationId` token, where a stranger lives, whether the
  scheme names them), which `plot_slot.prompt` words. `PlotMenus::RelationPhrase` is gone.
- `PlotBirth::StandingPhrase` became `StandingId`: `leads`, `senior`, `member`.
- `kVerbs` lost its dashboard sentence, and `PlotVerbs::Generic` and `RenderStep` went with it.
- `PlotModel::Label` remains for the plot log and for casting's word matching. It now renders the English
  `[Plot.Dashboard]` line, which a PlotText test had already pinned equal to the one removed.

**The comparison.** The scratchpad harness gained Inja's include callback, resolving paths under `prompts/` as
SkyrimNet does. The contexts are the real ones the planner builds: `PlotPlanning` was instrumented for the
comparison only, behind an environment variable, and the instrumentation was removed before the commit. It dumped
every context it handed its `Ask` across the plot test suite, 238 of them, once from the old code and once from the
new.

A conversion script turns each new context back into the old strings by the old C++ rules. It agreed with the old
code's dump on all 238, matched as multisets because the two test runs ordered their cases differently. It then
built 24 synthetic contexts for what the tests do not reach:

- every verb filled, done and on a menu, including the nine the tests never fill;
- every outcome, and an empty slot in a done step;
- every purpose, place and thing slots, every relation, a named stranger, and the mastermind's own option;
- every standing, and both birth prompts.

Every context, real and synthetic, was rendered twice: the old prompt tree against its old strings, and the new tree
against its record. **All 262 renders are byte-identical.**

**Left for the steps that remove their last readers.** Plot awareness, which Step 14 converts, still reads
`kVerbs`' two task phrases through `RenderPhrase`. The retry messages that `PlotBirthParse` and
`PlotCandidates::ResolveRoles` compose, which Step 13 turns into codes, still read the slot `says`. Both stay in
C++ until then, and the prompts carry their own copy of `says` meanwhile. `caption` stays as well. It is the
dashboard's fallback and `PlotVerbs::Plain`'s word for an id, a matching rule, which is Phase D's subject.

`pwsh -File build.ps1 test`: 557 of 557 passed. `build.ps1 build` is clean, and the five includes deploy with the
build. The planning tests keep their expected sentences: their helper rebuilds each from the record and the English
dashboard line, so they now also check that the records carry the right fills. A PlotText test fails the build when
a verb or a role has no branch in `plot_step.prompt` or `plot_role.prompt`.

#### Step 13 — Retry and rejection codes

- [ ] Complete

**[CLAUDE]**

**Goal:** A rejected answer tells the model why in the prompt's words, not C++'s.

- Every rejection in `PlotBirthParse` and `PlotCandidates` becomes a code plus arguments.
- `ctx["retry"]` carries them, and `plot_cast` / `plot_ambition` word each code they can receive.
- `PlotLog` records the code and its arguments.
- `kVerbs`' slot `says` goes, since those messages are its last C++ readers. Step 12 left it for them, and the
  prompts already word it in `plot_role.prompt`.

**Verification:**

- Offline render comparison of both prompts under every code.
- A `*.test.cpp` asserting that every code the parsers can return is one the receiving prompt handles, so a new code
  without wording fails the build's tests rather than reaching the model as nothing.

#### Step 14 — `ne_plot_awareness` returns structure

- [ ] Complete

**[CLAUDE]**

**Goal:** The plot-awareness submodule writes its own sentences.

- `PlotAwareness::Describe` returns JSON: the role, the scheme, the steps and their states for a mastermind, or the
  task and its sender for an agent.
- `0300_ne_plot_awareness.prompt` indexes into it and words it.
- An NPC in no plot still renders nothing.
- `kVerbs`' two task phrases go, with `PlotVerbs::RenderPhrase` and `Unfilled`: plot awareness was their last
  reader, which Step 12 left them for.

**Verification:** offline render comparison of the submodule for a mastermind at each step state, an agent, and an
NPC in no plot. The harness applies SkyrimNet's parse-as-JSON rule, which is the behaviour established by
disassembly. Step 29 confirms it in the running game.

#### Step 15 — Remaining prompt prose, and the audit

- [ ] Complete

**[CLAUDE]**

**Goal:** No LLM-bound prose is left in C++.

- `GossipContent::BandLengthSpec` passes `lo`/`hi`, and `gossip_seed` words the range.
- `DecisionProbe`'s sentences move to its prompt.
- `story_eval`'s `decision_log_tail` passes each record's failure and note *codes*, and the prompt words them. Step 8
  left them rendered through the English table's `[Decision]`, so the prompt stayed byte-identical meanwhile.
- Then audit: every string literal in `src/` and `include/` that reaches a prompt context, a decorator return or a
  memory.

**Verification:**

- Offline render comparison of `gossip_seed` and `probe`.
- The audit's result is recorded in this document: each remaining literal and why it may stay (a token, a log line,
  a key name). The expected count of LLM-bound sentences is zero.

### Phase D — Language-aware matching

#### Step 16 — The matching engine

- [ ] Complete

**[CLAUDE]**

**Goal:** The shared vocabulary under **Validators that read content**, as a pure module.

- `.Reject`, `.Require` and `.Match` lists at start, end and anywhere.
- Word-sequence entries, with `*` patterns.
- `WordSeparated = false` character-sequence matching.
- `Name.Suffixes`.
- `Disabled = <reason>`.
- The loader parses `[Matching]`, and the census names disabled checks.

**Verification:** `*.test.cpp` built from the design's own examples:

- the English scheme list;
- German `zu *` and `*zu*`, including the *zurück* false positive the doc admits;
- the Japanese scheme rules;
- the French and German ghost rules;
- *Drachenbluts* against `drachenblut*`.

The shipped-files test gains the rule that every check has rules or a `Disabled` entry.

#### Step 17 — Content validators on `[Matching]`

- [ ] Complete

**[CLAUDE]**

**Goal:** The five content validators read their rules from the active table.

- `SchemeOpensWithVerb`, `NamesThePlayer` (title and name halves), `IsGhostRecord`, title stripping and `SameLine`
  run on the matching engine and Step 1's folding.
- `english.ini`'s `[Matching]` reproduces today's word lists exactly.
- Each validator takes the table as a parameter, for the trial.

**Verification:** a characterization test of all five over today's cases, first passed against unmodified code, then
unchanged. New cases cover non-English inputs under synthetic tables. Today's three-*byte* name minimum becomes
three code points, so a two-letter Cyrillic name is now correctly too short. That is the one intended English-table
behaviour change, and it is listed in the write-up.

#### Step 18 — Name matchers in the player's script

- [ ] Complete

**[CLAUDE]**

**Goal:** Role names and named-in-text checks work in every script.

- `PlotCandidates::Closest` splits words with Step 1's splitter. It treats capitalization as a signal only in cased
  scripts, and matches whole names as substrings when `WordSeparated = false`.
- `PlotCasting::ContainsWord` allows the table's `Name.Suffixes`.
- Both take the table as a parameter.

**Verification:**

- A characterization test of both over today's English cases, first passed against unmodified code, then unchanged.
- New cases use real localized names taken from the Spriggit export's Russian, Japanese, Chinese, Polish and German
  strings. For each, a role naming the person resolves to them. A genitive (*Veleths*, *Ульфрика*) counts as naming
  them. A name inside a longer word does not.

### Phase E — Output language

#### Step 19 — The output-language line and `[Prompt.Form]`

- [ ] Complete

**[CLAUDE]**

**Goal:** Every content-producing prompt asks for the active language, and only for its content fields.

1. Confirm the provisional table under **The output-language line**, field by field, by tracing where each field
   ends up. Correct the table in this document before writing code.
2. Add `output_language`, `content_fields` and the `[Prompt.Form]` values to each content-producing context.
3. Add the shared include, rendered only when the language is not English.

**Verification:**

- Offline render comparison of every affected prompt under English: identical to before.
- A German-table rendering of each, recorded in the write-up, showing the line names exactly the content fields and
  carries the German form.

### Phase F — Surfaces

#### Step 20 — The dashboard formatter, and the shared fixture

- [ ] Complete

**[CLAUDE]**

**Goal:** The dashboard formats exactly as C++ does.

- A TypeScript port of `TextTemplate`'s rendering, for the dashboard's own strings. Plural categories come from
  `Intl.PluralRules`, with every plural falling back to `other` if `Intl` is unavailable (see **Dashboard** above).
- A committed fixture of template, arguments, grammar and expected output.
- Both sides read the fixture: C++ through a `*.test.cpp`, and TypeScript through a new `npm test` (compile the
  formatter with `tsc`, run the fixture under `node --test`).

**Verification:** both test runs pass on the same fixture. The fixture covers every form and every plural family.
Because the TypeScript side takes its categories from Node's CLDR data, the shared fixture also checks the C++
hand-written families against CLDR.

#### Step 21 — Dashboard strings

- [ ] Complete

**[CLAUDE]**

**Goal:** The dashboard shows no English that does not come from the language file.

- Every literal in `dashboard/src` becomes a `t()` key, and `english.ini` gains `[Dashboard]`.
- C++ sends the table with the initial state.
- C++ sends ids for phases, beats and outcomes, and the dashboard labels them itself.
- `<html lang>` follows `Code`.
- The status banner shows Step 4's font mismatch warning.

**Verification:**

- The bundle builds.
- A throwaway script confirms two things: every `t()` key exists in `english.ini`, and no JSX text node or label
  attribute outside a `t()` call contains a letter.
- The `plots.sample.json` fixture still renders through the dashboard's dev entry point.

#### Step 22 — MCM translation files and the language setting

- [ ] Complete

**[CLAUDE]**

**Goal:** The MCM page is translatable, and offers the language override.

- `config.json` switches to `$NE_…` keys.
- `Interface/Translations/NarrativeEngine_ENGLISH.txt` ships in UTF-16 LE with a BOM, as `$KEY<TAB>text`.
- A *Language* dropdown offers *Auto* plus the shipped languages, bound to `[Localization] sLanguage`.

**Verification:** a throwaway check that every `$` key in `config.json` is defined in the translation file. Its
encoding, BOM and line format must match a translation file from another mod installed in the modlist that is known
to work in game.

#### Step 23 — Manifest languages, and the configure check

- [ ] Complete

**[CLAUDE]**

**Goal:** SkyrimNet's hub lists what ships, and the repo cannot drift from it.

- `manifest.json` gains `languages`.
- The configure step fails unless that array equals the set of `Code` values under `Languages/`.

**Verification:** configure succeeds with `["en"]`. A throwaway mismatch makes it fail with a message naming both
sets, and is then reverted.

### Phase G — The other eight languages

#### Step 24 — French, German, Italian, Spanish and Polish drafts

- [ ] Complete

**[CLAUDE]**

**Goal:** The five Latin-script languages ship as drafts.

For each language:

- **A language file**, with `Status = draft` and the correct `Fonts`.
- **Every declared key translated.** Gender and plural selects are used where the language needs them.
- **`[Matching]` rules for every check**, each term citing the Spriggit record it was taken from.
- **`[Prompt.Form]`** for every spliced field.
- **An MCM translation file.**

`manifest.json` gains the five codes.

**Verification:**

- The shipped-files test passes with zero rejections, and every check has rules or a reasoned `Disabled`.
- A throwaway script confirms every cited record exists in the export and that its string in that language contains
  the cited term.
- Configure passes the manifest check.

#### Step 25 — Russian, Japanese and Chinese drafts

- [ ] Complete

**[CLAUDE]**

**Goal:** The remaining three ship as drafts.

As Step 24, for the three scripts that most test the design:

- Russian's three-way plurals and case-ending suffixes;
- Japanese and Chinese's `WordSeparated = false` and verb-final rules;
- Chinese's empty plural family.

**Verification:** as Step 24.

#### Step 26 — The README

- [ ] Complete

**[CLAUDE]**

**Goal:** Players know what works, what does not, and how to change it.

- The language Q&A is rewritten: detection, the override, the overlay file, and draft status.
- The font fix for an override whose script the game's fonts do not cover.
- The known gaps: SkyrimNet's own UI text, the MCM following the game's language, and English-only memory retrieval.
- The advice not to edit our prompts for another language is replaced.

**Verification:** `format.ps1`. Every setting, path and file name the README gives is checked against the repo.

### Phase H — In-game checks

#### Step 27 — The *Run checks* button

- [ ] Complete

**[CLAUDE]**

**Goal:** One button settles everything about real SkyrimNet rendering and real engine text that offline
verification cannot.

The button does three things:

- **Renders a check prompt.** `narrative_engine_render_check` loads a sentinel file through a *computed*
  `render_template` name, reads a field out of a check decorator's JSON, and words a loop variable and a `{% set %}`
  variable through an `{% include %}`. It asks the model to repeat the three sentinels verbatim. The dashboard shows
  pass or fail for each, from the response and from whether SkyrimNet reported a render error.
- **Queues a notification** containing Latin-1 text the English font covers ("Café · Jörgen · naïve · Ærø ·
  Ölfrún"), for the user to read once the dashboard closes.
- **Shows the font-check result**, and whether the dashboard's `Intl.PluralRules` is available.

**Verification:** the build. Unit tests cover the response check: a response containing both sentinels passes, and
one missing either fails. The prompt passes the offline render comparison with SkyrimNet's rules stubbed.

#### Step 28 — The *Language trial* button

- [ ] Complete

**[CLAUDE]**

**Goal:** One button measures the drafts against the real model, as described under **The language trial**.

- `Localization` loads a read-only table on demand.
- Plot birth runs on the plot worker against an explicit table. Steps 17 and 18 already pass it.
- Each trial plot is discarded before it reaches `PlotState`.
- Eight births run for each of English, German, Russian and Japanese. That is roughly 100 LLM calls, which the button
  states before it runs.
- Results go to the dashboard and `NarrativeEngine_Plots.log`, per language:
  - births attempted and accepted;
  - rejections by code and by check;
  - distinct masterminds;
  - three accepted plots' memories, rendered in that language.

**Verification:**

- The build.
- An `*.engine.test.cpp` showing a trial birth never mutates `PlotState`, the occupancy table, the plot budget or the
  RNG stream position.
- An `*.engine.test.cpp` showing two tables can be read at once without touching the session's own.

#### Step 29 — Run checks

- [ ] Complete

**[USER]**

**Load a save, open the dashboard, press *Run checks*, wait for it to report, then close the dashboard.**

**What to judge:**

- All three render checks say **pass**.
- The notification after closing reads "Café · Jörgen · naïve · Ærø · Ölfrún" exactly, with no blanks, boxes or
  `Ã©`-style mangling.
- The font check says the active font set covers English, and `Intl.PluralRules` is reported available. If it is
  not, the dashboard's plurals all read as `other`. That is cosmetic, and a follow-up ports the C++ families.

A failed computed-name check switches Step 10's include to an `if` chain over the beat set, as a follow-up step
inserted here. A failed decorator check means `ne_plot_awareness` returns prose again, rendered from a template file
through `TextTemplate`. A failed include check means the plot includes are inlined into each template that uses
them, which a script can generate from the include files.

#### Step 30 — Language trial

- [ ] Complete

**[USER]**

**Load a save, open the dashboard, press *Language trial*, and wait for it to finish**, which takes a few minutes.
Then send the dashboard's result, or `NarrativeEngine_Plots.log`.

**What to judge:** nothing by eye; the numbers decide. A language passes when:

- its acceptance rate is within a third of English's from the same run;
- no single check rejects more than a quarter of its births;
- no role resolves to "names nobody" for a person the model copied correctly from the menu.

Anything outside that is a draft-rule problem. Fixing it is a follow-up `[CLAUDE]` step, and the trial is re-run.
