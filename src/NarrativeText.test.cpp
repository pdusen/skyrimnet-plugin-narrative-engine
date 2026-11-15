#include <LanguageTable.h>
#include <NarrativeText.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Characterization of the non-plot sentences NarrativeEngine writes into
// SkyrimNet: gossip's heard and told memories, the letter writer's memory,
// and a visit's closing, resuming and walk-out narration.
//
// Phase 18 Step 6 moved these into english.ini. The cases were pinned first
// against the English as C++ composed it, fallbacks and edge cases included,
// and the move had to leave every one unchanged.

namespace
{
    using namespace NarrativeEngine::NarrativeText;
    using NarrativeEngine::TextTemplate::Sex;

    const Person kHulda{"Hulda", Sex::Female};
    const Person kNobody{"", Sex::Other};
    constexpr std::string_view kBand = "The Jarl's steward has been skimming the tithe.";

    // The shipped english.ini, read against this module's keys alone: the
    // other sections' keys are rejected as unknown here, which matters to
    // nobody, because only these are rendered.
    const NarrativeEngine::LanguageTable::Table& English()
    {
        static const std::string text = [] {
            std::ifstream in(std::filesystem::path{NARRATIVEENGINE_STATICS_DIR}
                                 / "SKSE/Plugins/NarrativeEngine/Languages/english.ini",
                             std::ios::binary);
            return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        }();
        static const auto table =
            NarrativeEngine::LanguageTable::Table::Build({text, std::nullopt, std::nullopt}, Keys());
        return table;
    }
} // namespace

TEST_CASE("NarrativeText: how a listener remembers hearing a rumor", "[NarrativeText]")
{
    const std::string band{kBand};
    CHECK(HeardIt(English(), Heard::KinFromAway, kHulda, "sister", "Whiterun", kBand)
          == "My sister came from Whiterun with news: " + band);
    CHECK(HeardIt(English(), Heard::KinFromAway, kHulda, "sister", "", kBand)
          == "My sister came from away with news: " + band);
    CHECK(HeardIt(English(), Heard::Kin, kHulda, "sister", "", kBand) == "My sister Hulda told me this: " + band);
    CHECK(HeardIt(English(), Heard::Kin, kNobody, "brother", "", kBand) == "My brother someone told me this: " + band);
    CHECK(HeardIt(English(), Heard::Household, kHulda, "", "", kBand) == "Hulda mentioned this over supper: " + band);
    CHECK(HeardIt(English(), Heard::Household, kNobody, "", "", kBand)
          == "someone mentioned this over supper: " + band);
    CHECK(HeardIt(English(), Heard::Settlement, kHulda, "", "", kBand) == "Hulda told me this: " + band);
    CHECK(HeardIt(English(), Heard::Rumor, kHulda, "", "", kBand) == "I heard a rumor going round: " + band);
}

TEST_CASE("NarrativeText: how a teller remembers telling it", "[NarrativeText]")
{
    const std::string band{kBand};
    const Person ysolda{"Ysolda", Sex::Female};
    const Person saadia{"Saadia", Sex::Female};
    const Person amren{"Amren", Sex::Male};
    CHECK(ToldIt(English(), {ysolda}, kBand) == "I told Ysolda this: " + band);
    CHECK(ToldIt(English(), {ysolda, saadia}, kBand) == "I told Ysolda and Saadia this: " + band);
    CHECK(ToldIt(English(), {ysolda, saadia, kHulda}, kBand) == "I told Ysolda, Saadia and Hulda this: " + band);
    CHECK(ToldIt(English(), {ysolda, saadia, kHulda, amren}, kBand)
          == "I told Ysolda, Saadia, Hulda and Amren this: " + band);
    CHECK(ToldIt(English(), {ysolda, kNobody}, kBand) == "I told Ysolda and someone this: " + band);
    CHECK(ToldIt(English(), {}, kBand) == "I told  this: " + band);
}

TEST_CASE("NarrativeText: the letter writer's memory", "[NarrativeText]")
{
    const std::string body = "Dear friend,\nThe mead is good this year.\n\nYours, Hulda";
    CHECK(LetterSent(English(), {"Dovahkiin", Sex::Male}, body)
          == "I wrote and sent a letter to Dovahkiin. The contents were as follows:\n\n" + body);
    CHECK(LetterSent(English(), kNobody, body)
          == "I wrote and sent a letter to the recipient. The contents were as follows:\n\n" + body);
}

TEST_CASE("NarrativeText: a visit's narration", "[NarrativeText]")
{
    const Person lydia{"Lydia", Sex::Female};
    SECTION("walked out on")
    {
        CHECK(WalkedOut(English(), lydia, kHulda)
              == "Lydia left abruptly, in the middle of their conversation with Hulda.");
        CHECK(WalkedOut(English(), kNobody, kNobody)
              == "The player left abruptly, in the middle of their conversation with their visitor.");
        // A name is not capitalised for opening the sentence.
        CHECK(WalkedOut(English(), {"lydia", Sex::Female}, kHulda)
              == "lydia left abruptly, in the middle of their conversation with Hulda.");
    }
    SECTION("taking leave")
    {
        CHECK(Valediction(English(), kHulda, false)
              == "Having said what needed saying, Hulda prepares to take their leave.");
        CHECK(Valediction(English(), kHulda, true)
              == "Having failed to hold the conversation, Hulda prepares to leave, frustrated by being ignored.");
        CHECK(Valediction(English(), kNobody, false)
              == "Having said what needed saying, the sender prepares to take their leave.");
    }
    SECTION("turning back after an interruption")
    {
        CHECK(
            ReEngage(English(), kHulda, lydia)
            == "Now that the interruption has ended, Hulda turns back to Lydia to try and resume their discussion where it "
               "left off.");
        CHECK(ReEngage(English(), kNobody, kNobody)
              == "Now that the interruption has ended, the sender turns back to the player to try and resume their "
                 "discussion where it left off.");
    }
}
