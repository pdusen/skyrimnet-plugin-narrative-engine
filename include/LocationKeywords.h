#pragma once

#include <array>
#include <string_view>

namespace RE { class BGSLocation; }

// Curated lists of vanilla BGSLocation keywords, grouped by the
// authorial meaning each set conveys, plus helpers that check a Location
// (and its ancestor chain) against those sets. Source of truth for the
// lists lives in docs/vanilla/keywords/locations/{safe,dangerous}.csv —
// edit those when a new keyword should join either set, and mirror the
// change in kSafe / kDangerous below.
//
// Each entry is a vanilla EditorID. Resolution to a runtime BGSKeyword*
// happens once inside this module (cached, lazy on first call) via
// RE::TESForm::LookupByEditorID, which requires powerofthree's Tweaks
// (or any equivalent runtime EditorID retention) to be installed. A
// per-keyword lookup miss is logged once and degrades open — failing
// closed would silently block actions everywhere instead of just at the
// affected location.
//
// `kSafe` / `kDangerous` are exposed for callers that need the raw
// EditorID list (logging, prompt context, etc.). Most call sites only
// want the boolean predicates IsSafe / IsDangerous and should reach for
// those — they walk the BGSLocation::parentLoc chain so that e.g. a
// child location like WhiterunStablesExterior inherits its parent
// WhiterunLocation's LocTypeCity classification, matching how vanilla
// quest conditions read these keywords.
namespace NarrativeEngine::LocationKeywords
{
    // "Safe" — civilized / settled space. An ambush, brawl, or other
    // disruptive intervention here reads as nonsensical or breaks vanilla
    // assumptions (guards on patrol, NPCs with schedules, scripted scenes
    // already owning the cell). Actions that disrupt the player's
    // immediate surroundings should reject the player's current location
    // if it carries any of these.
    inline constexpr std::array<std::string_view, 23> kSafe = {
        "LocTypeBarracks",
        "LocTypeCastle",
        "LocTypeCemetery",
        "LocTypeCity",
        "LocTypeDwelling",
        "LocTypeFarm",
        "LocTypeGuild",
        "LocTypeHabitation",
        "LocTypeHabitationHasInn",
        "LocTypeHoldCapital",
        "LocTypeHouse",
        "LocTypeInn",
        "LocTypeJail",
        "LocTypeLumberMill",
        "LocTypeMine",
        "LocTypeOrcStronghold",
        "LocTypePlayerHouse",
        "LocTypeSettlement",
        "LocTypeShip",
        "LocTypeStewardsDwelling",
        "LocTypeStore",
        "LocTypeTemple",
        "LocTypeTown",
    };

    // "Dangerous" — hostile / lair space. Vanilla already populates these
    // with combat encounters; layering another Director-issued threat on
    // top is redundant at best, and at worst stacks into unwinnable
    // gauntlets. Actions that spawn hostile NPCs should reject the
    // player's current location if it carries any of these.
    inline constexpr std::array<std::string_view, 25> kDangerous = {
        "LocSetCave",
        "LocSetCaveIce",
        "LocSetDwarvenRuin",
        "LocSetMilitaryCamp",
        "LocSetMilitaryFort",
        "LocSetNordicRuin",
        "LocTypeAnimalDen",
        "LocTypeBanditCamp",
        "LocTypeDragonLair",
        "LocTypeDragonPriestLair",
        "LocTypeDraugrCrypt",
        "LocTypeDungeon",
        "LocTypeDwarvenAutomatons",
        "LocTypeFalmerHive",
        "LocTypeForswornCamp",
        "LocTypeGiantCamp",
        "LocTypeHagravenNest",
        "LocTypeMilitaryCamp",
        "LocTypeMilitaryFort",
        "LocTypeShipwreck",
        "LocTypeSprigganGrove",
        "LocTypeVampireLair",
        "LocTypeWarlockLair",
        "LocTypeWerebearLair",
        "LocTypeWerewolfLair",
    };

    // True when `loc` or any ancestor reached via BGSLocation::parentLoc
    // carries any keyword from kSafe. Null `loc` returns false. Bounded
    // parentLoc walk; the resolved BGSKeyword* table is built once on
    // first call and reused thereafter.
    bool IsSafe(RE::BGSLocation* loc);

    // True when `loc` or any ancestor reached via BGSLocation::parentLoc
    // carries any keyword from kDangerous. Null `loc` returns false.
    bool IsDangerous(RE::BGSLocation* loc);
}
