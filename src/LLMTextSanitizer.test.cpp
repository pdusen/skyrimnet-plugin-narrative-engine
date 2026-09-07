#include <LLMTextSanitizer.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <string_view>

// Unit tests for the LLM text sanitizer.
//
// This module is the whole reason the co-located test convention works at all
// for an SKSE plugin: `Sanitize` and `TruncateUTF8` are pure byte-level string
// transforms over UTF-8, so they can be exercised in a console process with no
// Skyrim, no SKSE, and no CommonLibSSE relocations. The behaviour under test is
// the substitution table in `docs/LLM_RESPONSE_HANDLING.md`, which is the
// project's contract with every subsystem that stores or displays LLM output.
//
// What is asserted here is externally meaningful behaviour: the bytes that come
// out for the bytes that go in. Nothing reaches into `ReplaceCodepoint`, the
// decoder, or any other implementation detail — those live in an anonymous
// namespace and are free to be restructured as long as these cases still hold.

namespace
{
    using NarrativeEngine::LLMTextSanitizer::Sanitize;
    using NarrativeEngine::LLMTextSanitizer::TruncateUTF8;

    // The codepoints under test, spelled as explicit UTF-8 byte escapes rather
    // than as literal glyphs. A literal `—` in this file would depend on the
    // source encoding surviving every editor, git filter and formatting hook it
    // passes through; a test whose subject can be silently re-encoded is a test
    // that can silently stop testing anything.
    //
    // Each escape sequence lives in its own constant so no hex escape is ever
    // followed by a hex digit — `"\x94" "7"` is two characters, but the same
    // bytes written as one literal would be parsed as the single character
    // \x947.
    constexpr std::string_view kEmDash = "\xE2\x80\x94";           // U+2014
    constexpr std::string_view kEnDash = "\xE2\x80\x93";           // U+2013
    constexpr std::string_view kHorizontalBar = "\xE2\x80\x95";    // U+2015
    constexpr std::string_view kEllipsis = "\xE2\x80\xA6";         // U+2026
    constexpr std::string_view kLeftDoubleQuote = "\xE2\x80\x9C";  // U+201C
    constexpr std::string_view kRightDoubleQuote = "\xE2\x80\x9D"; // U+201D
    constexpr std::string_view kRightSingleQuote = "\xE2\x80\x99"; // U+2019
    constexpr std::string_view kBullet = "\xE2\x80\xA2";           // U+2022
    constexpr std::string_view kZeroWidthSpace = "\xE2\x80\x8B";   // U+200B
    constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";    // U+FEFF
    constexpr std::string_view kNonBreakingSpace = "\xC2\xA0";     // U+00A0

    // Codepoints that must survive untouched — the passthrough half of the
    // contract, which exists so non-English players keep their own letters.
    constexpr std::string_view kEAcute = "\xC3\xA9";                      // U+00E9
    constexpr std::string_view kLeftGuillemet = "\xC2\xAB";               // U+00AB
    constexpr std::string_view kRightGuillemet = "\xC2\xBB";              // U+00BB
    constexpr std::string_view kCyrillicMir = "\xD0\x9C\xD0\xB8\xD1\x80"; // Мир
    constexpr std::string_view kCjkWorld = "\xE4\xB8\x96";                // U+4E16

    // True when every byte is in the range a Skyrim ASCII-only engine field can
    // hold without truncating or garbling.
    bool IsAscii(std::string_view text)
    {
        for (const char c : text) {
            if (static_cast<unsigned char>(c) >= 0x80)
                return false;
        }
        return true;
    }

