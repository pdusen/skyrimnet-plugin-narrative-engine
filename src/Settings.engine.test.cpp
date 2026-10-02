#include <Settings.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

// Tests for the INI cascade.
//
// Engine-free in substance — SimpleIni over two files on disk — but it includes
// <logger.h>, which is `SKSE::log`, so it builds in the mocked target. It
// needed no engine stand-ins.
//
// It does need the disk, and that is not something to mock away: the whole
// module is about which of two real files wins on a given key, and a fake
// filesystem would be testing the fake. The fixture below writes the two INIs
// at the exact relative paths production reads, then removes them, so a run
// leaves nothing behind. The paths are relative to the working directory, which
// for the test binary is the build directory rather than a Skyrim install.
//
// The state is a process-global singleton with no reset, so every case reloads
// through the same public entry point production uses and asserts against what
// it finds. Cases that write are careful to leave the files as they found them.

namespace
{
    namespace Settings = NarrativeEngine::Settings;

    const std::filesystem::path kPluginIni{"Data/SKSE/Plugins/NarrativeEngine.ini"};
    const std::filesystem::path kMcmIni{"Data/MCM/Settings/NarrativeEngine.ini"};

    void WriteIni(const std::filesystem::path& path, const std::string& contents)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        out << contents;
    }

    std::string ReadFile(const std::filesystem::path& path)
    {
        std::ifstream in{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }

    // Starts each case from "no configuration at all" and leaves the tree clean
    // whether the case passed or threw.
    //
    // The FILES go away but the DIRECTORIES stay, because that is what a real
    // install looks like: the mod's own folders ship with it and MCM Helper
    // creates its Settings directory. Deleting them made the environment
    // unlike production and turned a write into a logged failure, which is a
    // property of the missing directory rather than of the module.
    struct IniFixture
    {
        IniFixture()
        {
            RemoveFiles();
            std::filesystem::create_directories(kPluginIni.parent_path());
            std::filesystem::create_directories(kMcmIni.parent_path());
        }
        ~IniFixture()
        {
            RemoveFiles();
            PruneDirectories();
            // Leave the singleton holding defaults rather than whatever the
            // last case wrote, since the next one may not call Load itself.
            Settings::Load();
        }

        static void RemoveFiles()
        {
            std::error_code ec;
            std::filesystem::remove(kPluginIni, ec);
            std::filesystem::remove(kMcmIni, ec);
        }

        // Deepest first. `remove` on a non-empty directory fails harmlessly,
        // so this can only take away what these tests put there.
        static void PruneDirectories()
        {
            std::error_code ec;
            for (const char* dir : {"Data/MCM/Settings", "Data/MCM", "Data/SKSE/Plugins", "Data/SKSE", "Data"}) {
                std::filesystem::remove(dir, ec);
            }
        }
    };
} // namespace

TEST_CASE("Settings::Load reads the plugin INI", "[Settings][engine]")
{
    // Happy path, re-run per leaf: no files on disk, so every case writes only
    // the keys it is about and everything else stands at its baked-in default.
    IniFixture files;

    SECTION("when no INI exists at all")
    {
        Settings::Load();

        SECTION("should fall back to the baked-in defaults")
        {
            // The plugin is documented as fully functional with no INI, so
            // this is a supported configuration rather than a degraded one.
            REQUIRE(Settings::Get().tickIntervalSeconds == 90);
            REQUIRE_FALSE(Settings::Get().debugMode);
        }
    }

    SECTION("when the plugin INI sets a value")
    {
        WriteIni(kPluginIni, "[Director]\niTickIntervalSeconds=45\n\n[General]\nbDebugMode=1\n");
        Settings::Load();

        SECTION("should take the integer from it")
        {
            REQUIRE(Settings::Get().tickIntervalSeconds == 45);
        }

        SECTION("should take the boolean from it")
        {
            REQUIRE(Settings::Get().debugMode);
        }

        SECTION("should leave keys it does not mention at their defaults")
        {
            REQUIRE(Settings::Get().beatCooldownSeconds == Settings::Config{}.beatCooldownSeconds);
        }
    }

    SECTION("when a string setting is present")
    {
        WriteIni(kPluginIni, "[CombatEvents]\nsSpellNameBlocklist=Healing;Muffle\n");
        Settings::Load();

        SECTION("should read it verbatim")
        {
            REQUIRE(Settings::Get().spellNameBlocklist == "Healing;Muffle");
        }
    }
}

