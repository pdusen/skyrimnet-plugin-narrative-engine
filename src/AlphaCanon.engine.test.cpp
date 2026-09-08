#include <AlphaCanon.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

// Mocked-engine tests for the "vanilla is in charge" predicates.
//
// The Director consults this bitmask every tick before deciding whether to act,
// so a predicate that answers wrong does not crash anything — it just has the
// Director stage a beat in the middle of a scripted scene, or refuse to act
// forever. Both failures are silent, and neither is visible at the call site.
//
// Two of the five degrade OPEN by design: cell and location editor IDs are only
// retained at runtime with powerofthree's Tweaks installed, so without it the
// configured blocklists match nothing rather than everything. That is the safer
// direction and it is pinned below, because the opposite mistake would block
// the Director everywhere for anyone missing a soft dependency.

namespace
{
    using NarrativeEngine::Testing::EngineMock;
    namespace AlphaCanon = NarrativeEngine::AlphaCanon;
    using AlphaCanon::Signal;

    using NarrativeEngine::Testing::ConfiguredSettings;

    bool Contains(const std::vector<std::string>& names, std::string_view wanted)
    {
        return std::find(names.begin(), names.end(), wanted) != names.end();
    }
} // namespace

TEST_CASE("AlphaCanon::IsInActiveCombat", "[AlphaCanon][engine]")
{
    // Happy path, re-run per leaf: a quiet world where nothing is in charge.
    EngineMock engine;

    SECTION("when the player is fighting")
    {
        engine.player.inCombat = true;

        SECTION("should report combat")
        {
            REQUIRE(AlphaCanon::IsInActiveCombat());
        }
    }

    SECTION("when the player is not fighting")
    {
        SECTION("should report no combat")
        {
            REQUIRE_FALSE(AlphaCanon::IsInActiveCombat());
        }
    }

    SECTION("when the player singleton is unavailable")
    {
        // Combat is set, so a false answer here can only be the null guard.
        engine.player.present = false;
        engine.player.inCombat = true;

        SECTION("should report no combat rather than crash")
        {
            REQUIRE_FALSE(AlphaCanon::IsInActiveCombat());
        }
    }
}

TEST_CASE("AlphaCanon::IsInDialogue", "[AlphaCanon][engine]")
{
    EngineMock engine;

    SECTION("when the vanilla dialogue menu is open")
    {
        engine.ui.openMenus.emplace_back(RE::DialogueMenu::MENU_NAME);

        SECTION("should report dialogue")
        {
            REQUIRE(AlphaCanon::IsInDialogue());
        }
    }

    SECTION("when a different menu is open")
    {
        engine.ui.openMenus.emplace_back("InventoryMenu");

        SECTION("should report no dialogue")
        {
            REQUIRE_FALSE(AlphaCanon::IsInDialogue());
        }
    }

    SECTION("when the UI singleton is unavailable")
    {
        engine.ui.present = false;
        engine.ui.openMenus.emplace_back(RE::DialogueMenu::MENU_NAME);

        SECTION("should report no dialogue rather than crash")
        {
            REQUIRE_FALSE(AlphaCanon::IsInDialogue());
        }
    }
}

TEST_CASE("AlphaCanon::IsInScriptedScene", "[AlphaCanon][engine]")
{
    EngineMock engine;

    SECTION("when the player is in a playing scene")
    {
        engine.world.playerInScene = true;
        engine.world.sceneIsPlaying = true;

        SECTION("should report a scripted scene")
        {
            REQUIRE(AlphaCanon::IsInScriptedScene());
        }
    }

    SECTION("when the player is in a scene that is not playing")
    {
        // A scene the player is attached to but which has not started, or has
        // finished. Treating that as "vanilla is in charge" would keep the
        // Director locked out long after the scene ended.
        engine.world.playerInScene = true;
        engine.world.sceneIsPlaying = false;

        SECTION("should report no scripted scene")
        {
            REQUIRE_FALSE(AlphaCanon::IsInScriptedScene());
        }
    }

    SECTION("when the player is in no scene")
    {
        SECTION("should report no scripted scene")
        {
            REQUIRE_FALSE(AlphaCanon::IsInScriptedScene());
        }
    }
}

