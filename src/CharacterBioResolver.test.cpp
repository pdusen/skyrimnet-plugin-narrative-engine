#include <CharacterBioResolver.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

// Unit tests for the profile resolver.
//
// What is asserted is SkyrimNet's behaviour, as decoded from its DLL and
// checked against every uuid_mappings row on the development machine (see
// docs/engine-findings/skyrimnet-bio-template-name-assignment.md). The cases
// are the ones that document names: each is a way the old suffix matcher got a
// real NPC wrong, or a branch of the rule that matters.

namespace
{
    using namespace NarrativeEngine::CharacterBioResolver;

    ActorFacts Actor(std::uint32_t refId, std::string displayName, std::string basePlugin = "Skyrim.esm")
    {
        ActorFacts facts;
        facts.refId = refId;
        facts.displayName = std::move(displayName);
        facts.basePlugin = std::move(basePlugin);
        return facts;
    }
} // namespace

TEST_CASE("CharacterBioResolver::Slug", "[CharacterBioResolver][BioName]")
{
    SECTION("keeps a hyphen")
    {
        CHECK(Slug("Parvana al-Rihad") == "parvana_al-rihad");
    }
    SECTION("keeps square brackets")
    {
        CHECK(Slug("Sahpur [Black-Briar Mercenary]") == "sahpur_[black-briar_mercenary]");
    }
    SECTION("replaces an apostrophe, a backslash and a slash")
    {
        CHECK(Slug("Alik'r a\\b c/d") == "alik_r_a_b_c_d");
    }
    SECTION("leaves bytes above ASCII untouched")
    {
        // "Flüchtling": the u-umlaut is two UTF-8 bytes, kept as they are.
        CHECK(Slug("Fl\xC3\xBC"
                   "chtling")
              == "fl\xC3\xBC"
                 "chtling");
    }
    SECTION("does not trim")
    {
        CHECK(Slug(" Bandit ") == "_bandit_");
    }
}

TEST_CASE("CharacterBioResolver::RepairWindows1252", "[CharacterBioResolver][BioName]")
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

TEST_CASE("CharacterBioResolver::LocalId and UniqueKey", "[CharacterBioResolver][BioName]")
{
    CHECK(LocalId(0x00019DDC) == 0x019DDC);
    CHECK(LocalId(0x4C00B503) == 0x00B503);
    CHECK(LocalId(0xFE23ACAC) == 0xCAC);
    CHECK(UniqueKey("  Skyrim.esm ", 0x19DDC) == UniqueKey("skyrim.ESM", 0x019DDC));
    CHECK(UniqueKey("Skyrim.esm", 0x19DDC) != UniqueKey("Skyrim.esm", 0x19DDD));
}

TEST_CASE("CharacterBioResolver::BioName", "[CharacterBioResolver][BioName]")
{
    Tables tables;
    tables.unique[UniqueKey("Skyrim.esm", 0x019DDC)] = "brandish";
    tables.unique[UniqueKey("Skyrim.esm", 0x039FB7)] = "courier";
    tables.unique[UniqueKey("Light.esp", 0xCAC)] = "light_one";
    tables.generic[Slug("Refugee")] = "deceased_refugee";

    SECTION("falls back to the slug, zero-padded to three hex digits")
    {
        CHECK(BioName(Actor(0x0004E0B3, "Grelka"), tables) == "grelka_0B3");
    }
    SECTION("uses a unique row, keyed by the base NPC's plugin")
    {
        auto facts = Actor(0x00019DDC, "Brand-Shei");
        facts.refPlugin = "Unofficial Skyrim Special Edition Patch.esp";
        CHECK(BioName(facts, tables) == "brandish_DDC");
    }
    SECTION("uses the reference's plugin only when the base has none")
    {
        auto facts = Actor(0x00019DDC, "Brand-Shei", "");
        facts.refPlugin = "Skyrim.esm";
        CHECK(BioName(facts, tables) == "brandish_DDC");
    }
    SECTION("misses the unique row when the base plugin differs")
    {
        CHECK(BioName(Actor(0x00019DDC, "Brand-Shei", "Other.esp"), tables) == "brand-shei_DDC");
    }
    SECTION("applies the light-plugin mask to the unique key")
    {
        CHECK(BioName(Actor(0xFE23ACAC, "Someone", "Light.esp"), tables) == "light_one_CAC");
    }
    SECTION("lets a unique row beat the name, and the generic table")
    {
        auto facts = Actor(0x00039FB7, "Refugee");
        CHECK(BioName(facts, tables) == "courier_FB7");
    }
    SECTION("uses the generic table on the trimmed name, from any plugin")
    {
        CHECK(BioName(Actor(0x4C00B503, " Refugee ", "JKs Skyrim.esp"), tables) == "deceased_refugee_503");
    }
    SECTION("falls back to the base name when the display name is empty")
    {
        auto facts = Actor(0x0004E0B3, "");
        facts.baseName = "Grelka";
        CHECK(BioName(facts, tables) == "grelka_0B3");
    }
    SECTION("repairs the name before slugging it")
    {
        CHECK(BioName(Actor(0x00000ABC,
                            "Fl\xFC"
                            "chtling"),
                      tables)
              == "fl\xC3\xBC"
                 "chtling_ABC");
    }
    SECTION("names the player player_special")
    {
        CHECK(BioName(Actor(kPlayerRefId, "Prisoner"), tables) == "player_special");
    }
    SECTION("assigns nothing to an actor with no name, or one named Unknown")
    {
        CHECK(BioName(Actor(0x00000ABC, ""), tables).empty());
        CHECK(BioName(Actor(0x00000ABC, "Unknown"), tables).empty());
    }
}
