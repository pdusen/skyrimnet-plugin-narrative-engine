#pragma once

#include <cstdint>

#include <RE/T/TESForm.h>

namespace RE
{
    class TESObjectREFR;
    class TESQuest;
}

// CourierUtils — vanilla WICourier / WICourierContainerRef resolution
// and inventory-count helpers. Any beat that needs to hand items off to
// the vanilla courier system or observe its staging container reuses
// this module — resolution is cached behind a one-shot flag so the
// second caller pays only the atomic-load cost.
//
// See docs/engine-findings/wicourier-alias-vs-refr.md (if it lives)
// for the two-path (alias preferred, REFR fallback) rationale.
namespace NarrativeEngine::CourierUtils
{
    // Look up and cache the vanilla WICourier quest. Also caches the
    // preferred `Container` alias on that quest and, as a fallback,
    // the `WICourierContainerRef` REFR looked up by EditorID. Safe to
    // call from any thread; the underlying resolution runs at most once
    // per session. Returns nullptr if WICourier isn't in the load order.
    RE::TESQuest* ResolveCourierQuest();

    // The container reference WICourier writes items into via
    // WICourierScript.AddItemToContainer. Prefers the alias's live
    // reference (so mods that repoint the alias are honored); falls
    // back to the direct WICourierContainerRef REFR. Returns nullptr
    // when neither resolved.
    RE::TESObjectREFR* GetCourierContainerRef();

    // Absolute inventory count of `bookFormID` in the courier container.
    // Uses TESObjectREFR::GetInventoryCounts (base CONT contents +
    // InventoryChanges delta) so the returned value is a real total,
    // not a signed delta. Returns 0 on any missing-piece path
    // (bookFormID==0, courier not resolved, book isn't a bound object,
    // etc.).
    std::int32_t GetCourierInventoryCount(RE::FormID bookFormID);
}