TEST_CASE("AlphaCanon::IsInDoNotDisturbCell", "[AlphaCanon][engine]")
{
    EngineMock engine;

    SECTION("when the cell is on the configured list")
    {
        const ConfiguredSettings settings{
            "[AlphaCanon]\nsDoNotDisturbCellEDIDsCSV=SomewhereElse, WhiterunBanneredMare ,AndAnother\n"};

        SECTION("should report the cell")
        {
            // The entry is padded with spaces, which is how a hand-edited CSV
            // actually looks. Without trimming it would never match.
            REQUIRE(AlphaCanon::IsInDoNotDisturbCell());
        }
    }

    SECTION("when the cell is not on the list")
    {
        const ConfiguredSettings settings{"[AlphaCanon]\nsDoNotDisturbCellEDIDsCSV=SomewhereElse\n"};

        SECTION("should not report the cell")
        {
            REQUIRE_FALSE(AlphaCanon::IsInDoNotDisturbCell());
        }
    }

    SECTION("when the match differs only in case")
    {
        // Editor IDs are case-sensitive and the comparison is too, so a
        // mis-cased entry silently does nothing. Pinned so the behaviour is a
        // decision rather than a surprise.
        const ConfiguredSettings settings{"[AlphaCanon]\nsDoNotDisturbCellEDIDsCSV=whiterunbanneredmare\n"};

        SECTION("should not match")
        {
            REQUIRE_FALSE(AlphaCanon::IsInDoNotDisturbCell());
        }
    }

    SECTION("when no list is configured")
    {
        const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"};

        SECTION("should report nothing")
        {
            REQUIRE_FALSE(AlphaCanon::IsInDoNotDisturbCell());
        }
    }

    SECTION("when the cell has no editor id")
    {
        // The no-Tweaks case. The list is configured and would match, so a
        // false answer here is the degrade-open path rather than a miss.
        const ConfiguredSettings settings{"[AlphaCanon]\nsDoNotDisturbCellEDIDsCSV=WhiterunBanneredMare\n"};
        engine.world.cellEditorID.clear();

        SECTION("should degrade open rather than block")
        {
            REQUIRE_FALSE(AlphaCanon::IsInDoNotDisturbCell());
        }
    }

    SECTION("when the player has no cell")
    {
        const ConfiguredSettings settings{"[AlphaCanon]\nsDoNotDisturbCellEDIDsCSV=WhiterunBanneredMare\n"};
        engine.world.playerHasCell = false;

        SECTION("should report nothing")
        {
            REQUIRE_FALSE(AlphaCanon::IsInDoNotDisturbCell());
        }
    }
}

TEST_CASE("AlphaCanon::IsInBlacklistedLocation", "[AlphaCanon][engine]")
{
    EngineMock engine;

    SECTION("when the player's location is listed")
    {
        const ConfiguredSettings settings{"[AlphaCanon]\nsBlacklistedLocationEDIDsCSV=WhiterunLocation\n"};

        SECTION("should report the location")
        {
            REQUIRE(AlphaCanon::IsInBlacklistedLocation());
        }
    }

    SECTION("when only a different location is listed")
    {
        const ConfiguredSettings settings{"[AlphaCanon]\nsBlacklistedLocationEDIDsCSV=SovngardeLocation\n"};

        SECTION("should report nothing")
        {
            REQUIRE_FALSE(AlphaCanon::IsInBlacklistedLocation());
        }
    }

    SECTION("when only an ancestor location is listed")
    {
        // The reason the walk exists. SovngardeHallofHeroesLocation's
        // ParentLocation is SovngardeLocation, so blacklisting the parent has
        // to block the child without naming it -- otherwise every interior of
        // every blacklisted place would have to be listed by hand.
        const ConfiguredSettings settings{"[AlphaCanon]\nsBlacklistedLocationEDIDsCSV=SovngardeLocation\n"};
        engine.world.locationEditorID = "SovngardeHallofHeroesLocation";
        engine.world.locationParentEditorID = "SovngardeLocation";

        SECTION("should report the location")
        {
            REQUIRE(AlphaCanon::IsInBlacklistedLocation());
        }
    }

    SECTION("when neither the location nor its ancestor is listed")
    {
        const ConfiguredSettings settings{"[AlphaCanon]\nsBlacklistedLocationEDIDsCSV=BleakFallsBarrowLocation\n"};
        engine.world.locationEditorID = "SovngardeHallofHeroesLocation";
        engine.world.locationParentEditorID = "SovngardeLocation";

        SECTION("should report nothing")
        {
            REQUIRE_FALSE(AlphaCanon::IsInBlacklistedLocation());
        }
    }

    SECTION("when the player is in unmarked wilderness")
    {
        const ConfiguredSettings settings{"[AlphaCanon]\nsBlacklistedLocationEDIDsCSV=WhiterunLocation\n"};
        engine.world.playerHasLocation = false;

        SECTION("should report nothing")
        {
            REQUIRE_FALSE(AlphaCanon::IsInBlacklistedLocation());
        }
    }

    SECTION("when the location has no editor id")
    {
        const ConfiguredSettings settings{"[AlphaCanon]\nsBlacklistedLocationEDIDsCSV=WhiterunLocation\n"};
        engine.world.locationEditorID.clear();

        SECTION("should degrade open rather than block")
        {
            REQUIRE_FALSE(AlphaCanon::IsInBlacklistedLocation());
        }
    }

    SECTION("when no list is configured")
    {
        const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"};

        SECTION("should report nothing")
        {
            REQUIRE_FALSE(AlphaCanon::IsInBlacklistedLocation());
        }
    }
}

