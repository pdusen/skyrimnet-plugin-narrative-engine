#include <LLMTextSanitizer.h>

#include <cstdint>
#include <optional>

namespace NarrativeEngine::LLMTextSanitizer
{
    namespace
    {
        // Decode the next UTF-8 codepoint starting at byte index `i`.
        // Advances `i` past the consumed bytes (always at least 1 — invalid
        // lead bytes count as a single-byte consumption). Returns 0xFFFD
        // (Unicode replacement) for malformed sequences; the caller drops
        // 0xFFFD via the default case in ReplaceCodepoint.
        std::uint32_t DecodeUTF8(std::string_view bytes, std::size_t& i)
        {
            const auto end = bytes.size();
            if (i >= end) return 0;

            const auto first = static_cast<unsigned char>(bytes[i]);
            std::uint32_t cp = 0;
            int extra = 0;

            if ((first & 0x80) == 0) {
                cp = first;
                extra = 0;
            } else if ((first & 0xE0) == 0xC0) {
                cp = first & 0x1F;
                extra = 1;
            } else if ((first & 0xF0) == 0xE0) {
                cp = first & 0x0F;
                extra = 2;
            } else if ((first & 0xF8) == 0xF0) {
                cp = first & 0x07;
                extra = 3;
            } else {
                ++i;
                return 0xFFFD;
            }

            ++i;
            for (int k = 0; k < extra; ++k) {
                if (i >= end) return 0xFFFD;
                const auto next = static_cast<unsigned char>(bytes[i]);
                if ((next & 0xC0) != 0x80) return 0xFFFD;
                cp = (cp << 6) | (next & 0x3F);
                ++i;
            }
            return cp;
        }

        // Map a single codepoint to its substitution.
        //   - Returned string ""    : drop entirely.
        //   - Returned string non-"": emit the replacement bytes.
        //   - Returned nullopt      : codepoint is not on the transform list;
        //                             pass the original UTF-8 bytes through
        //                             verbatim (handled by the caller).
        //
        // Only codepoints that LLMs frequently emit as typographic / smart /
        // invisible noise are listed. Everything else (accented Latin
        // letters, Cyrillic, Greek, Hebrew, Arabic, CJK, language-native
        // punctuation like the French / German guillemets « », etc.) falls
        // through the default arm so non-English players keep their
        // language's normal letters and punctuation.
        //
        // See docs/LLM_RESPONSE_HANDLING.md for the source of truth on
        // policy; this switch is the implementation of that table.
        std::optional<std::string_view> ReplaceCodepoint(std::uint32_t cp)
        {
            switch (cp) {
                // -------- invisible / zero-width / formatting --------
                case 0x00AD: // soft hyphen
                case 0x200B: // zero-width space
                case 0x200C: // zero-width non-joiner
                case 0x200D: // zero-width joiner
                case 0x200E: // LTR mark
                case 0x200F: // RTL mark
                case 0x2028: // line separator
                case 0x2029: // paragraph separator
                case 0x202A: // LRE
                case 0x202B: // RLE
                case 0x202C: // PDF
                case 0x202D: // LRO
                case 0x202E: // RLO
                case 0x2060: // word joiner
                case 0xFEFF: // BOM / ZW no-break space
                case 0x2015: // horizontal bar (U+2015): drop entirely.
                             // Looks like a long line in most fonts; bare
                             // removal is preferable to substituting a
                             // sentence-altering punctuation mark.
                    return std::string_view{""};

                // -------- short dashes --------
                case 0x2010: // hyphen
                case 0x2011: // non-breaking hyphen
                case 0x2012: // figure dash
                case 0x2013: // en-dash
                case 0x2212: // minus sign
                    return std::string_view{"-"};

                // -------- em-dash --------
                // Per house style: render as '--' since that reads as an
                // em-dash to a Skyrim-era ASCII reader without shifting
                // grammar the way a semicolon would. U+2015 HORIZONTAL
                // BAR is handled in the drop group above — it appears as
                // a very long line rather than a clause separator, so
                // the '--' substitution doesn't suit it.
                case 0x2014:
                    return std::string_view{"--"};

                // -------- ellipsis --------
                case 0x2026:
                    return std::string_view{"..."};

                // -------- non-breaking + exotic spaces --------
                case 0x00A0: // NBSP
                case 0x2002: case 0x2003: case 0x2004: case 0x2005:
                case 0x2006: case 0x2007: case 0x2008: case 0x2009:
                case 0x200A: // hair space
                case 0x202F: // narrow NBSP
                case 0x205F: // medium math space
                case 0x3000: // ideographic space
                    return std::string_view{" "};

                // -------- smart double quotes --------
                // Guillemets « » (U+00AB, U+00BB) are intentionally absent:
                // they're standard quotation marks in French, German,
                // Russian, etc., not LLM "smart quote" artifacts. Pass.
                case 0x201C: case 0x201D: case 0x201E: case 0x201F:
                    return std::string_view{"\""};

                // -------- smart apostrophes --------
                case 0x2018: case 0x2019: case 0x201A: case 0x201B:
                case 0x2032: // prime
                    return std::string_view{"'"};

                // -------- bullet / list markers --------
                // These show up when the LLM uses markdown-style bullet
                // lists that we then render as plain text. Convert to '-'
                // so the list structure stays visible.
                case 0x2022: case 0x2023: case 0x2043: case 0x25E6:
                    return std::string_view{"-"};

                default:
                    return std::nullopt;
            }
        }

        bool IsAsciiWhitespace(unsigned char c)
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }
    }

    std::string Sanitize(std::string_view input)
    {
        std::string out;
        out.reserve(input.size());

        std::size_t i = 0;
        while (i < input.size()) {
            const auto byte = static_cast<unsigned char>(input[i]);
            if (byte < 0x80) {
                // ASCII fast path. Keep printable bytes (0x20..0x7E) plus
                // the three whitespace controls we allow through. All
                // other ASCII controls (including DEL 0x7F) get dropped.
                if ((byte >= 0x20 && byte < 0x7F) ||
                    byte == '\t' || byte == '\n' || byte == '\r') {
                    out.push_back(static_cast<char>(byte));
                }
                ++i;
                continue;
            }
            const std::size_t startByte = i;
            const std::uint32_t cp = DecodeUTF8(input, i);
            if (cp == 0xFFFD) {
                // Invalid UTF-8 sequence — drop the offending bytes
                // rather than propagate malformed input downstream.
                continue;
            }
            const auto replacement = ReplaceCodepoint(cp);
            if (replacement.has_value()) {
                out.append(*replacement);
            } else {
                // Codepoint isn't on our typographic-noise list. Pass
                // through the original UTF-8 bytes verbatim so non-
                // English letters and language-native punctuation
                // survive untouched.
                out.append(input.substr(startByte, i - startByte));
            }
        }

        // Trim trailing whitespace.
        while (!out.empty() &&
               IsAsciiWhitespace(static_cast<unsigned char>(out.back()))) {
            out.pop_back();
        }
        // Trim leading whitespace.
        std::size_t leading = 0;
        while (leading < out.size() &&
               IsAsciiWhitespace(static_cast<unsigned char>(out[leading]))) {
            ++leading;
        }
        if (leading > 0) out.erase(0, leading);

        return out;
    }
}
