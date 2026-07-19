#pragma once

#include <cstdint>
#include <string>

#include <RE/Skyrim.h>

// Region — hold and biome resolution for the player's current location.
//
// The player's current hold is derived by walking the parent chain of
// `PlayerCharacter::GetCurrentLocation()` looking for an ancestor
// tagged with the vanilla `LocTypeHold` keyword. That's how the vanilla
// game itself decides "which hold is the player in" for quest
// conditions, guard dialogue, etc. — every hold has a hold-level
// BGSLocation (WhiterunHoldLocation, FalkreathHoldLocation, ...) that
// wilderness / city / dungeon locations all chain up to via parentLoc.
//
// An earlier design keyed on `TESRegion` records, but those are
// weather / vegetation zones, not political boundaries — the record
// names and coverage don't cleanly map to holds. Locations do.
//
// Interior cells assigned to buildings within a hold (Bannered Mare,
// Warmaiden's, Dragonsreach interior, etc.) still resolve correctly
// because their locations chain up to the hold's location the same
// way. Genuinely unassigned cells (some scripted dungeon interiors,
// pre-vanilla test cells) return Climate::Unknown / holdFormID=0 and
// callers should treat that as "no signal."
//
// Threading: main-thread. Uses `TESForm::LookupByEditorID` (needs
// powerofthree's Tweaks for EditorID retention, same as
// LocationKeywords).
namespace NarrativeEngine::Region
{
    enum class Climate : std::uint8_t
    {
        Unknown,   // no hold resolved, or hold not in the climate table
        Tundra,    // Whiterun Hold plains
        Pine,      // Falkreath Hold forest
        Marsh,     // Hjaalmarch
        Snow,      // Winterhold, The Pale — permanent snow
        Reach,     // The Reach — mountainous / rocky
        Rift,      // The Rift — autumnal forest
        Coast,     // Haafingar coast
        Volcanic,  // Eastmarch geothermal
        Solstheim, // DLC02 ashland
    };

    struct Resolution
    {
        Climate climate = Climate::Unknown;
        // FormID of the hold-level BGSLocation resolved via parentLoc
        // walk. 0 when the current location chain has no LocTypeHold-
        // tagged ancestor. Consumers use this as the "which hold?"
        // identity for cross-boundary detection.
        RE::FormID holdFormID = 0;
        // Display name of the resolved hold — from
        // `BGSLocation::GetFullName()` when available (e.g.
        // "Whiterun Hold"), falling back to the EditorID when the
        // location has no display name (rare). Empty when no hold
        // resolved.
        std::string holdDisplayName;
    };

    // Main-thread. Resolves the player's current location. Null-safe.
    Resolution ForPlayer();

    // Main-thread. Walks the parent chain of `startLoc`. Null-safe.
    Resolution ForLocation(RE::BGSLocation* startLoc);
} // namespace NarrativeEngine::Region
