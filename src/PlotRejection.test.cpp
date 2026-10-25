#include <PlotRejection.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

// Every refusal plot birth can send back is a code that
// components/narrative_engine/plot_rejection.prompt words. A code with no
// branch there would reach the model as an empty retry, which tells it
// nothing, so a new code without wording fails here instead.

namespace
{
    using NarrativeEngine::PlotRejection::Code;
    using NarrativeEngine::PlotRejection::Id;

    std::string RejectionPrompt()
    {
        std::ifstream in(std::filesystem::path{NARRATIVEENGINE_STATICS_DIR}
                             / "SKSE/Plugins/SkyrimNet/external/pdusen.narrative-engine/prompts/components/"
                               "narrative_engine/plot_rejection.prompt",
                         std::ios::binary);
        return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
} // namespace

TEST_CASE("PlotRejection: every code is worded for the retry", "[PlotRejection]")
{
    const auto prompt = RejectionPrompt();
    REQUIRE_FALSE(prompt.empty());

    std::set<std::string> seen;
    for (std::size_t i = 1; i < static_cast<std::size_t>(Code::Count); ++i) {
        const auto id = std::string{Id(static_cast<Code>(i))};
        INFO(id);
        REQUIRE_FALSE(id.empty());
        CHECK(seen.insert(id).second);
        CHECK(prompt.find("code == \"" + id + "\"") != std::string::npos);
    }
}

TEST_CASE("PlotRejection: what a retry reads", "[PlotRejection]")
{
    using NarrativeEngine::PlotRejection::Make;

    SECTION("when a refusal carries arguments")
    {
        const auto json = Make(Code::TooLong, {{"key", "scheme"}, {"length", 412}, {"limit", 300}}).ToJson();

        SECTION("should give its code beside them")
        {
            CHECK(json["code"] == "too_long");
            CHECK(json["key"] == "scheme");
            CHECK(json["length"] == 412);
        }
    }

    SECTION("when nothing was refused")
    {
        SECTION("should be empty")
        {
            CHECK(NarrativeEngine::PlotRejection::Rejection{}.Empty());
            CHECK(Id(Code::None).empty());
        }
    }
}
