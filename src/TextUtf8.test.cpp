#include <TextUtf8.h>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// Unit tests for the character layer every language-aware step builds on.
//
// The cases are chosen from the scripts the shipped languages use: Latin with
// diacritics (German, French, Polish), Greek, Cyrillic (Russian), and Japanese,
// which has no case and no spaces. Each English-only behaviour these replace
// is wrong for at least one of them.

namespace
{
    using namespace NarrativeEngine::TextUtf8;

    // Spelled as explicit UTF-8 byte escapes rather than literal glyphs, as
    // LLMTextSanitizer.test.cpp does and for the same reason: a test whose
    // subject can be silently re-encoded can silently stop testing anything.
    constexpr std::string_view kAUmlautUpper = "\xC3\x84";                                                // U+00C4
    constexpr std::string_view kAUmlautLower = "\xC3\xA4";                                                // U+00E4
    constexpr std::string_view kOUmlautLower = "\xC3\xB6";                                                // U+00F6
    constexpr std::string_view kZheUpper = "\xD0\x96";                                                    // U+0416
    constexpr std::string_view kZheLower = "\xD0\xB6";                                                    // U+0436
    constexpr std::string_view kLStrokeUpper = "\xC5\x81";                                                // U+0141
    constexpr std::string_view kLStrokeLower = "\xC5\x82";                                                // U+0142
    constexpr std::string_view kEAcuteUpper = "\xC3\x89";                                                 // U+00C9
    constexpr std::string_view kEAcuteLower = "\xC3\xA9";                                                 // U+00E9
    constexpr std::string_view kOmegaUpper = "\xCE\xA9";                                                  // U+03A9
    constexpr std::string_view kOmegaLower = "\xCF\x89";                                                  // U+03C9
    constexpr std::string_view kSharpS = "\xC3\x9F";                                                      // U+00DF
    constexpr std::string_view kCombiningAcute = "\xCC\x81";                                              // U+0301
    constexpr std::string_view kCyrillicA = "\xD0\xB0";                                                   // U+0430
    constexpr std::string_view kUlfrikUpper = "\xD0\xA3\xD0\xBB\xD1\x8C\xD1\x84\xD1\x80\xD0\xB8\xD0\xBA"; // Ульфрик
    constexpr std::string_view kUlfrikLower = "\xD1\x83\xD0\xBB\xD1\x8C\xD1\x84\xD1\x80\xD0\xB8\xD0\xBA"; // ульфрик
    constexpr std::string_view kDragon = "\xE3\x83\x89\xE3\x83\xA9\xE3\x82\xB4\xE3\x83\xB3";              // ドラゴン
    constexpr std::string_view kKatakanaMiddleDot = "\xE3\x83\xBB";                                       // U+30FB
    constexpr std::string_view kIdeographicSpace = "\xE3\x80\x80";                                        // U+3000
    constexpr std::string_view kSteal = "\xE7\x9B\x97\xE3\x82\x80";                                       // 盗む
    constexpr std::string_view kEmoji = "\xF0\x9F\x98\x80";                                               // U+1F600
    constexpr std::string_view kRightSingleQuote = "\xE2\x80\x99";                                        // U+2019
    constexpr std::string_view kReplacement = "\xEF\xBF\xBD";                                             // U+FFFD

    std::string Cat(std::initializer_list<std::string_view> parts)
    {
        std::string out;
        for (const auto part : parts) {
            out.append(part);
        }
        return out;
    }

    std::vector<std::string> AsStrings(const std::vector<std::string_view>& views)
    {
        return {views.begin(), views.end()};
    }
} // namespace

// Moved from CharacterBioResolver.test.cpp with the function; the cases are
// unchanged.
TEST_CASE("TextUtf8::RepairWindows1252", "[TextUtf8]")
{
    SECTION("leaves valid UTF-8 alone")
    {
        CHECK(RepairWindows1252("Fl\xC3\xBC"
                                "chtling")
              == "Fl\xC3\xBC"
                 "chtling");
    }
    SECTION("re-encodes a Latin-1 byte")
    {
        CHECK(RepairWindows1252("Fl\xFC"
                                "chtling")
              == "Fl\xC3\xBC"
                 "chtling");
    }
    SECTION("re-encodes 0x80-0x9F through the code page")
    {
        // 0x92 is Windows-1252's right single quote, U+2019.
        CHECK(RepairWindows1252("Alik\x92r") == "Alik\xE2\x80\x99r");
    }
    SECTION("keeps valid sequences beside a repaired byte")
    {
        CHECK(RepairWindows1252("\xC3\xA9\xE9") == "\xC3\xA9\xC3\xA9");
    }
    SECTION("treats a truncated sequence as single bytes")
    {
        CHECK(RepairWindows1252("a\xC3") == "a\xC3\x83");
    }
}

