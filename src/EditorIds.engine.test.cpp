#include <EditorIds.h>

#include <EngineMock.h>
#include <RelocationMocks.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

// Tests for reading a form's EditorID back. The case that matters is the one
// the EditorID-recovery mods leave behind: the name is in the engine's
// EditorID-to-form table, but the form's own GetFormEditorID() answers "".

namespace
{
    namespace EditorIds = NarrativeEngine::EditorIds;
    using NarrativeEngine::Testing::EditorIDTable;
    using NarrativeEngine::Testing::EngineMock;

    constexpr std::uint32_t kGovRuling = 0x0003662A;
    constexpr std::uint32_t kTownFaction = 0x00051596;
} // namespace

TEST_CASE("EditorIds reads a name only the engine's table holds", "[EditorIds][engine]")
{
    EngineMock engine;
    // Added unnamed, so the faction's own GetFormEditorID() answers "", then
    // named in the table alone -- what Tweaks and Native EditorID Fix leave.
    auto* faction = engine.AddFaction(kGovRuling);
    EditorIDTable().insert({RE::BSFixedString("GovRuling"), faction});

    EditorIds::Initialize();
    REQUIRE(std::string_view{EditorIds::Of(faction)} == "GovRuling");
}

TEST_CASE("EditorIds takes a form's own EditorID when it keeps one", "[EditorIds][engine]")
{
    EngineMock engine;
    auto* faction = engine.AddFaction(kTownFaction, "TownWhiterunFaction");
    EditorIds::Initialize();
    REQUIRE(std::string_view{EditorIds::Of(faction)} == "TownWhiterunFaction");
}

TEST_CASE("EditorIds answers empty for a form with no EditorID anywhere", "[EditorIds][engine]")
{
    EngineMock engine;
    auto* faction = engine.AddFaction(kTownFaction);
    EditorIds::Initialize();
    REQUIRE(std::string_view{EditorIds::Of(faction)}.empty());
    REQUIRE(std::string_view{EditorIds::Of(nullptr)}.empty());
}