TEST_CASE("Settings holds the measured visit-arrival defaults", "[Settings][engine]")
{
    // These three are outputs of a measurement rather than preferences, so a
    // silent edit to one should not pass. Phase 14's Step 6 probe ran seventy
    // arrival searches across plain, forest, mountain pass and four cities,
    // and what it found is written up under "Arrivals beyond the numbered
    // plan" in that phase's doc. Change a number here only with a newer
    // measurement, and update the doc in the same commit.
    IniFixture files;
    Settings::Load();

    SECTION("should keep the cover silhouette at one actor's width")
    {
        // 64 units is a body, which is what the gate is nominally asking
        // about. The probe's finding was that widening it is not the lever:
        // under 2000 units the gate already rejects 97% of road candidates,
        // and past that it passes nearly everything whatever width it is
        // given, because range and not silhouette is what defeats it.
        REQUIRE(Settings::Get().visitArrivalCoverRadiusUnits == 64);
    }

    SECTION("should keep the floor far enough out to settle the direction")
    {
        // Both were parsed and never read before Phase 14. Since Phase 16
        // the floor is the only distance rule the road path has, and it is
        // doing two jobs: a visitor has to arrive far enough out to read as
        // having walked there, and far enough out that hop expansion cannot
        // reach a node across the player. Two hops along a road ribbon is a
        // measured 1,784 units at worst across all of vanilla, so 2,000
        // settles the second by itself and no directional test is needed. The
        // ceiling is the city approach's alone now.
        REQUIRE(Settings::Get().visitMarkerMinDistanceUnits == 2000);
        REQUIRE(Settings::Get().visitMarkerMaxDistanceUnits == 5000);
    }

    SECTION("should leave the bearing fallback switched on")
    {
        // Tier 2 carried 8 of 70 searches in the probe. Off, those are
        // declines.
        REQUIRE(Settings::Get().visitArrivalAllowCoarseBearing);
    }
}

TEST_CASE("Settings leaves the road graphs the visit chain needs switched on", "[Settings][engine]")
{
    // Both shipped off while they were diagnostics that nothing consumed.
    // Phase 16 made them load bearing: TravelGraph is the long-distance
    // skeleton an arriving visitor crosses the map on, FineRoads is the dense
    // near end of the same chain and the only source of road nodes the escort
    // can hop a stuck visitor along. Off, a visit still happens -- it just
    // routes on the straight line from the visitor's home to the player and
    // arrives having walked through whatever lay between.
    //
    // That failure is invisible in the game and quiet in the log, which is
    // why it is pinned here rather than left to the INI.
    IniFixture files;
    Settings::Load();

    SECTION("should build the long-distance skeleton")
    {
        REQUIRE(Settings::Get().travelGraphEnabled);
    }

    SECTION("should build the local road graph")
    {
        REQUIRE(Settings::Get().fineRoadsEnabled);
    }

    SECTION("should leave both bitmaps off")
    {
        // The graphs are not diagnostics any more, so their diagnostics do
        // not belong in everybody's log directory.
        REQUIRE_FALSE(Settings::Get().travelGraphDebugBitmap);
        REQUIRE_FALSE(Settings::Get().fineRoadsDebugBitmap);
    }
}