TEST_CASE("TextUtf8::DecodeAt and SequenceLength", "[TextUtf8]")
{
    SECTION("decodes one to four bytes")
    {
        CHECK(DecodeAt("a", 0).codePoint == U'a');
        CHECK(DecodeAt(kZheUpper, 0).codePoint == 0x0416);
        CHECK(DecodeAt(kZheUpper, 0).length == 2);
        CHECK(DecodeAt(kDragon, 0).codePoint == 0x30C9);
        CHECK(DecodeAt(kDragon, 0).length == 3);
        CHECK(DecodeAt(kEmoji, 0).codePoint == 0x1F600);
        CHECK(DecodeAt(kEmoji, 0).length == 4);
    }
    SECTION("reads a malformed byte as U+FFFD, one byte long")
    {
        for (const std::string_view bad : {"\xFF", "\xC0\xAF", "\xED\xA0\x80", "\xE3\x83", "\x80"}) {
            CHECK(SequenceLength(bad, 0) == 0);
            CHECK(DecodeAt(bad, 0).codePoint == 0xFFFD);
            CHECK(DecodeAt(bad, 0).length == 1);
        }
    }
}

TEST_CASE("TextUtf8::IsValid", "[TextUtf8]")
{
    CHECK(IsValid(""));
    CHECK(IsValid(Cat({"Fl", kOUmlautLower, "te ", kUlfrikUpper, kDragon, kEmoji})));
    CHECK_FALSE(IsValid("Fl\xFC"
                        "te"));
    CHECK_FALSE(IsValid(Cat({kDragon, "\xE3\x83"})));
}

TEST_CASE("TextUtf8::CodePointCount", "[TextUtf8]")
{
    CHECK(CodePointCount("") == 0);
    CHECK(CodePointCount("Veleth") == 6);
    CHECK(CodePointCount(kUlfrikUpper) == 7);
    CHECK(kUlfrikUpper.size() == 14);
    CHECK(CodePointCount(kDragon) == 4);
    CHECK(CodePointCount(Cat({"a", kEmoji, "b"})) == 3);
    SECTION("a malformed byte counts as one")
    {
        CHECK(CodePointCount("a\xFF"
                             "b")
              == 3);
    }
}

TEST_CASE("TextUtf8::TruncateToCodePoints", "[TextUtf8]")
{
    SECTION("counts letters, not bytes")
    {
        std::string name{kUlfrikUpper};
        TruncateToCodePoints(name, 3);
        CHECK(name == "\xD0\xA3\xD0\xBB\xD1\x8C");
    }
    SECTION("never splits a character")
    {
        std::string text = Cat({"ab", kEmoji, "cd"});
        TruncateToCodePoints(text, 3);
        CHECK(text == Cat({"ab", kEmoji}));
        CHECK(IsValid(text));
    }
    SECTION("leaves a string that fits alone")
    {
        std::string text{kDragon};
        TruncateToCodePoints(text, 4);
        CHECK(text == kDragon);
        TruncateToCodePoints(text, 40);
        CHECK(text == kDragon);
    }
    SECTION("to zero is empty")
    {
        std::string text = "abc";
        TruncateToCodePoints(text, 0);
        CHECK(text.empty());
    }
}

