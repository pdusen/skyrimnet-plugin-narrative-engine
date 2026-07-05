# Sanitizing LLM-returned strings before use

LLMs frequently return Unicode characters that are valid English typography
but cause problems when used as visible text in Skyrim — either because the
game's bundled fonts don't include the glyphs, or because engine fields are
ASCII-only byte buffers that silently drop or garble multi-byte sequences.
Curly quotes, em-dashes, ellipsis characters, non-breaking spaces, and
zero-width formatting characters are routine offenders.

**Every free-form content string we accept from any LLM MUST pass through
`NarrativeEngine::LLMTextSanitizer::Sanitize(...)` before it's saved to
co-save, written to a `RE::TESForm` field, displayed in a UI, fed into
another LLM call, persisted to the SkyrimNet memory store, or used to
populate any downstream field.** Sanitization happens at the point of
extraction from the response JSON — wrap the `get<std::string>()` call, do
not defer.

The header is `include/LLMTextSanitizer.h`; the implementation is
`src/LLMTextSanitizer.cpp`. The function takes a `std::string_view` and
returns a fresh `std::string` with surrounding whitespace stripped.

## Policy: transform a small list, pass everything else through

The sanitizer targets a **fixed list** of codepoints that LLMs emit as
typographic noise, invisible formatting, or smart-character variants. Any
codepoint NOT on the list is passed through verbatim — including accented
Latin letters (`à`, `é`, `ñ`, `ö`, `ü`), Latin ligatures (`æ`, `œ`, `ß`,
`þ`, `ð`), language-native punctuation (French / German / Russian
guillemets `«` `»`), and characters from any other script (Cyrillic, Greek,
Hebrew, Arabic, CJK). This is deliberate: players running Skyrim in
non-English locales should keep their letters intact. We filter LLM
artifacts; we don't romanize the text.

## What it does

| Input class | Action |
| --- | --- |
| Pure ASCII printable + `\t` / `\n` / `\r` | Pass through unchanged |
| Other ASCII control bytes (`0x00..0x1F` except `\t \n \r`, plus `0x7F`) | Drop |
| Invisible / zero-width / bidirectional formatting (`U+00AD`, `U+200B..F`, `U+2028..E`, `U+2060`, `U+FEFF`) | Drop |
| Horizontal bar `U+2015` `―` | Drop (long line glyph that has no good ASCII clause-separator substitute) |
| Non-breaking and exotic spaces (`U+00A0`, `U+2002..A`, `U+202F`, `U+205F`, `U+3000`) | Replace with ASCII `' '` |
| Short dashes (hyphen `U+2010`, non-breaking hyphen `U+2011`, figure dash `U+2012`, en-dash `U+2013`, math minus `U+2212`) | Replace with `'-'` |
| Em-dash `U+2014` `—` | Replace with `'--'` (per house style — em-dashes consistently render badly in inventory tooltips and dialogue; the doubled-hyphen convention reads as an em-dash to an ASCII-era reader without shifting the sentence's grammar) |
| Ellipsis `U+2026` `…` | Replace with literal `"..."` |
| Smart double quotes (`U+201C..F`) | Replace with `'"'` |
| Smart apostrophes (`U+2018..B`, `U+2032` prime) | Replace with `'\''` |
| Bullet / list markers (`U+2022`, `U+2023`, `U+2043`, `U+25E6`) | Replace with `'-'` (preserves visible list structure when the LLM emits markdown bullets) |
| Invalid UTF-8 sequences | Drop the offending byte |
| All other non-ASCII codepoints | **Pass through verbatim** |
| Leading + trailing whitespace on the final string | Trimmed |

To add a substitution for a newly-identified problematic codepoint, add a
case to `ReplaceCodepoint` in `src/LLMTextSanitizer.cpp` and a row above.
The default case (`std::nullopt`) handles passthrough; no plumbing change
needed.

## Why em-dash → `--`

Em-dash is rejected harder than other Unicode punctuation because (a) the
glyph itself is absent from many of Skyrim's bundled fonts, so the
rendered character is often the empty-box tofu rather than a fallback
hyphen, and (b) when the LLM uses it as a clause separator (the common
case in flowery prose), replacing it with a single hyphen produces
ambiguous output (`"I went to market - the stall was closed"` reads as
a list item rather than an em-dash clause break). The doubled-hyphen
`--` is the standard ASCII-era convention for an em-dash — it preserves
the clause-separator visual and doesn't reshape the sentence's grammar
the way substituting a semicolon would.

`U+2015` HORIZONTAL BAR doesn't get the same treatment because it isn't
the same character class — it's a long horizontal rule used for quotation
attribution in some languages, not a sentence-internal clause separator.
Substituting `--` would be grammatically wrong; substituting `-` would be
visually wrong (it's much longer than a hyphen). Dropping it altogether
is the least-bad option.

## Why not romanize accented Latin / non-ASCII letters

An earlier iteration of this filter stripped accents (`café` → `cafe`,
`résumé` → `resume`) and expanded ligatures (`Æ` → `AE`). That hurts
players running Skyrim in French / German / Spanish / Russian / etc., who
expect their localized text — including any LLM-generated content the
plugin produces in their language — to render with the correct letters.
Skyrim's bundled fonts cover the common European character ranges that
those localizations ship for, so the engine handles these characters fine
on its own. Romanization here would only force English orthography onto
non-English text for no benefit. So: pass them through.

If a future audit finds Skyrim *doesn't* render a particular non-ASCII
letter correctly in some context (e.g. a specific font or menu surface),
add a targeted substitution case for that codepoint — don't blanket-drop
everything outside ASCII.

## Why not normalize via a library

`uni-algo`, `utf8proc`, and ICU all offer Unicode normalization forms
(NFKD / NFKC) that would handle most of the Latin-letter cases by
decomposing accented characters into base + combining marks. We don't
want that decomposition (per the previous section). None of those
libraries also implement our opinionated substitutions — em-dash →
`--`, ellipsis → three ASCII dots, smart-quote variants → straight
ASCII — so we'd end up with custom post-processing on top of any
library, doubling the code paths. Hand-rolling the whole pass keeps
everything in one switch statement and stays dependency-free.

## Adding new call sites

When you add a new LLM-driven feature:

1. Identify which fields of the response JSON are free-form content
   (anything that's not a known-vocabulary enum, a numeric, or an
   identifier you validate against a closed set).
2. Wrap each `get<std::string>()` for those fields in
   `LLMTextSanitizer::Sanitize(...)`. The wrap happens at extraction
   time — before the value is stored, resized, persisted, displayed,
   or passed onward.
3. If your feature also writes the value to a co-save record or a
   `RE::TESForm` field, the sanitization at extraction time covers it
   transitively. Do not re-sanitize on the way out.
4. If you're tempted to skip sanitization because "the value is short
   and validated" — sanitize anyway. The cost is microseconds; the
   debugging cost of a smart-quote slipping into a player-visible book
   title is much higher.

See `CLAUDE.md` for the project-wide rule.