TEST_CASE("Settings MCM override wins over the plugin INI", "[Settings][engine]")
{
    // The cascade is the module's reason for existing: the plugin INI carries
    // author defaults, and the MCM file is the player's universal override.
    IniFixture files;

    SECTION("when both files set the same key")
    {
        WriteIni(kPluginIni, "[Director]\niTickIntervalSeconds=45\n");
        WriteIni(kMcmIni, "[Director]\niTickIntervalSeconds=90\n");
        Settings::Load();

        SECTION("should take the MCM value")
        {
            REQUIRE(Settings::Get().tickIntervalSeconds == 90);
        }
    }

    SECTION("when only the plugin INI sets a key")
    {
        WriteIni(kPluginIni, "[Director]\niTickIntervalSeconds=45\n");
        WriteIni(kMcmIni, "[General]\nbDebugMode=1\n");
        Settings::Load();

        SECTION("should keep the plugin value")
        {
            // Absent keys fall through rather than resetting to the default,
            // which is what makes the MCM file an override layer rather than a
            // replacement.
            REQUIRE(Settings::Get().tickIntervalSeconds == 45);
        }

        SECTION("should still apply the MCM keys that are present")
        {
            REQUIRE(Settings::Get().debugMode);
        }
    }

    SECTION("when the MCM INI is absent")
    {
        WriteIni(kPluginIni, "[Director]\niTickIntervalSeconds=45\n");
        Settings::Load();

        SECTION("should leave the plugin values standing")
        {
            // A fresh install where the player has never opened the MCM page.
            REQUIRE(Settings::Get().tickIntervalSeconds == 45);
        }
    }

    SECTION("when the MCM INI changes after load")
    {
        WriteIni(kPluginIni, "[Director]\niTickIntervalSeconds=45\n");
        Settings::Load();
        WriteIni(kMcmIni, "[Director]\niTickIntervalSeconds=120\n");
        Settings::ApplyMcmOverride();

        SECTION("should pick the change up without a full reload")
        {
            // What the MCM page's OnSettingChange path relies on: the player
            // moves a slider and the value takes effect on the next tick.
            REQUIRE(Settings::Get().tickIntervalSeconds == 120);
        }
    }
}

TEST_CASE("Settings dashboard hotkey modifiers", "[Settings][engine]")
{
    // Two spellings exist for the same setting: MCM Helper's schema writes
    // three booleans, while the plugin INI carries a packed bitmask. The reader
    // has to accept both and produce one value.
    IniFixture files;

    SECTION("when the MCM boolean keys are present")
    {
        WriteIni(kPluginIni, "[Dashboard]\nbHotkeyShift=1\nbHotkeyAlt=1\n");
        Settings::Load();

        SECTION("should pack them into the bitmask")
        {
            REQUIRE(Settings::Get().dashboardHotkeyModifiers
                    == static_cast<std::uint8_t>(Settings::kModShift | Settings::kModAlt));
        }
    }

    SECTION("when only some of the boolean keys are present")
    {
        WriteIni(kPluginIni, "[Dashboard]\nbHotkeyCtrl=1\n");
        Settings::Load();

        SECTION("should treat the absent ones as off")
        {
            // Presence of ANY of the three switches the reader into boolean
            // mode, so the other two must read as false rather than falling
            // back to whatever the bitmask said.
            REQUIRE(Settings::Get().dashboardHotkeyModifiers == Settings::kModCtrl);
        }
    }

    SECTION("when no boolean key is present")
    {
        WriteIni(kPluginIni, "[Dashboard]\niHotkeyModifiers=3\n");
        Settings::Load();

        SECTION("should fall back to the packed bitmask")
        {
            // Backwards compatibility for a plugin INI written before the MCM
            // page existed.
            REQUIRE(Settings::Get().dashboardHotkeyModifiers == 3);
        }
    }

    SECTION("when the boolean keys are present alongside the bitmask")
    {
        WriteIni(kPluginIni, "[Dashboard]\niHotkeyModifiers=7\nbHotkeyShift=1\n");
        Settings::Load();

        SECTION("should let the booleans win")
        {
            REQUIRE(Settings::Get().dashboardHotkeyModifiers == Settings::kModShift);
        }
    }
}