TEST_CASE("TextUtf8::ToLower and ToUpper", "[TextUtf8]")
{
    SECTION("map every cased script")
    {
        CHECK(ToLower(Cat({kAUmlautUpper, kZheUpper, kLStrokeUpper, kOmegaUpper, kEAcuteUpper, "X"}))
              == Cat({kAUmlautLower, kZheLower, kLStrokeLower, kOmegaLower, kEAcuteLower, "x"}));
        CHECK(ToUpper(Cat({kAUmlautLower, kZheLower, kLStrokeLower, kOmegaLower, kEAcuteLower, "x"}))
              == Cat({kAUmlautUpper, kZheUpper, kLStrokeUpper, kOmegaUpper, kEAcuteUpper, "X"}));
        CHECK(ToLower(kUlfrikUpper) == "\xD1\x83\xD0\xBB\xD1\x8C\xD1\x84\xD1\x80\xD0\xB8\xD0\xBA");
    }
    SECTION("leave caseless scripts, digits and punctuation alone")
    {
        const auto text = Cat({kDragon, kSteal, " 42, ", kKatakanaMiddleDot});
        CHECK(ToLower(text) == text);
        CHECK(ToUpper(text) == text);
    }
    SECTION("copy a malformed byte through")
    {
        CHECK(ToLower("A\xFF"
                      "B")
              == "a\xFF"
                 "b");
    }
}

TEST_CASE("TextUtf8::UppercaseFirst and LowercaseFirst", "[TextUtf8]")
{
    SECTION("change only the first code point")
    {
        CHECK(UppercaseFirst("the amulet") == "The amulet");
        CHECK(LowercaseFirst("Steal The Amulet") == "steal The Amulet");
        CHECK(UppercaseFirst(Cat({kEAcuteLower, "lise ", kEAcuteLower})) == Cat({kEAcuteUpper, "lise ", kEAcuteLower}));
        CHECK(LowercaseFirst(kUlfrikUpper) == kUlfrikLower);
        CHECK(UppercaseFirst(kUlfrikLower) == kUlfrikUpper);
    }
    SECTION("leave a caseless first letter and an empty string alone")
    {
        CHECK(UppercaseFirst(kDragon) == kDragon);
        CHECK(UppercaseFirst("").empty());
        CHECK(UppercaseFirst("42 men") == "42 men");
    }
}

TEST_CASE("TextUtf8::Fold and EqualsFolded", "[TextUtf8]")
{
    SECTION("full folding: sharp s is ss")
    {
        CHECK(EqualsFolded(Cat({"Stra", kSharpS, "e"}), "STRASSE"));
    }
    SECTION("composed and decomposed accents are the same letter")
    {
        CHECK(EqualsFolded(Cat({"caf", kEAcuteLower}), Cat({"CAFE", kCombiningAcute})));
    }
    SECTION("every cased script folds")
    {
        CHECK(EqualsFolded(kUlfrikUpper, kUlfrikLower));
        CHECK(EqualsFolded(Cat({kAUmlautUpper, kZheUpper, kLStrokeUpper, kOmegaUpper}),
                           Cat({kAUmlautLower, kZheLower, kLStrokeLower, kOmegaLower})));
    }
    SECTION("different words stay different")
    {
        CHECK_FALSE(EqualsFolded("Veleth", "Velith"));
        CHECK_FALSE(EqualsFolded(kUlfrikUpper, Cat({kUlfrikUpper, kCyrillicA})));
        CHECK_FALSE(EqualsFolded(kAUmlautLower, "a"));
    }
    SECTION("caseless text folds to itself")
    {
        CHECK(Fold(kDragon) == kDragon);
    }
    SECTION("malformed bytes fold to U+FFFD")
    {
        CHECK(Fold("a\xFF") == Cat({"a", kReplacement}));
    }
}

TEST_CASE("TextUtf8::character classes", "[TextUtf8]")
{
    SECTION("letters, in every script")
    {
        CHECK(IsLetter(U'a'));
        CHECK(IsLetter(0x0416)); // Ж
        CHECK(IsLetter(0x30C9)); // ド
        CHECK(IsLetter(0x76D7)); // 盗
        CHECK_FALSE(IsLetter(U'4'));
        CHECK_FALSE(IsLetter(U' '));
        CHECK_FALSE(IsLetter(0x30FB)); // ・
    }
    SECTION("case, where a script has it")
    {
        CHECK(IsUpper(0x0416));
        CHECK(IsLower(0x0436));
        CHECK(IsUpper(U'V'));
        CHECK_FALSE(IsUpper(0x30C9));
        CHECK_FALSE(IsLower(0x30C9));
    }
    SECTION("word characters")
    {
        for (const char32_t c : {U'a', U'Z', U'7', U'-', U'\'', char32_t{0x0141}, char32_t{0x0301}, char32_t{0x30C9}}) {
            CHECK(IsWordChar(c));
        }
        for (const char32_t c :
             {U' ', U'.', U',', char32_t{0x3000}, char32_t{0x30FB}, char32_t{0x2019}, char32_t{0xFFFD}}) {
            CHECK_FALSE(IsWordChar(c));
        }
    }
}

