#include <BookTextRenderer.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

// Unit tests for the book-markup renderer.
//
// A pure string-to-string transform over Skyrim's Gamebryo/Scaleform
// pseudo-HTML, so no engine and nothing to mock. It earns real tests anyway:
// its whole job is surviving markup nobody validated. Vanilla BOOK records and
// hand-written mod-added HTML both land here verbatim, and the failure that
// created the module was ~20KB of illustrated tag soup from Crafting Manuals
// blowing out a Director prompt.
//
// Everything below drives the single public entry point. The tag scanner,
// entity decoder and whitespace normalizer are anonymous-namespace helpers and
// are exercised only through it.

namespace
{
    using NarrativeEngine::BookTextRenderer::RenderToPlainText;

    // Written as explicit UTF-8 byte escapes rather than literal glyphs, so the
    // expected bytes cannot change under an editor that re-encodes the file.
    constexpr std::string_view kEAcute = "\xC3\xA9";                   // U+00E9, 2 bytes
    constexpr std::string_view kCjkWorld = "\xE4\xB8\x96";             // U+4E16, 3 bytes
    constexpr std::string_view kGrinning = "\xF0\x9F\x98\x80";         // U+1F600, 4 bytes
    constexpr std::string_view kCyrillic = "\xD0\x9C\xD0\xB8\xD1\x80"; // Мир
} // namespace

TEST_CASE("BookTextRenderer strips inline tags", "[BookTextRenderer]")
{
    // There is little to hoist anywhere in this file, and that is honest rather
    // than an oversight: the module is a pure function of one argument, so the
    // "happy path" is a shape of input rather than state to arrange. What is
    // shared is the prose each case wraps in markup — named so an assertion
    // that the text survived cannot be confused with one about the markup.
    const std::string kText = "Ysolda kept the ledger.";

    SECTION("when the markup contains a styling tag")
    {
        SECTION("should drop the tag and keep the text")
        {
            REQUIRE(RenderToPlainText("<b>" + kText + "</b>") == kText);
        }
    }

    SECTION("when the tag carries attributes")
    {
        SECTION("should drop the whole tag")
        {
            REQUIRE(RenderToPlainText("<font face='$HandwrittenFont' color='#000'>" + kText) == kText);
        }
    }

    SECTION("when an attribute value contains an angle bracket")
    {
        SECTION("should not end the tag early")
        {
            // The scanner tracks quotes so a '>' inside an attribute cannot
            // terminate the tag. Vanilla illustrated books carry src='img://…'
            // values, and ending early would spill the rest of the attribute
            // into the page as visible text.
            REQUIRE(RenderToPlainText("<font face='a>b'>" + kText) == kText);
        }
    }

    SECTION("when the tag is a closing tag")
    {
        SECTION("should drop it too")
        {
            REQUIRE(RenderToPlainText("</i>" + kText) == kText);
        }
    }

    SECTION("when the tag is self-closing")
    {
        SECTION("should drop it")
        {
            REQUIRE(RenderToPlainText("<span/>" + kText) == kText);
        }
    }

    SECTION("when the markup contains an HTML comment")
    {
        SECTION("should drop the comment and its contents")
        {
            REQUIRE(RenderToPlainText("<!-- editor note -->" + kText) == kText);
        }
    }

    SECTION("when the markup contains a doctype")
    {
        SECTION("should drop it")
        {
            REQUIRE(RenderToPlainText("<!DOCTYPE html>" + kText) == kText);
        }
    }
}