    // True when `text` is well-formed UTF-8. This is the property that actually
    // matters downstream: nlohmann::json refuses to serialize invalid UTF-8 and
    // throws type_error.316 out of a later `dump()`, far from wherever the bad
    // bytes were introduced. Both functions under test promise never to produce
    // such a string, whatever they are handed.
    bool IsValidUTF8(std::string_view text)
    {
        std::size_t i = 0;
        while (i < text.size()) {
            const auto lead = static_cast<unsigned char>(text[i]);
            std::size_t continuationBytes = 0;
            if (lead < 0x80)
                continuationBytes = 0;
            else if ((lead & 0xE0) == 0xC0)
                continuationBytes = 1;
            else if ((lead & 0xF0) == 0xE0)
                continuationBytes = 2;
            else if ((lead & 0xF8) == 0xF0)
                continuationBytes = 3;
            else
                return false;

            if (continuationBytes > 0 && i + continuationBytes >= text.size())
                return false;

            for (std::size_t k = 1; k <= continuationBytes; ++k) {
                if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80)
                    return false;
            }
            i += continuationBytes + 1;
        }
        return true;
    }
} // namespace

TEST_CASE("LLMTextSanitizer::Sanitize", "[LLMTextSanitizer][Sanitize]")
{
    // Common setup. Catch2 re-runs everything above a SECTION for each leaf
    // path below it, so each expectation gets its own fresh `input`.
    //
    // Every case embeds its subject in a sentence rather than sanitizing the
    // bare artifact, because the failure this module prevents is not "a lone
    // em-dash survived" — it is "an em-dash survived in the middle of a line
    // an NPC is about to speak". Sanitizing the artifact alone would pass even
    // if the surrounding text were mangled.
    const auto Wrap = [](std::string_view middle) {
        return std::string{"Ysolda said "} + std::string{middle} + " and left.";
    };
    std::string input;

    SECTION("when the response is already plain ASCII")
    {
        input = Wrap("nothing unusual");
        const std::string result = Sanitize(input);

        SECTION("should return it byte-for-byte unchanged")
        {
            REQUIRE(result == input);
        }
    }

    SECTION("when the response contains smart typography")
    {
        SECTION("and the artifact is an em-dash")
        {
            input = Wrap(kEmDash);
            const std::string result = Sanitize(input);

            SECTION("should replace it with a doubled hyphen")
            {
                REQUIRE(result == Wrap("--"));
            }

            SECTION("should leave nothing the engine cannot render")
            {
                REQUIRE(IsAscii(result));
            }
        }

        SECTION("and the artifact is an ellipsis")
        {
            input = Wrap(kEllipsis);

            SECTION("should replace it with three periods")
            {
                REQUIRE(Sanitize(input) == Wrap("..."));
            }
        }

        SECTION("and the artifact is a pair of smart double quotes")
        {
            input = Wrap(std::string{kLeftDoubleQuote} + "no" + std::string{kRightDoubleQuote});

            SECTION("should replace both with straight ASCII quotes")
            {
                REQUIRE(Sanitize(input) == Wrap("\"no\""));
            }
        }

        SECTION("and the artifact is a smart apostrophe")
        {
            input = Wrap(std::string{"the guard"} + std::string{kRightSingleQuote} + "s name");

            SECTION("should replace it with a straight apostrophe")
            {
                REQUIRE(Sanitize(input) == Wrap("the guard's name"));
            }
        }

        SECTION("and the artifact is an en-dash")
        {
            input = Wrap(std::string{"pages 3"} + std::string{kEnDash} + "7");

            SECTION("should replace it with a single hyphen")
            {
                REQUIRE(Sanitize(input) == Wrap("pages 3-7"));
            }
        }

        SECTION("and the artifact is a markdown bullet marker")
        {
            input = Wrap(std::string{kBullet} + " one");

            SECTION("should replace it with a hyphen so the list stays visible")
            {
                REQUIRE(Sanitize(input) == Wrap("- one"));
            }
        }

        SECTION("and the artifact is a horizontal bar")
        {
            input = Wrap(std::string{"a"} + std::string{kHorizontalBar} + "b");

            // The one long-dash codepoint that is dropped rather than
            // substituted: it is a quotation rule, not a clause separator, so
            // neither "-" nor "--" would be right.
            SECTION("should drop it rather than substitute a dash")
            {
                REQUIRE(Sanitize(input) == Wrap("ab"));
            }
        }
    }

    SECTION("when the response contains invisible formatting characters")
    {
        input = Wrap(std::string{kZeroWidthSpace} + "hidden" + std::string{kByteOrderMark});
        const std::string result = Sanitize(input);

        SECTION("should drop them and keep the visible text")
        {
            REQUIRE(result == Wrap("hidden"));
        }

        SECTION("should leave nothing the engine cannot render")
        {
            REQUIRE(IsAscii(result));
        }
    }

    SECTION("when the response contains a non-breaking space")
    {
        input = Wrap(std::string{"two"} + std::string{kNonBreakingSpace} + "words");

        SECTION("should replace it with an ordinary space")
        {
            REQUIRE(Sanitize(input) == Wrap("two words"));
        }
    }

    SECTION("when the response is in a language other than English")
    {
        // The passthrough half of the contract. These are the cases a naive
        // "strip everything non-ASCII" filter would get wrong, and getting them
        // wrong romanizes or deletes a non-English player's own text.
        SECTION("and it uses accented Latin letters")
        {
            input = Wrap(std::string{"caf"} + std::string{kEAcute});

            SECTION("should pass the letters through untouched")
            {
                REQUIRE(Sanitize(input) == input);
            }
        }

        SECTION("and it uses guillemets as quotation marks")
        {
            input = Wrap(std::string{kLeftGuillemet} + "oui" + std::string{kRightGuillemet});

            SECTION("should not mistake them for smart quotes")
            {
                REQUIRE(Sanitize(input) == input);
            }
        }

        SECTION("and it uses a non-Latin script")
        {
            input = Wrap(std::string{kCyrillicMir} + " " + std::string{kCjkWorld});

            SECTION("should pass the codepoints through untouched")
            {
                REQUIRE(Sanitize(input) == input);
            }
        }
    }

    SECTION("when the response is malformed UTF-8")
    {
        SECTION("and a multi-byte sequence is cut short")
        {
            // A three-byte lead followed by only one continuation byte, then
            // ordinary text — the shape a response truncated mid-character
            // arrives in.
            input = std::string{"Windhelm "} + "\xE2\x80" + "gate";
            const std::string result = Sanitize(input);

            SECTION("should drop the incomplete sequence")
            {
                REQUIRE(result == "Windhelm gate");
            }

            SECTION("should still be valid UTF-8")
            {
                REQUIRE(IsValidUTF8(result));
            }
        }

        SECTION("and a stray continuation byte appears on its own")
        {
            input = std::string{"gate"} + "\xA0" + "house";
            const std::string result = Sanitize(input);

            SECTION("should drop the orphan byte and keep the rest")
            {
                REQUIRE(result == "gatehouse");
            }

            SECTION("should still be valid UTF-8")
            {
                REQUIRE(IsValidUTF8(result));
            }
        }
    }

    SECTION("when the response has surrounding whitespace")
    {
        SECTION("and it is ordinary ASCII whitespace")
        {
            input = "  \n\tYsolda said\tnothing.\n  ";
            const std::string result = Sanitize(input);

            SECTION("should trim both ends")
            {
                REQUIRE(result == "Ysolda said\tnothing.");
            }

            SECTION("should keep the whitespace inside the text")
            {
                REQUIRE(result.find('\t') != std::string::npos);
            }
        }

        SECTION("and the outer whitespace is a non-breaking space")
        {
            // Ordering matters here: the NBSP first becomes an ASCII space,
            // and only then is the string trimmed. A sanitizer that trimmed
            // before substituting would leave a leading and trailing blank.
            input = std::string{kNonBreakingSpace} + "Ysolda said nothing." + std::string{kNonBreakingSpace};

            SECTION("should trim the space its own substitution produced")
            {
                REQUIRE(Sanitize(input) == "Ysolda said nothing.");
            }
        }
    }

    SECTION("when the response carries no visible text")
    {
        SECTION("and it is empty")
        {
            input = "";

            SECTION("should return an empty string")
            {
                REQUIRE(Sanitize(input).empty());
            }
        }

        SECTION("and it is nothing but invisible characters and whitespace")
        {
            input = std::string{kByteOrderMark} + "  " + std::string{kZeroWidthSpace} + "\n";

            SECTION("should return an empty string")
            {
                REQUIRE(Sanitize(input).empty());
            }
        }
    }
}

