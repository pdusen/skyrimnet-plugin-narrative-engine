#include <PromptLanguage.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

// Unit tests for what a content-producing prompt's context is told about the
// language to write its content in. The include that reads these fields is
// checked by rendering it; see Step 19 of
// docs/implementation/PHASE_18_INTERNATIONALIZATION.md.

namespace
{
    using namespace NarrativeEngine;
    using LanguageTable::Table;

    constexpr std::array<TextKeys::Key, 0> kNoKeys{};

    constexpr std::string_view kEnglish = "[Language]\nName = English\nNativeName = English\nCode = en\n"
                                          "Status = reviewed\nPluralRule = one_other\nFonts = en\n";
    constexpr std::string_view kGerman = "[Language]\nName = German\nNativeName = Deutsch\nCode = de\n"
                                         "Status = draft\nPluralRule = one_other\nFonts = en\n"
                                         "[Prompt.Form]\nscheme = a zu-clause\nbody = unused\n";
} // namespace

TEST_CASE("PromptLanguage: the context names the language and the content fields", "[PromptLanguage]")
{
    SECTION("English, which the include renders nothing for")
    {
        nlohmann::json ctx;
        PromptLanguage::Add(ctx, {"narration"}, Table::Build({kEnglish, std::nullopt, std::nullopt}, kNoKeys));
        CHECK(ctx["output_language"] == "English");
        CHECK(ctx["content_fields"] == nlohmann::json::array({"narration"}));
        CHECK(ctx["prompt_forms"] == nlohmann::json::array());
    }
    SECTION("another language, with a form only for the fields this prompt has and the file gives one")
    {
        nlohmann::json ctx;
        PromptLanguage::Add(
            ctx, {"ambition", "scheme", "finish"}, Table::Build({kEnglish, kGerman, std::nullopt}, kNoKeys));
        CHECK(ctx["output_language"] == "German");
        CHECK(ctx["content_fields"] == nlohmann::json::array({"ambition", "scheme", "finish"}));
        CHECK(ctx["prompt_forms"] == nlohmann::json::array({{{"field", "scheme"}, {"form", "a zu-clause"}}}));
    }
}