TEST_CASE("BookTextRenderer keeps block structure", "[BookTextRenderer]")
{
    // Two runs of prose, distinct so a case can tell "kept the break" from
    // "ran them together" without counting characters.
    const std::string kFirst = "The first paragraph.";
    const std::string kSecond = "The second paragraph.";

    SECTION("when a block tag separates two runs of text")
    {
        SECTION("should put them on separate lines")
        {
            REQUIRE(RenderToPlainText(kFirst + "<p>" + kSecond) == kFirst + "\n" + kSecond);
        }
    }

    SECTION("when the separator is a closing block tag")
    {
        SECTION("should still break the line")
        {
            // The scanner strips the leading '/' before reading the name, so
            // </p> breaks exactly as <p> does. Books close paragraphs far more
            // consistently than they open them.
            REQUIRE(RenderToPlainText(kFirst + "</p>" + kSecond) == kFirst + "\n" + kSecond);
        }
    }

    SECTION("when several block tags stack up")
    {
        SECTION("should collapse them to one blank line")
        {
            // Vanilla books stack four or five <p> tags to push a heading down
            // the page. Rendered literally that is four blank lines of nothing.
            REQUIRE(RenderToPlainText(kFirst + "<p><p><p><p>" + kSecond) == kFirst + "\n\n" + kSecond);
        }
    }

    SECTION("when an illustration appears between paragraphs")
    {
        SECTION("should break the line around it")
        {
            // The Crafting Manuals case: the renderer lays plates out as
            // blocks, so the text either side of one must not run together.
            const std::string markup = kFirst + "<img src='img://textures/interface/book/plate.dds'>" + kSecond;
            REQUIRE(RenderToPlainText(markup) == kFirst + "\n" + kSecond);
        }
    }
}

TEST_CASE("BookTextRenderer decodes entities", "[BookTextRenderer]")
{
    SECTION("when the entity is named")
    {
        SECTION("should decode it to its character")
        {
            REQUIRE(RenderToPlainText("Tools &amp; Trade") == "Tools & Trade");
        }

        SECTION("should decode the typographic ones to ASCII")
        {
            // Deliberately ASCII rather than the real glyphs: this text ends up
            // in event-log lines and prompts, and the substitutions match the
            // house style used for LLM text elsewhere.
            REQUIRE(RenderToPlainText("&ldquo;Hm&rdquo;&mdash;&hellip;") == "\"Hm\"--...");
        }
    }

    SECTION("when the entity is a decimal reference")
    {
        SECTION("should decode it")
        {
            REQUIRE(RenderToPlainText("caf&#233;") == "caf" + std::string{kEAcute});
        }
    }

    SECTION("when the entity is a hexadecimal reference")
    {
        SECTION("should decode it")
        {
            REQUIRE(RenderToPlainText("caf&#xE9;") == "caf" + std::string{kEAcute});
        }
    }

    SECTION("when the codepoint needs more than one byte")
    {
        SECTION("should encode it as UTF-8")
        {
            // One fact at three widths. A decoder that emitted the codepoint as
            // a single byte would pass every ASCII case above and corrupt every
            // non-English book.
            REQUIRE(RenderToPlainText("&#xE9;") == std::string{kEAcute});
            REQUIRE(RenderToPlainText("&#x4E16;") == std::string{kCjkWorld});
            REQUIRE(RenderToPlainText("&#x1F600;") == std::string{kGrinning});
        }
    }

    SECTION("when the entity is a soft hyphen")
    {
        SECTION("should drop it entirely")
        {
            REQUIRE(RenderToPlainText("sum&shy;mer") == "summer");
        }
    }
}