TEST_CASE("TextUtf8::Words", "[TextUtf8]")
{
    SECTION("hyphens and apostrophes inside a word keep it whole")
    {
        CHECK(AsStrings(Words("Asgeir Snow-Shod met Ri'saad."))
              == std::vector<std::string>{"Asgeir", "Snow-Shod", "met", "Ri'saad"});
    }
    SECTION("hyphens and apostrophes at either end are trimmed")
    {
        CHECK(AsStrings(Words("'quoted' --dash--")) == std::vector<std::string>{"quoted", "dash"});
    }
    SECTION("a word in Cyrillic or with diacritics is one word")
    {
        CHECK(AsStrings(Words(Cat({kUlfrikUpper, " ", kUlfrikLower})))
              == std::vector<std::string>{std::string{kUlfrikUpper}, std::string{kUlfrikLower}});
        CHECK(AsStrings(Words(Cat({"J", kOUmlautLower, "rgen"})))
              == std::vector<std::string>{Cat({"J", kOUmlautLower, "rgen"})});
    }
    SECTION("Japanese splits on its own separators and nowhere else")
    {
        CHECK(AsStrings(Words(Cat({kDragon, kKatakanaMiddleDot, kDragon})))
              == std::vector<std::string>{std::string{kDragon}, std::string{kDragon}});
        CHECK(AsStrings(Words(Cat({kDragon, kIdeographicSpace, kSteal})))
              == std::vector<std::string>{std::string{kDragon}, std::string{kSteal}});
        CHECK(AsStrings(Words(Cat({kDragon, kSteal}))) == std::vector<std::string>{Cat({kDragon, kSteal})});
    }
    SECTION("a typographic apostrophe and a malformed byte separate words")
    {
        CHECK(AsStrings(Words(Cat({"Hulda", kRightSingleQuote, "s"}))) == std::vector<std::string>{"Hulda", "s"});
        CHECK(AsStrings(Words("a\xFF"
                              "b"))
              == std::vector<std::string>{"a", "b"});
    }
    SECTION("nothing but separators is no words")
    {
        CHECK(Words("").empty());
        CHECK(Words(" -- , ' ").empty());
    }
}

TEST_CASE("TextUtf8::StandsAlone", "[TextUtf8]")
{
    SECTION("a trailing apostrophe is read as the caller says")
    {
        const std::string_view text = "Veleth's movements";
        CHECK_FALSE(StandsAlone(text, 0, 6, TrailingApostrophe::PartOfWord));
        CHECK(StandsAlone(text, 0, 6, TrailingApostrophe::EndsWord));
    }
    SECTION("a leading apostrophe always continues a word")
    {
        CHECK_FALSE(StandsAlone("O'Ryan", 2, 4, TrailingApostrophe::EndsWord));
    }
    SECTION("a match inside a longer word, in any script, does not stand alone")
    {
        CHECK_FALSE(StandsAlone("Velethson", 0, 6));
        const auto genitive = Cat({kUlfrikUpper, kCyrillicA});
        CHECK_FALSE(StandsAlone(genitive, 0, kUlfrikUpper.size()));
        const auto prefixed = Cat({kZheLower, kUlfrikUpper});
        CHECK_FALSE(StandsAlone(prefixed, kZheLower.size(), kUlfrikUpper.size()));
    }
    SECTION("a match between separators stands alone")
    {
        const auto text = Cat({"Jarl ", kUlfrikUpper, ", the rebel"});
        CHECK(StandsAlone(text, 5, kUlfrikUpper.size()));
        CHECK(StandsAlone("Veleth", 0, 6));
    }
    SECTION("a malformed byte is a separator")
    {
        CHECK(StandsAlone("\xFF"
                          "Veleth\xFF",
                          1,
                          6));
    }
    SECTION("unseparated Japanese never stands alone, which is why it needs substring matching")
    {
        const auto text = Cat({kDragon, kSteal});
        CHECK_FALSE(StandsAlone(text, 0, kDragon.size()));
    }
}