TEST_CASE("Settings clamps the ambush attacker counts", "[Settings][engine]")
{
    // The load-bearing clamp: eight Attacker0N aliases are authored on
    // _ne_AmbushQuest, so a request for nine has nowhere to go. The ESP and the
    // code cannot drift apart silently because the ceiling is the same
    // constant both sides use.
    IniFixture files;

    SECTION("when the INI asks for more attackers than there are alias slots")
    {
        WriteIni(kPluginIni, "[Beats]\niAmbushMaxAttackerCount=99\n");
        Settings::Load();

        SECTION("should clamp to the authored slot count")
        {
            REQUIRE(Settings::Get().ambushMaxAttackerCount == Settings::kAmbushAttackerSlotCount);
        }
    }

    SECTION("when the INI asks for fewer than one attacker")
    {
        WriteIni(kPluginIni, "[Beats]\niAmbushMinAttackerCount=0\n");
        Settings::Load();

        SECTION("should raise the minimum to one")
        {
            REQUIRE(Settings::Get().ambushMinAttackerCount == 1);
        }
    }

    SECTION("when the maximum is below the minimum")
    {
        WriteIni(kPluginIni, "[Beats]\niAmbushMinAttackerCount=5\niAmbushMaxAttackerCount=2\n");
        Settings::Load();

        SECTION("should raise the maximum to meet it")
        {
            // An inverted range would otherwise produce an empty attacker
            // count and an ambush with nobody in it.
            REQUIRE(Settings::Get().ambushMaxAttackerCount >= Settings::Get().ambushMinAttackerCount);
        }
    }

    SECTION("when the default sits outside the clamped range")
    {
        WriteIni(kPluginIni,
                 "[Beats]\niAmbushMinAttackerCount=4\niAmbushMaxAttackerCount=6\niAmbushDefaultAttackerCount=1\n");
        Settings::Load();

        SECTION("should pull it inside")
        {
            REQUIRE(Settings::Get().ambushDefaultAttackerCount >= Settings::Get().ambushMinAttackerCount);
            REQUIRE(Settings::Get().ambushDefaultAttackerCount <= Settings::Get().ambushMaxAttackerCount);
        }
    }
}

TEST_CASE("Settings::IsSpellNameBlocked", "[Settings][engine]")
{
    // The blocklist is authored as one semicolon-separated string and matched
    // case-insensitively, because the names come from spell records whose
    // capitalization nobody controls.
    IniFixture files;

    SECTION("when the blocklist names a spell")
    {
        WriteIni(kPluginIni, "[CombatEvents]\nsSpellNameBlocklist=Healing; Muffle ;Candlelight\n");
        Settings::Load();

        SECTION("should block it")
        {
            REQUIRE(Settings::IsSpellNameBlocked("Healing"));
        }

        SECTION("should ignore the case it was written in")
        {
            REQUIRE(Settings::IsSpellNameBlocked("hEaLiNg"));
        }

        SECTION("should trim the space around each entry")
        {
            // Authors write the list with spaces after the semicolons; without
            // trimming, " Muffle " would never match anything.
            REQUIRE(Settings::IsSpellNameBlocked("Muffle"));
        }

        SECTION("should not block a spell that is not listed")
        {
            REQUIRE_FALSE(Settings::IsSpellNameBlocked("Fireball"));
        }
    }

    SECTION("when the blocklist is empty")
    {
        Settings::Load();

        SECTION("should block nothing")
        {
            REQUIRE_FALSE(Settings::IsSpellNameBlocked("Healing"));
        }
    }

    SECTION("when the blocklist is replaced by an override")
    {
        WriteIni(kPluginIni, "[CombatEvents]\nsSpellNameBlocklist=Healing\n");
        Settings::Load();
        WriteIni(kMcmIni, "[CombatEvents]\nsSpellNameBlocklist=Fireball\n");
        Settings::ApplyMcmOverride();

        SECTION("should block the new list")
        {
            REQUIRE(Settings::IsSpellNameBlocked("Fireball"));
        }

        SECTION("should stop blocking the old one")
        {
            // The parsed set is rebuilt on every path that mutates the string,
            // so a stale entry here would mean a rebuild was missed.
            REQUIRE_FALSE(Settings::IsSpellNameBlocked("Healing"));
        }
    }
}

