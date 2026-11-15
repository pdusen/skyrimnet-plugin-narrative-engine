#include <CharacterBioResolver.h>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

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

    const IsLightPlugin kNoLightPlugins = [](std::string_view) { return false; };
    const FormName kNoNames = [](std::string_view, std::uint32_t) { return std::string{}; };

    constexpr std::string_view kUniqueHeader = "\"pluginName\",\"refId\",\"sanitizedEnglishKey\"\n";
    constexpr std::string_view kGenericHeader = "\"pluginName\",\"formId\",\"sanitizedEnglishKey\"\n";

    CsvFile Unique(std::string path, std::string_view rows)
    {
        return CsvFile{std::move(path), std::string{kUniqueHeader} + std::string{rows}};
    }

    CsvFile Generic(std::string path, std::string_view rows)
    {
        return CsvFile{std::move(path), std::string{kGenericHeader} + std::string{rows}};
    }

    ActorFacts Actor(std::uint32_t refId, std::string displayName, std::string basePlugin = "Skyrim.esm")
    {
        ActorFacts facts;
        facts.refId = refId;
        facts.displayName = std::move(displayName);
        facts.basePlugin = std::move(basePlugin);
        return facts;
    }

    Layer MakeLayer(LayerKind kind,
                    std::string folder,
                    std::initializer_list<std::string> paths,
                    std::string version = "1.0.0",
                    std::string manifestId = {})
    {
        Layer layer;
        layer.kind = kind;
        layer.manifestId = manifestId.empty() ? folder : std::move(manifestId);
        layer.folder = std::move(folder);
        layer.version = std::move(version);
        layer.paths.insert(paths.begin(), paths.end());
        return layer;
    }

    constexpr std::string_view kBio = "prompts/characters/grelka_0B3.prompt";

    // A registry naming `ids`, highest first, all enabled.
    std::string RegistryJson(std::initializer_list<std::string_view> ids, std::string_view pins = "{}")
    {
        std::string plugins;
        std::string priority;
        for (const auto id : ids) {
            plugins += std::string{plugins.empty() ? "" : ","} + "\"" + std::string{id}
                       + "\":{\"enabled\":true,\"source\":\"external\"}";
            priority += std::string{priority.empty() ? "" : ","} + "\"" + std::string{id} + "\"";
        }
        return "{\"schema_version\":1,\"plugins\":{" + plugins + "},\"priority\":[" + priority
               + "],\"pins\":" + std::string{pins} + ",\"items\":{}}";
    }

    std::string WinnerFolder(const std::vector<Layer>& layers, const Registry& registry, std::string_view path = kBio)
    {
        const auto stack = BuildStack(layers, registry);
        const auto* winner = Winner(stack, path);
        return winner == nullptr ? std::string{"<none>"} : winner->folder;
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

TEST_CASE("CharacterBioResolver::ParseHexId", "[CharacterBioResolver][Tables]")
{
    CHECK(ParseHexId("019DDC") == 0x019DDCu);
    CHECK(ParseHexId(" 0x19ddc ") == 0x019DDCu);
    CHECK(ParseHexId("0X19DDC") == 0x019DDCu);
    CHECK(ParseHexId("FFFFFFFF") == 0xFFFFFFFFu);
    CHECK_FALSE(ParseHexId("").has_value());
    CHECK_FALSE(ParseHexId("0x").has_value());
    CHECK_FALSE(ParseHexId("123456789").has_value());
    CHECK_FALSE(ParseHexId("19DDG").has_value());
    CHECK_FALSE(ParseHexId("refId").has_value());
}

TEST_CASE("CharacterBioResolver::BuildTables", "[CharacterBioResolver][Tables]")
{
    SECTION("reads quoted rows and skips the header")
    {
        const auto tables =
            BuildTables({Unique("prompts/translation/unique/00.csv", "\"Skyrim.esm\",\"019DDC\",\"brandish\"\r\n")},
                        {},
                        kNoLightPlugins,
                        kNoNames);
        REQUIRE(tables.unique.size() == 1);
        CHECK(tables.unique.at(UniqueKey("skyrim.esm", 0x019DDC)) == "brandish");
    }
    SECTION("lets a later file override one row of an earlier one and leaves the rest")
    {
        // Passed out of order on purpose: path order decides, not argument order.
        TableCensus census;
        const auto tables =
            BuildTables({Unique("prompts/translation/unique/50_mod.csv", "Skyrim.esm,019DDC,brand_shei\n"),
                         Unique("prompts/translation/unique/00_base.csv",
                                "Skyrim.esm,019DDC,brandish\nSkyrim.esm,039FB7,courier\n")},
                        {},
                        kNoLightPlugins,
                        kNoNames,
                        &census);
        CHECK(tables.unique.at(UniqueKey("Skyrim.esm", 0x019DDC)) == "brand_shei");
        CHECK(tables.unique.at(UniqueKey("Skyrim.esm", 0x039FB7)) == "courier");
        CHECK(census.uniqueRows == 3);
    }
    SECTION("skips a malformed row or a bad id without dropping the file")
    {
        TableCensus census;
        const auto tables = BuildTables({Unique("u.csv",
                                                "# a comment,000001,x\n"
                                                "\n"
                                                "Skyrim.esm,000002\n"
                                                "Skyrim.esm,not-hex,x\n"
                                                "Skyrim.esm,000003,   \n"
                                                ",000004,x\n"
                                                "Skyrim.esm,000005,kept\n")},
                                        {},
                                        kNoLightPlugins,
                                        kNoNames,
                                        &census);
        CHECK(tables.unique.size() == 1);
        CHECK(tables.unique.contains(UniqueKey("Skyrim.esm", 0x000005)));
        CHECK(census.skipped == 6);
    }
    SECTION("masks a light plugin's ids to twelve bits")
    {
        const IsLightPlugin lightIsLight = [](std::string_view plugin) { return plugin == "Light.esp"; };
        const auto tables = BuildTables(
            {Unique("u.csv", "Light.esp,FE000CAC,light_one\nHeavy.esp,01000CAC,heavy\n")}, {}, lightIsLight, kNoNames);
        CHECK(tables.unique.contains(UniqueKey("Light.esp", 0xCAC)));
        CHECK(tables.unique.contains(UniqueKey("Heavy.esp", 0x000CAC)));
    }
    SECTION("keys the generic table by the slug of each row's trimmed, repaired name")
    {
        const FormName names = [](std::string_view plugin, std::uint32_t formId) -> std::string {
            if (plugin == "Skyrim.esm" && formId == 0x0745CD) {
                return " Refugee ";
            }
            if (plugin == "Mod.esp" && formId == 0x000ABC) {
                return "Fl\xFC"
                       "chtling";
            }
            return {};
        };
        TableCensus census;
        const auto tables = BuildTables(
            {},
            {Generic("g.csv", "Skyrim.esm,0745CD,deceased_refugee\nMod.esp,000ABC,refugee\nGone.esp,000001,x\n")},
            kNoLightPlugins,
            names,
            &census);
        CHECK(tables.generic.at("refugee") == "deceased_refugee");
        CHECK(tables.generic.at("fl\xC3\xBC"
                                "chtling")
              == "refugee");
        CHECK(census.genericRows == 2);
        CHECK(census.genericUnnamed == 1);
    }
    SECTION("keeps the later of two generic rows with the same name")
    {
        const FormName refugee = [](std::string_view, std::uint32_t) { return std::string{"Refugee"}; };
        const auto tables = BuildTables(
            {}, {Generic("g.csv", "Skyrim.esm,0745CD,first\nSkyrim.esm,07443C,second\n")}, kNoLightPlugins, refugee);
        CHECK(tables.generic.at("refugee") == "second");
    }
}

TEST_CASE("CharacterBioResolver::ParseRegistry", "[CharacterBioResolver][Stack]")
{
    SECTION("reads records, priority and pins")
    {
        const auto registry =
            ParseRegistry(RegistryJson({"a.one", "b.two"}, R"({"prompts/characters/x.prompt":"b.two"})"));
        CHECK(registry.plugins.size() == 2);
        CHECK(registry.priority == std::vector<std::string>{"a.one", "b.two"});
        CHECK(registry.pins.at("prompts/characters/x.prompt") == "b.two");
    }
    SECTION("takes a non-boolean enabled as true, and reads missing")
    {
        const auto registry = ParseRegistry(
            R"({"plugins":{"a.one":{"enabled":"no"},"b.two":{"enabled":false},"c.three":{"missing":true}}})");
        CHECK(registry.plugins.at("a.one").enabled);
        CHECK_FALSE(registry.plugins.at("b.two").enabled);
        CHECK(registry.plugins.at("c.three").missing);
    }
    SECTION("appends records without a priority entry at the bottom, in key order")
    {
        const auto registry = ParseRegistry(
            R"({"plugins":{"c.three":{},"a.one":{},"b.two":{}},"priority":["b.two",7,"b.two","gone.x"]})");
        CHECK(registry.priority == std::vector<std::string>{"b.two", "a.one", "c.three"});
    }
    SECTION("drops a record that collides case-folded, and the reserved base id")
    {
        const auto registry = ParseRegistry(R"({"plugins":{"A.One":{},"a.one":{},"skyrimnet.base":{},"x":5}})");
        CHECK(registry.plugins.size() == 1);
        CHECK(registry.plugins.contains("A.One"));
    }
    SECTION("treats an unreadable or newer-schema file as empty")
    {
        CHECK(ParseRegistry("{not json").plugins.empty());
        CHECK(ParseRegistry(R"({"schema_version":2,"plugins":{"a.one":{}}})").plugins.empty());
    }
}

TEST_CASE("CharacterBioResolver::CompareVersions", "[CharacterBioResolver][Stack]")
{
    CHECK(CompareVersions("1.4.0", "1.10.0") < 0);
    CHECK(CompareVersions("2.0.0", "1.99.99") > 0);
    CHECK(CompareVersions("1.0.0", "v1.0.0+build.7") == 0);
    CHECK(CompareVersions("1.0.0-beta", "1.0.0") < 0);
    CHECK(CompareVersions("1.0.0-alpha.2", "1.0.0-alpha.10") < 0);
    CHECK(CompareVersions("1.0.0-alpha", "1.0.0-alpha.1") < 0);
    CHECK(CompareVersions("", "0.0.0") == 0);
    CHECK(CompareVersions("garbage", "0.0.1") < 0);
}

TEST_CASE("CharacterBioResolver::BuildStack and Winner", "[CharacterBioResolver][Stack]")
{
    const auto bio = std::string{kBio};
    const auto base = MakeLayer(LayerKind::Base, "skyrimnet.base", {bio}, "1.0.0", "skyrimnet.base");

    SECTION("ranks plugins by priority, first highest, above the base")
    {
        const std::vector layers{
            base, MakeLayer(LayerKind::External, "low.mod", {bio}), MakeLayer(LayerKind::Library, "high.mod", {bio})};
        CHECK(WinnerFolder(layers, ParseRegistry(RegistryJson({"high.mod", "low.mod"}))) == "high.mod");
        CHECK(WinnerFolder(layers, ParseRegistry(RegistryJson({"low.mod", "high.mod"}))) == "low.mod");
    }
    SECTION("falls back to the base when no plugin has the file")
    {
        const std::vector layers{base, MakeLayer(LayerKind::External, "a.mod", {"prompts/other.prompt"})};
        CHECK(WinnerFolder(layers, ParseRegistry(RegistryJson({"a.mod"}))) == "skyrimnet.base");
    }
    SECTION("puts the overlay above every plugin")
    {
        const std::vector layers{
            base, MakeLayer(LayerKind::External, "a.mod", {bio}), MakeLayer(LayerKind::Overlay, "overlay", {bio})};
        CHECK(WinnerFolder(layers, ParseRegistry(RegistryJson({"a.mod"}))) == "overlay");
    }
    SECTION("lets a pin beat the overlay")
    {
        const std::vector layers{base,
                                 MakeLayer(LayerKind::External, "a.mod", {bio}),
                                 MakeLayer(LayerKind::External, "b.mod", {bio}),
                                 MakeLayer(LayerKind::Overlay, "overlay", {bio})};
        const auto pins = std::string{"{\""} + bio + "\":\"b.mod\"}";
        CHECK(WinnerFolder(layers, ParseRegistry(RegistryJson({"a.mod", "b.mod"}, pins))) == "b.mod");
    }
    SECTION("lets the save layer beat a pin")
    {
        const std::vector layers{base,
                                 MakeLayer(LayerKind::External, "b.mod", {bio}),
                                 MakeLayer(LayerKind::Save, "1778871594119-52513", {bio})};
        const auto pins = std::string{"{\""} + bio + "\":\"b.mod\"}";
        CHECK(WinnerFolder(layers, ParseRegistry(RegistryJson({"b.mod"}, pins))) == "1778871594119-52513");
    }
    SECTION("falls back to stack order when the pinned plugin lacks the file")
    {
        const std::vector layers{base,
                                 MakeLayer(LayerKind::External, "a.mod", {bio}),
                                 MakeLayer(LayerKind::External, "b.mod", {"prompts/other.prompt"})};
        const auto pins = std::string{"{\""} + bio + "\":\"b.mod\"}";
        CHECK(WinnerFolder(layers, ParseRegistry(RegistryJson({"b.mod", "a.mod"}, pins))) == "a.mod");
    }
    SECTION("ignores a plugin the registry does not list, or lists as disabled or missing")
    {
        const std::vector layers{base,
                                 MakeLayer(LayerKind::External, "unlisted.mod", {bio}),
                                 MakeLayer(LayerKind::External, "off.mod", {bio}),
                                 MakeLayer(LayerKind::External, "gone.mod", {bio})};
        const auto registry = ParseRegistry(
            R"({"plugins":{"off.mod":{"enabled":false},"gone.mod":{"missing":true}},"priority":["off.mod","gone.mod"]})");
        CHECK(WinnerFolder(layers, registry) == "skyrimnet.base");
    }
    SECTION("ranks a registered plugin without a priority entry below every prioritized one")
    {
        const std::vector layers{base,
                                 MakeLayer(LayerKind::External, "a.unranked", {bio}),
                                 MakeLayer(LayerKind::External, "z.ranked", {bio})};
        const auto registry = ParseRegistry(R"({"plugins":{"a.unranked":{},"z.ranked":{}},"priority":["z.ranked"]})");
        CHECK(WinnerFolder(layers, registry) == "z.ranked");
    }
    SECTION("ignores a library folder whose manifest names another id")
    {
        const std::vector layers{base, MakeLayer(LayerKind::Library, "a.mod", {bio}, "1.0.0", "someone.else")};
        CHECK(WinnerFolder(layers, ParseRegistry(RegistryJson({"a.mod"}))) == "skyrimnet.base");
    }
    SECTION("keeps the higher version of one id in two places")
    {
        const std::vector layers{base,
                                 MakeLayer(LayerKind::External, "a.mod", {bio}, "2.0.0"),
                                 MakeLayer(LayerKind::Library, "a.mod", {"prompts/other.prompt"}, "1.5.0")};
        const auto registry = ParseRegistry(RegistryJson({"a.mod"}));
        const auto stack = BuildStack(layers, registry);
        REQUIRE(Winner(stack, bio) != nullptr);
        CHECK(Winner(stack, bio)->kind == LayerKind::External);
        CHECK(Winner(stack, "prompts/other.prompt") == nullptr);
    }
    SECTION("prefers the library copy of one id on a version tie")
    {
        const std::vector layers{base,
                                 MakeLayer(LayerKind::External, "a.mod", {bio}, "1.0.0"),
                                 MakeLayer(LayerKind::Library, "a.mod", {"prompts/other.prompt"}, "1.0.0")};
        const auto stack = BuildStack(layers, ParseRegistry(RegistryJson({"a.mod"})));
        CHECK(Winner(stack, bio)->folder == "skyrimnet.base");
        CHECK(Winner(stack, "prompts/other.prompt")->kind == LayerKind::Library);
    }
    SECTION("takes a newer base update, and the plain base on a tie")
    {
        const auto update = [&](std::string version) {
            return MakeLayer(LayerKind::Base, "skyrimnet.base.update", {bio}, std::move(version), "skyrimnet.base");
        };
        CHECK(WinnerFolder({base, update("1.1.0")}, Registry{}) == "skyrimnet.base.update");
        CHECK(WinnerFolder({update("1.0.0"), base}, Registry{}) == "skyrimnet.base");
    }
    SECTION("matches a path with backslashes, but not one in another case")
    {
        const std::vector layers{base};
        const auto stack = BuildStack(layers, Registry{});
        CHECK(Winner(stack, "prompts\\characters\\grelka_0B3.prompt") != nullptr);
        CHECK(Winner(stack, "prompts/characters/grelka_0b3.prompt") == nullptr);
    }
}

TEST_CASE("CharacterBioResolver::Resolve", "[CharacterBioResolver][Resolve]")
{
    constexpr std::string_view kDefault = "prompts/characters/default.prompt";
    const auto stackOf = [](const std::vector<Layer>& layers) { return BuildStack(layers, Registry{}); };
    const auto base = [&](std::initializer_list<std::string> paths) {
        return MakeLayer(LayerKind::Base, "skyrimnet.base", paths, "1.0.0", "skyrimnet.base");
    };
    Tables tables;
    tables.generic[Slug("Refugee")] = "deceased_refugee";

    auto courier = Actor(0x00039FB7, "Boren Vallkirson [Courier]");
    auto bandit = Actor(0x0001A2B3, "Bandit");
    auto grelka = Actor(0x0004E0B3, "Grelka");
    grelka.baseUnique = true;

    SECTION("prefers a dynamic bio to the actor's own")
    {
        const std::vector layers{
            base({"prompts/characters/grelka_0B3.prompt", std::string{kDefault}}),
            MakeLayer(LayerKind::Save, "save", {"prompts/characters/dynamic/grelka_0B3.dynamic.prompt"})};
        const auto stack = stackOf(layers);
        const auto resolution = Resolve(grelka, "grelka_0B3", tables, stack);
        CHECK(resolution.tier == Tier::Dynamic);
        CHECK(resolution.path == "prompts/characters/dynamic/grelka_0B3.dynamic.prompt");
        CHECK(resolution.layer->folder == "save");
    }
    SECTION("prefers the actor's own file to any generic")
    {
        const std::vector layers{
            base({"prompts/characters/courier_FB7.prompt", "prompts/characters/courier_generic.prompt"})};
        const auto stack = stackOf(layers);
        CHECK(Resolve(courier, "courier_FB7", tables, stack).tier == Tier::Own);
    }
    SECTION("sends a unique actor with no file of their own straight to default")
    {
        const std::vector layers{base({"prompts/characters/grelka_generic.prompt", std::string{kDefault}})};
        const auto stack = stackOf(layers);
        const auto resolution = Resolve(grelka, "grelka_0B3", tables, stack);
        CHECK(resolution.tier == Tier::Default);
        CHECK(resolution.path == kDefault);
    }
    SECTION("resolves a bracketed tag to its _generic")
    {
        const std::vector layers{base({"prompts/characters/courier_generic.prompt", std::string{kDefault}})};
        const auto stack = stackOf(layers);
        const auto resolution = Resolve(courier, "courier_FB7", tables, stack);
        CHECK(resolution.tier == Tier::Generic);
        CHECK(resolution.path == "prompts/characters/courier_generic.prompt");
    }
    SECTION("falls from the tag to the whole name")
    {
        const std::vector layers{
            base({"prompts/characters/boren_vallkirson_[courier]_generic.prompt", std::string{kDefault}})};
        const auto stack = stackOf(layers);
        CHECK(Resolve(courier, "courier_FB7", tables, stack).path
              == "prompts/characters/boren_vallkirson_[courier]_generic.prompt");
    }
    SECTION("resolves an untagged name to its _generic")
    {
        const std::vector layers{base({"prompts/characters/bandit_generic.prompt"})};
        const auto stack = stackOf(layers);
        CHECK(Resolve(bandit, "bandit_2B3", tables, stack).path == "prompts/characters/bandit_generic.prompt");
    }
    SECTION("uses a generic-table hit as it stands, with no suffix")
    {
        const std::vector layers{base({"prompts/characters/deceased_refugee_generic.prompt"})};
        const auto stack = stackOf(layers);
        CHECK(Resolve(Actor(0x4C00B503, "Refugee"), "deceased_refugee_503", tables, stack).path
              == "prompts/characters/deceased_refugee_generic.prompt");
    }
    SECTION("resolves None only when even default is missing")
    {
        const std::vector withDefaultLayers{base({std::string{kDefault}})};
        CHECK(Resolve(bandit, "bandit_2B3", tables, stackOf(withDefaultLayers)).tier == Tier::Default);
        const std::vector emptyLayers{base({})};
        CHECK(Resolve(bandit, "bandit_2B3", tables, stackOf(emptyLayers)).tier == Tier::None);
    }
    SECTION("resolves None for an actor with no bio name, default or not")
    {
        const std::vector layers{base({std::string{kDefault}})};
        CHECK(Resolve(bandit, "", tables, stackOf(layers)).tier == Tier::None);
    }
}