TEST_CASE("LLMTextSanitizer::TruncateUTF8", "[LLMTextSanitizer][TruncateUTF8]")
{
    // Common setup: the buffer each section clamps in place. Constructed fresh
    // for every leaf path, so a section that mutates it cannot affect another.
    std::string text;

    SECTION("when the text already fits the budget")
    {
        SECTION("and it is well under")
        {
            text = "Riverwood";
            TruncateUTF8(text, 64);

            SECTION("should leave it unchanged")
            {
                REQUIRE(text == "Riverwood");
            }
        }

        SECTION("and it is exactly the budget")
        {
            text = "Riverwood";
            TruncateUTF8(text, text.size());

            SECTION("should leave it unchanged")
            {
                REQUIRE(text == "Riverwood");
            }
        }
    }

    SECTION("when the text is too long")
    {
        SECTION("and every character is one byte")
        {
            text = "Riverwood";
            TruncateUTF8(text, 5);

            SECTION("should cut to exactly the budget")
            {
                REQUIRE(text == "River");
            }
        }

        SECTION("and the cut would land inside a two-byte character")
        {
            // "caf" + U+00E9 is five bytes; a budget of four splits the é.
            text = std::string{"caf"} + std::string{kEAcute};
            TruncateUTF8(text, 4);

            SECTION("should drop the whole character rather than half of it")
            {
                REQUIRE(text == "caf");
            }

            SECTION("should leave the string valid UTF-8")
            {
                REQUIRE(IsValidUTF8(text));
            }
        }

        SECTION("and the cut would land inside a three-byte character")
        {
            // "A" + U+4E16 is four bytes; a budget of three splits the CJK
            // character two bytes in, so the result comes back two bytes under
            // budget. Callers get "at most maxBytes", never "exactly".
            text = std::string{"A"} + std::string{kCjkWorld};
            TruncateUTF8(text, 3);

            SECTION("should drop the whole character rather than part of it")
            {
                REQUIRE(text == "A");
            }

            SECTION("should leave the string valid UTF-8")
            {
                REQUIRE(IsValidUTF8(text));
            }
        }

        SECTION("and the budget is zero")
        {
            text = std::string{"caf"} + std::string{kEAcute};
            TruncateUTF8(text, 0);

            SECTION("should empty the string")
            {
                REQUIRE(text.empty());
            }
        }
    }
}

TEST_CASE("LLMTextSanitizer sanitize-then-clamp pipeline", "[LLMTextSanitizer]")
{
    // The two functions are documented as a pair: sanitize at the point of
    // extraction from the response JSON, then clamp to whatever byte budget the
    // destination field has. `Sanitize` passes non-ASCII scripts through
    // verbatim, so its output is UTF-8 but not necessarily ASCII — which is
    // exactly the case a plain `resize()` would corrupt. This is the real usage
    // and the composition is where the corruption would show up.
    std::string extracted = std::string{kLeftDoubleQuote} + std::string{kCyrillicMir} + std::string{kRightDoubleQuote};

    SECTION("when a clamped budget cuts into the passed-through script")
    {
        std::string result = Sanitize(extracted);
        TruncateUTF8(result, 4);

        SECTION("should keep the substitution the sanitizer made")
        {
            REQUIRE(result.front() == '"');
        }

        SECTION("should stay within the budget")
        {
            REQUIRE(result.size() <= 4);
        }

        SECTION("should still be serializable as UTF-8")
        {
            REQUIRE(IsValidUTF8(result));
        }
    }
}