TEST_CASE("Settings::IsSenderBlacklisted", "[Settings][engine]")
{
    // One comma-separated list, tested against two different things. An entry
    // is never declared to be an EditorID or a name — whichever it happens to
    // be, it matches — because the arm that can fire depends on whether the
    // player has an EditorID-recovery mod installed, and the list should not
    // have to be rewritten when that changes.
    IniFixture files;

    SECTION("when the list names senders")
    {
        WriteIni(kPluginIni, "[Beats]\nsBlacklistedSenders=WICourierNPC; Nazeem ;Ysolda; Rothgar, the Unbowed\n");
        Settings::Load();

        SECTION("should match on the EditorID")
        {
            REQUIRE(Settings::IsSenderBlacklisted("WICourierNPC", "Courier")
                    == Settings::SenderBlacklistMatch::EditorID);
        }

        SECTION("should match on the display name")
        {
            REQUIRE(Settings::IsSenderBlacklisted("WEJS02Farmer", "Nazeem")
                    == Settings::SenderBlacklistMatch::DisplayName);
        }

        SECTION("should match a name when no EditorID resolved")
        {
            // The common case on an install without an EditorID-recovery mod:
            // GetFormEditorID() hands back nothing at all and the name arm is
            // the only one left.
            REQUIRE(Settings::IsSenderBlacklisted("", "Ysolda") == Settings::SenderBlacklistMatch::DisplayName);
        }

        SECTION("should ignore the case each was written in")
        {
            REQUIRE(Settings::IsSenderBlacklisted("wicouriernpc", "") == Settings::SenderBlacklistMatch::EditorID);
            REQUIRE(Settings::IsSenderBlacklisted("", "nAzEeM") == Settings::SenderBlacklistMatch::DisplayName);
        }

        SECTION("should trim the space around each entry")
        {
            // " Nazeem " is what a player writing a list with spaces after the
            // semicolons actually produces.
            REQUIRE(Settings::IsSenderBlacklisted("", "Nazeem") == Settings::SenderBlacklistMatch::DisplayName);
        }

        SECTION("should report the EditorID when both arms match")
        {
            // Nothing turns on which is reported except the log line, but the
            // log line is the whole way a surprising exclusion gets diagnosed.
            REQUIRE(Settings::IsSenderBlacklisted("Ysolda", "Ysolda") == Settings::SenderBlacklistMatch::EditorID);
        }

        SECTION("should keep an entry containing a comma whole")
        {
            // What the semicolon buys over a comma. No vanilla NPC is named
            // with one, but a mod-added NPC can be, and splitting the entry
            // would leave two halves that match nobody.
            REQUIRE(Settings::IsSenderBlacklisted("", "Rothgar, the Unbowed")
                    == Settings::SenderBlacklistMatch::DisplayName);
        }

        SECTION("should not match someone the list does not name")
        {
            REQUIRE(Settings::IsSenderBlacklisted("Belethor", "Belethor") == Settings::SenderBlacklistMatch::None);
        }
    }

    SECTION("when the list is empty")
    {
        Settings::Load();

        SECTION("should match nobody")
        {
            REQUIRE(Settings::IsSenderBlacklisted("WICourierNPC", "Courier") == Settings::SenderBlacklistMatch::None);
        }

        SECTION("should match nobody even when both arguments are empty")
        {
            // An actor with no base form and a nameless engagement row reaches
            // the query as two empty strings; it must not match an entry that
            // trimmed down to nothing.
            REQUIRE(Settings::IsSenderBlacklisted("", "") == Settings::SenderBlacklistMatch::None);
        }
    }

    SECTION("when the list is replaced by an override")
    {
        WriteIni(kPluginIni, "[Beats]\nsBlacklistedSenders=Ysolda\n");
        Settings::Load();
        WriteIni(kMcmIni, "[Beats]\nsBlacklistedSenders=Nazeem\n");
        Settings::ApplyMcmOverride();

        SECTION("should match the new list")
        {
            REQUIRE(Settings::IsSenderBlacklisted("", "Nazeem") == Settings::SenderBlacklistMatch::DisplayName);
        }

        SECTION("should stop matching the old one")
        {
            // The parsed set is rebuilt on every path that mutates the string,
            // so a stale entry here would mean a rebuild was missed.
            REQUIRE(Settings::IsSenderBlacklisted("", "Ysolda") == Settings::SenderBlacklistMatch::None);
        }
    }
}

