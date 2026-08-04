#pragma once

#include <string>
#include <string_view>

// Render Skyrim's book markup down to plain text.
//
// Why: Skyrim book records (`BOOK` / `TESObjectBOOK::GetDescription`) store
// their body as a Gamebryo/Scaleform pseudo-HTML dialect — `<p align='center'>`,
// `<font face='$HandwrittenFont'>`, `<img src='img://textures/...'>`,
// `<b>`, plus the engine's own `[pagebreak]` token. All of that exists to
// drive the in-game book renderer's layout. In an event-log line it is pure
// noise: SkyrimNet's `book_read` event hands us the raw markup verbatim, and
// a single illustrated book (Crafting Manuals is the case that surfaced this)
// contributes ~20KB of tag soup to whatever consumes it — enough on its own
// to blow out a Director evaluation prompt.
//
// What "render" means here: we approximate what the book *looks like* as
// closely as a plain string can. Concretely —
//   - Tags are stripped. Most (`font`, `b`, `i`, `a`, `span`, ...) leave
//     nothing behind; they only affect styling we can't reproduce.
//   - Tags that occupy vertical space in the rendered page (`p`, `br`,
//     `div`, `img`, `li`, headings, ...) leave a newline behind, so the
//     paragraph structure the author intended survives.
//   - `[pagebreak]` becomes a blank-line separator.
//   - HTML entities (`&amp;`, `&nbsp;`, `&#233;`, `&#x2014;`, ...) are
//     decoded to their actual characters.
//   - Whitespace is then normalized the way a layout engine would: runs of
//     spaces collapse to one, runs of blank lines collapse to at most one,
//     and every line plus the whole string is trimmed. Vanilla books stack
//     four or five `<p>` tags to push a title down the page; without this
//     the output would be mostly empty lines.
//
// The input is UTF-8 and the output is UTF-8. Bytes that aren't part of a
// tag, an entity, or the page-break token pass through untouched, so
// non-English book text survives intact. The function never throws, and
// malformed markup degrades gracefully — an unterminated tag is emitted as
// literal text rather than swallowing the remainder of the book.
//
// This is *not* an LLM-output sanitizer; book text comes from ESP records,
// not from a model. See `LLMTextSanitizer` for that separate, mandatory pass.
namespace NarrativeEngine::BookTextRenderer
{
    // Render `markup` to plain text. Returns an empty string for empty or
    // markup-only input.
    std::string RenderToPlainText(std::string_view markup);
} // namespace NarrativeEngine::BookTextRenderer