TEST_CASE("BookTextRenderer leaves unrecognized markup alone", "[BookTextRenderer]")
{
    SECTION("when a bare angle bracket appears in prose")
    {
        SECTION("should keep it")
        {
            REQUIRE(RenderToPlainText("a < b") == "a < b");
        }
    }

    SECTION("when a tag is never closed")
    {
        SECTION("should keep it as literal text")
        {
            // Rejecting an unterminated construct rather than consuming to end
            // of input is the point: swallowing the tail to salvage a truncated
            // tag would lose the rest of the book.
            REQUIRE(RenderToPlainText("the end <b") == "the end <b");
        }
    }

    SECTION("when a comment is never closed")
    {
        SECTION("should keep it as literal text")
        {
            REQUIRE(RenderToPlainText("the end <!-- oops") == "the end <!-- oops");
        }
    }

    SECTION("when an ampersand has no semicolon after it")
    {
        SECTION("should keep the ampersand")
        {
            REQUIRE(RenderToPlainText("Tools & Trade") == "Tools & Trade");
        }
    }

    SECTION("when the entity name is unknown")
    {
        SECTION("should keep it verbatim")
        {
            REQUIRE(RenderToPlainText("&copy;") == "&copy;");
        }
    }

    SECTION("when a numeric entity is out of the Unicode range")
    {
        SECTION("should keep it verbatim")
        {
            REQUIRE(RenderToPlainText("&#1114112;") == "&#1114112;");
        }
    }

    SECTION("when a numeric entity is zero")
    {
        SECTION("should keep it verbatim")
        {
            // A NUL would terminate the string for every C consumer downstream.
            REQUIRE(RenderToPlainText("&#0;") == "&#0;");
        }
    }

    SECTION("when the entity body is empty")
    {
        SECTION("should keep it verbatim")
        {
            REQUIRE(RenderToPlainText("&;") == "&;");
        }
    }
}

TEST_CASE("BookTextRenderer separates pages", "[BookTextRenderer]")
{
    const std::string kPageOne = "End of the first page.";
    const std::string kPageTwo = "Start of the second.";

    SECTION("when a page-break token appears")
    {
        SECTION("should leave a blank line between the pages")
        {
            // A page turn is a harder break than a paragraph, so it survives
            // normalization as a blank line where stacked <p> tags do not.
            REQUIRE(RenderToPlainText(kPageOne + "[pagebreak]" + kPageTwo) == kPageOne + "\n\n" + kPageTwo);
        }
    }

    SECTION("when the token is written in a different case")
    {
        SECTION("should still separate the pages")
        {
            REQUIRE(RenderToPlainText(kPageOne + "[PageBreak]" + kPageTwo) == kPageOne + "\n\n" + kPageTwo);
        }
    }

    SECTION("when a bracket opens something that is not the token")
    {
        SECTION("should keep the bracket")
        {
            REQUIRE(RenderToPlainText("[note] " + kPageOne) == "[note] " + kPageOne);
        }
    }
}

TEST_CASE("BookTextRenderer normalizes whitespace", "[BookTextRenderer]")
{
    SECTION("when a run of spaces appears")
    {
        SECTION("should collapse it to one space")
        {
            REQUIRE(RenderToPlainText("one     two") == "one two");
        }
    }

    SECTION("when the text has whitespace at both ends")
    {
        SECTION("should trim it")
        {
            REQUIRE(RenderToPlainText("\n\n   trimmed   \n\n") == "trimmed");
        }
    }

    SECTION("when carriage returns appear")
    {
        SECTION("should drop them")
        {
            REQUIRE(RenderToPlainText("line\r\nnext") == "line\nnext");
        }
    }

    SECTION("when a line break and spaces sit together")
    {
        SECTION("should keep the line break and drop the spaces")
        {
            // Without this a paragraph break would come out as "\n " and every
            // rendered line would start with a stray space.
            REQUIRE(RenderToPlainText("a  \n  b") == "a\nb");
        }
    }
}

TEST_CASE("BookTextRenderer passes ordinary text through", "[BookTextRenderer]")
{
    SECTION("when the input is empty")
    {
        SECTION("should return an empty string")
        {
            REQUIRE(RenderToPlainText("").empty());
        }
    }

    SECTION("when the input is nothing but markup")
    {
        SECTION("should return an empty string")
        {
            REQUIRE(RenderToPlainText("<p><b></b></p>  ").empty());
        }
    }

    SECTION("when the text is not English")
    {
        SECTION("should keep the bytes intact")
        {
            // Book text is UTF-8 from an ESP record and is not ours to
            // romanize; only markup and entities are touched.
            REQUIRE(RenderToPlainText("<p>" + std::string{kCyrillic} + "</p>") == std::string{kCyrillic});
        }
    }
}