TEST_CASE("Settings::WriteMcmOverride", "[Settings][engine]")
{
    IniFixture files;

    SECTION("when a value is written")
    {
        Settings::Load();
        Settings::McmOverride mutations;
        mutations.tickIntervalSeconds = 77;
        Settings::WriteMcmOverride(mutations);

        SECTION("should update the in-memory config immediately")
        {
            // No wait for a subsequent Load: the dashboard expects the change
            // it just made to be visible on the next read.
            REQUIRE(Settings::Get().tickIntervalSeconds == 77);
        }

        SECTION("should persist it to the MCM INI")
        {
            REQUIRE(ReadFile(kMcmIni).find("77") != std::string::npos);
        }

        SECTION("should survive a reload")
        {
            Settings::Load();
            REQUIRE(Settings::Get().tickIntervalSeconds == 77);
        }
    }

    SECTION("when only some fields are engaged")
    {
        Settings::Load();
        Settings::McmOverride mutations;
        mutations.debugMode = true;
        Settings::WriteMcmOverride(mutations);

        SECTION("should leave the others alone")
        {
            // The optionals are the whole interface: an unengaged one means
            // "do not touch", not "write the default".
            REQUIRE(Settings::Get().debugMode);
            REQUIRE(Settings::Get().tickIntervalSeconds == 90);
        }
    }

    SECTION("when the MCM settings directory does not exist")
    {
        // A player with no MCM Helper installed. SimpleIni will not create the
        // directory, so the save fails and the module logs it.
        Settings::Load();
        std::error_code ec;
        std::filesystem::remove(kMcmIni, ec);
        std::filesystem::remove("Data/MCM/Settings", ec);
        std::filesystem::remove("Data/MCM", ec);

        Settings::McmOverride mutations;
        mutations.tickIntervalSeconds = 55;
        Settings::WriteMcmOverride(mutations);

        SECTION("should still apply the change in memory")
        {
            // The write is best-effort; the in-memory update is not. A
            // dashboard toggle has to take effect this session even when it
            // cannot be persisted for the next one.
            REQUIRE(Settings::Get().tickIntervalSeconds == 55);
        }

        std::filesystem::create_directories(kMcmIni.parent_path());
    }

    SECTION("when the MCM INI already holds keys the writer does not know")
    {
        WriteIni(kMcmIni, "[Director]\niTickIntervalSeconds=45\n\n[Unknown]\nsSomethingElse=keep me\n");
        Settings::Load();
        Settings::McmOverride mutations;
        mutations.debugMode = true;
        Settings::WriteMcmOverride(mutations);

        SECTION("should preserve them")
        {
            // The write is a read-modify-write of the player's own file. Losing
            // keys the plugin does not recognize would eat settings belonging
            // to a future version or another tool.
            REQUIRE(ReadFile(kMcmIni).find("keep me") != std::string::npos);
        }
    }
}

TEST_CASE("Settings per-beat enable flags", "[Settings][engine]")
{
    // Addressed by the beat's own name rather than by struct field, because the
    // callers that need them only have a name: the dashboard hands back
    // {"name":"npc_visit","enabled":false} with no way to pick a field.
    IniFixture files;

    SECTION("when the beat has a registered enable key")
    {
        WriteIni(kPluginIni, "[Beats]\nbEnableNpcLetter=0\n");
        Settings::Load();

        SECTION("should report the configured state")
        {
            REQUIRE_FALSE(Settings::GetBeatEnabled("npc_letter", true));
        }

        SECTION("should ignore the caller's fallback")
        {
            // The fallback is for beats with no key at all, not a default to
            // blend with a configured value.
            REQUIRE_FALSE(Settings::GetBeatEnabled("npc_letter", true));
        }
    }

    SECTION("when the beat has no registered enable key")
    {
        Settings::Load();

        SECTION("should return the caller's fallback")
        {
            REQUIRE(Settings::GetBeatEnabled("no_such_beat", true));
            REQUIRE_FALSE(Settings::GetBeatEnabled("no_such_beat", false));
        }

        SECTION("should refuse to persist a toggle for it")
        {
            REQUIRE_FALSE(Settings::WriteBeatEnabledOverride("no_such_beat", false));
        }
    }

    SECTION("when a toggle is persisted")
    {
        Settings::Load();
        const bool wrote = Settings::WriteBeatEnabledOverride("npc_visit", false);

        SECTION("should report success")
        {
            REQUIRE(wrote);
        }

        SECTION("should update the in-memory config")
        {
            REQUIRE_FALSE(Settings::GetBeatEnabled("npc_visit", true));
        }

        SECTION("should survive a reload")
        {
            // The read path and the write path share one name-to-key table, so
            // a round trip proves they have not drifted apart.
            Settings::Load();
            REQUIRE_FALSE(Settings::GetBeatEnabled("npc_visit", true));
        }
    }
}
