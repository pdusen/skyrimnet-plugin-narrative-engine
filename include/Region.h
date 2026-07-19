#pragma once

#include <cstdint>
#include <string>

#include <RE/Skyrim.h>

// Region — hold and climate resolution for the player's current cell.
//
// The `BGSLocation` keyword surface (LocSetTundra, LocSetPine,
// LocSetSnow, etc.) is inconsistently applied across vanilla locations
// and can't be trusted as a biome / hold signal. Vanilla `TESRegion`
// records, on the other hand, are authored on cells and are what the
// vanilla weather system itself consults — the same TESRegion whose
// `currentWeather` field seeds Sky::region on cell load.
//
// This module walks `RE::TESObjectCELL::GetRegionList(false)`, matches
// each returned TESRegion by EditorID against a curated per-hold table,
// and returns a small Resolution describing the hold + climate. Interior
// cells and cells with no matching region resolve to Climate::Unknown;
// callers should treat that as "no signal" rather than an error.
//
// Consumers: TravelEventLog (hold-region tracking), future biome-gated
// beats (a Reach-only encounter beat, a Solstheim-only ash-storm portent,
// etc.).
//
// Threading: main-thread. `LookupByEditorID` and `GetRegionList` are safe
// off-thread in practice but we don't need them off-thread, so the
// contract is main-thread-only.
namespace NarrativeEngine::Region
{
    enum class Climate : std::uint8_t
    {
        Unknown,   // interior, unrecognised region, or no region — treat as "no signal"
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
        // FormID of the TESRegion that resolved, if any. 0 when Unknown.
        // Useful for consumers that want to diff on region identity
        // (TravelEventLog uses it to detect hold-boundary crossings even
        // when the climate bucket happens to match).
        RE::FormID holdRegionFormID = 0;
        // "Whiterun Hold", "Falkreath Hold", ... — derived from the
        // internal EditorID table, not from the region's own display
        // name (TESRegion has no FULL name). Empty when Unknown.
        std::string holdDisplayName;
    };

    // Main-thread. Resolves the player's current cell. Null-safe.
    Resolution ForPlayer();

    // Main-thread. Walks `cell->GetRegionList(false)` and returns the
    // first hold-level match. Null-safe.
    Resolution ForCell(RE::TESObjectCELL* cell);
} // namespace NarrativeEngine::Region