TEST_CASE("AlphaCanon::EvaluateAll", "[AlphaCanon][engine]")
{
    EngineMock engine;

    SECTION("when nothing has taken charge")
    {
        SECTION("should report no signals")
        {
            REQUIRE_FALSE(AlphaCanon::HasAny(AlphaCanon::EvaluateAll()));
        }
    }

    SECTION("when one signal is raised")
    {
        engine.player.inCombat = true;

        SECTION("should set that bit")
        {
            REQUIRE(AlphaCanon::HasFlag(AlphaCanon::EvaluateAll(), Signal::InActiveCombat));
        }

        SECTION("should leave the other bits clear")
        {
            REQUIRE_FALSE(AlphaCanon::HasFlag(AlphaCanon::EvaluateAll(), Signal::InDialogue));
        }
    }

    SECTION("when several signals are raised at once")
    {
        engine.player.inCombat = true;
        engine.ui.openMenus.emplace_back(RE::DialogueMenu::MENU_NAME);
        engine.world.playerInScene = true;

        SECTION("should set every one of them")
        {
            // The bits are independent, so an aggregator that returned the
            // first match rather than the union would still look right to a
            // caller asking only HasAny.
            const auto mask = AlphaCanon::EvaluateAll();
            REQUIRE(AlphaCanon::HasFlag(mask, Signal::InActiveCombat));
            REQUIRE(AlphaCanon::HasFlag(mask, Signal::InDialogue));
            REQUIRE(AlphaCanon::HasFlag(mask, Signal::InScriptedScene));
        }
    }
}

TEST_CASE("AlphaCanon::Names", "[AlphaCanon][engine]")
{
    SECTION("when no bit is set")
    {
        SECTION("should name nothing")
        {
            REQUIRE(AlphaCanon::Names(Signal::None).empty());
        }
    }

    SECTION("when one bit is set")
    {
        SECTION("should name it")
        {
            const auto names = AlphaCanon::Names(Signal::InDialogue);
            REQUIRE(names.size() == 1);
            REQUIRE(names[0] == "InDialogue");
        }
    }

    SECTION("when several bits are set")
    {
        const auto mask = Signal::InActiveCombat | Signal::InBlacklistedLocation;
        const auto names = AlphaCanon::Names(mask);

        SECTION("should name each of them")
        {
            REQUIRE(names.size() == 2);
            REQUIRE(Contains(names, "InActiveCombat"));
            REQUIRE(Contains(names, "InBlacklistedLocation"));
        }

        SECTION("should list them in declaration order")
        {
            // These strings go into the prompt context, so a stable order keeps
            // an otherwise identical situation from reading as a new one to the
            // model on the next tick.
            REQUIRE(names[0] == "InActiveCombat");
            REQUIRE(names[1] == "InBlacklistedLocation");
        }
    }
}
