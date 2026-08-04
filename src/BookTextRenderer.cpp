#include <BookTextRenderer.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace NarrativeEngine::BookTextRenderer
{
    namespace
    {
        constexpr std::string_view kPageBreakToken = "[pagebreak]";

        // Local ASCII classifiers rather than <cctype> — the <cctype>
        // functions are locale-sensitive and UB for negative `char`
        // values, both of which bite on arbitrary UTF-8 book text.
        constexpr bool IsAsciiAlpha(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        }

        constexpr bool IsAsciiDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        constexpr bool IsAsciiAlnum(char c)
        {
            return IsAsciiAlpha(c) || IsAsciiDigit(c);
        }

        constexpr bool IsAsciiSpace(char c)
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        }

        constexpr char LowerAscii(char c)
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }

        // Element names that occupy vertical space in the rendered page, and
        // so leave a newline behind when stripped. Everything not on this
        // list is inline styling and vanishes without a trace.
        //
        // `img` is here because Skyrim's book renderer lays illustrations out
        // as blocks — the vanilla Crafting Manuals put each plate on its own
        // line. The list is broader than what vanilla books actually use
        // (mod-added books are hand-written HTML of varying pedigree), which
        // costs nothing: surplus newlines collapse during normalization.
        bool IsBreakTag(std::string_view name)
        {
            static constexpr std::array<std::string_view, 22> kBreakTags = {
                "blockquote", "br", "center", "dd",  "div", "dl", "dt", "h1",  "h2",    "h3", "h4",
                "h5",         "h6", "hr",     "img", "li",  "ol", "p",  "pre", "table", "tr", "ul",
            };
            return std::find(kBreakTags.begin(), kBreakTags.end(), name) != kBreakTags.end();
        }

        void AppendUTF8(std::uint32_t cp, std::string& out)
        {
            const auto emit = [&out](std::uint32_t byte) {
                out.push_back(static_cast<char>(static_cast<unsigned char>(byte)));
            };
            if (cp < 0x80) {
                emit(cp);
            } else if (cp < 0x800) {
                emit(0xC0 | (cp >> 6));
                emit(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                emit(0xE0 | (cp >> 12));
                emit(0x80 | ((cp >> 6) & 0x3F));
                emit(0x80 | (cp & 0x3F));
            } else {
                emit(0xF0 | (cp >> 18));
                emit(0x80 | ((cp >> 12) & 0x3F));
                emit(0x80 | ((cp >> 6) & 0x3F));
                emit(0x80 | (cp & 0x3F));
            }
        }

        // Parse an unsigned integer in the given base. Returns false on an
        // empty body, a bad digit, or overflow past the Unicode range.
        bool ParseCodepoint(std::string_view digits, unsigned base, std::uint32_t& out)
        {
            if (digits.empty())
                return false;
            std::uint32_t value = 0;
            for (const char c : digits) {
                unsigned digit = 0;
                if (IsAsciiDigit(c)) {
                    digit = static_cast<unsigned>(c - '0');
                } else if (base == 16 && c >= 'a' && c <= 'f') {
                    digit = static_cast<unsigned>(c - 'a' + 10);
                } else if (base == 16 && c >= 'A' && c <= 'F') {
                    digit = static_cast<unsigned>(c - 'A' + 10);
                } else {
                    return false;
                }
                if (digit >= base)
                    return false;
                value = value * base + digit;
                if (value > 0x10FFFF)
                    return false;
            }
            out = value;
            return true;
        }

        // Scan the markup construct starting at `start` (which must be '<').
        //
        // On success `nameOut` holds the lowercased element name — empty for
        // comments / doctypes / processing instructions, which carry no
        // renderable content — and `consumedOut` the byte length of the whole
        // construct. On failure nothing is written and the caller emits the
        // '<' as literal text.
        //
        // A '<' that isn't followed by a letter, '/', or '!' is not a tag:
        // book prose containing "a < b" must survive. An *unterminated*
        // construct is likewise rejected rather than consumed to end-of-input
        // — dropping the tail of a book to salvage a truncated tag loses far
        // more than the stray angle bracket costs.
        bool TryScanTag(std::string_view s, std::size_t start, std::string& nameOut, std::size_t& consumedOut)
        {
            const std::size_t n = s.size();
            if (start + 1 >= n || s[start] != '<')
                return false;

            nameOut.clear();
            std::size_t i = start + 1;

            if (s[i] == '!') {
                if (s.compare(i, 3, "!--") == 0) {
                    const auto close = s.find("-->", i + 3);
                    if (close == std::string_view::npos)
                        return false;
                    consumedOut = (close + 3) - start;
                    return true;
                }
                const auto close = s.find('>', i);
                if (close == std::string_view::npos)
                    return false;
                consumedOut = (close + 1) - start;
                return true;
            }

            if (s[i] == '/')
                ++i;

            if (i >= n || !IsAsciiAlpha(s[i]))
                return false;

            while (i < n && IsAsciiAlnum(s[i])) {
                nameOut.push_back(LowerAscii(s[i]));
                ++i;
            }

            // Walk to the closing '>', honouring quoted attribute values so a
            // '>' inside e.g. src='img://...' can't terminate the tag early.
            char quote = '\0';
            while (i < n) {
                const char c = s[i];
                if (quote != '\0') {
                    if (c == quote)
                        quote = '\0';
                } else if (c == '\'' || c == '"') {
                    quote = c;
                } else if (c == '>') {
                    consumedOut = (i + 1) - start;
                    return true;
                }
                ++i;
            }
            return false;
        }

        // Decode the HTML entity starting at `start` (which must be '&').
        // Writes the replacement text to `out` and the construct's byte
        // length to `consumedOut`. Returns false for anything unrecognized,
        // leaving the '&' to be emitted literally.
        bool TryDecodeEntity(std::string_view s, std::size_t start, std::string& out, std::size_t& consumedOut)
        {
            // Named entities top out well under this; the cap keeps a bare
            // '&' in prose from scanning the rest of the book for a ';'.
            constexpr std::size_t kMaxEntityLength = 12;

            const std::size_t n = s.size();
            if (start >= n || s[start] != '&')
                return false;

            const std::size_t limit = std::min(n, start + kMaxEntityLength);
            std::size_t semi = std::string_view::npos;
            for (std::size_t i = start + 1; i < limit; ++i) {
                if (s[i] == ';') {
                    semi = i;
                    break;
                }
                if (s[i] == '&' || s[i] == '<' || IsAsciiSpace(s[i]))
                    break;
            }
            if (semi == std::string_view::npos)
                return false;

            const std::string_view body = s.substr(start + 1, semi - start - 1);
            if (body.empty())
                return false;

            out.clear();

            if (body.front() == '#') {
                const bool hex = body.size() > 2 && (body[1] == 'x' || body[1] == 'X');
                const std::string_view digits = hex ? body.substr(2) : body.substr(1);
                std::uint32_t cp = 0;
                if (!ParseCodepoint(digits, hex ? 16u : 10u, cp) || cp == 0)
                    return false;
                AppendUTF8(cp, out);
                consumedOut = (semi + 1) - start;
                return true;
            }

            std::string name;
            name.reserve(body.size());
            for (const char c : body)
                name.push_back(LowerAscii(c));

            // The set books actually use, plus the typographic entities
            // mod authors reach for. Anything else falls through and is
            // emitted verbatim, which reads better than dropping it.
            static constexpr std::array<std::pair<std::string_view, std::string_view>, 16> kNamed = {{
                {"amp", "&"},
                {"apos", "'"},
                {"bull", "-"},
                {"gt", ">"},
                {"hellip", "..."},
                {"ldquo", "\""},
                {"lsquo", "'"},
                {"lt", "<"},
                {"mdash", "--"},
                {"ndash", "-"},
                {"nbsp", " "},
                {"quot", "\""},
                {"rdquo", "\""},
                {"rsquo", "'"},
                {"shy", ""},
                {"thinsp", " "},
            }};
            for (const auto& [key, value] : kNamed) {
                if (name == key) {
                    out.assign(value);
                    consumedOut = (semi + 1) - start;
                    return true;
                }
            }
            return false;
        }

        bool MatchesPageBreak(std::string_view s, std::size_t start)
        {
            if (start + kPageBreakToken.size() > s.size())
                return false;
            for (std::size_t k = 0; k < kPageBreakToken.size(); ++k) {
                if (LowerAscii(s[start + k]) != kPageBreakToken[k])
                    return false;
            }
            return true;
        }

        // Whitespace normalization, standing in for what a layout engine
        // does with the author's slack: collapse runs of spaces/tabs to one
        // space, cap runs of newlines at two (one blank line), trim every
        // line, and trim the whole string. Vanilla books stack four or five
        // `<p>` tags to push a heading down the page — without this the
        // output would be mostly empty lines.
        std::string NormalizeWhitespace(const std::string& raw)
        {
            std::string out;
            out.reserve(raw.size());

            int pendingNewlines = 0;
            bool pendingSpace = false;
            bool anyOutput = false;

            for (const char c : raw) {
                if (c == '\r')
                    continue;
                if (c == '\n') {
                    ++pendingNewlines;
                    pendingSpace = false;
                    continue;
                }
                if (c == ' ' || c == '\t' || c == '\f' || c == '\v') {
                    if (anyOutput && pendingNewlines == 0)
                        pendingSpace = true;
                    continue;
                }

                // A real character: flush whatever separator the run of
                // whitespace before it earned. Leading whitespace earns
                // nothing, which is what trims the front of the string.
                if (anyOutput) {
                    if (pendingNewlines > 0) {
                        out.append(static_cast<std::size_t>(std::min(pendingNewlines, 2)), '\n');
                    } else if (pendingSpace) {
                        out.push_back(' ');
                    }
                }
                pendingNewlines = 0;
                pendingSpace = false;
                anyOutput = true;
                out.push_back(c);
            }

            // Trailing whitespace is never flushed, so it's already gone.
            return out;
        }
    } // namespace

    std::string RenderToPlainText(std::string_view markup)
    {
        std::string raw;
        raw.reserve(markup.size());

        std::string scratch;
        const std::size_t n = markup.size();
        std::size_t i = 0;

        while (i < n) {
            const char c = markup[i];

            if (c == '<') {
                std::size_t consumed = 0;
                if (TryScanTag(markup, i, scratch, consumed)) {
                    if (IsBreakTag(scratch))
                        raw.push_back('\n');
                    i += consumed;
                    continue;
                }
            } else if (c == '&') {
                std::size_t consumed = 0;
                if (TryDecodeEntity(markup, i, scratch, consumed)) {
                    raw.append(scratch);
                    i += consumed;
                    continue;
                }
            } else if (c == '[' && MatchesPageBreak(markup, i)) {
                // A page turn is a harder break than a paragraph — give it a
                // blank line so the sections stay visually distinct.
                raw.append("\n\n");
                i += kPageBreakToken.size();
                continue;
            }

            raw.push_back(c);
            ++i;
        }

        return NormalizeWhitespace(raw);
    }
} // namespace NarrativeEngine::BookTextRenderer
