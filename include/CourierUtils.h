#pragma once

#include <cstdint>
#include <optional>

#include <RE/T/TESForm.h>

namespace RE
{
    class TESGlobal;
    class TESObjectREFR;
    class TESQuest;
} // namespace RE

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

    // The vanilla `WICourierItemCount` global (0x00039FBB), or nullptr
    // when it didn't resolve. Cached alongside the rest of the
    // resolution.
    RE::TESGlobal* GetCourierItemCountGlobal();

    // How many LETTERS are staged in the courier container right now.
    //
    // Letters, not objects. `WICourierScript::addItemToContainer`
    // increments the item-count global by exactly one however large a
    // stack it is handed, and vanilla leans on that: `QF_WIKill03`
    // stages an inheritance as one letter plus `pAward` gold in two
    // calls, so the barrel legitimately holds several hundred objects
    // against a count of two. Counting objects would therefore read
    // wildly high. Every vanilla courier payload except that gold is a
    // BOOK, so book-ness is the discriminator.
    //
    // Counts everyone's letters, not just the pool's: the global gates
    // delivery for every mod that uses the courier, and a repair that
    // only accounted for ours would leave somebody else's letter
    // undercounted and still stuck.
    //
    // MAIN THREAD -- walks the container's inventory.
    std::int32_t CountLettersInCourierContainer();

    // Raise `WICourierItemCount` to the number of letters actually in
    // the barrel, if it currently reads lower. Never lowers it.
    //
    // The global is a signed `GlobalShort` persisted in the save, and
    // every gate on the vanilla delivery chain -- the Story Manager
    // node `WICourierNodeSharesEvent`, the `WICourierDeliverToPlayer`
    // package, and the greeting topic info -- tests it `>= 1`. The only
    // write that ever sets it to a known value is
    // `GiveItemsToPlayer`'s `SetValue(0)`, which runs at the END of a
    // delivery. So a count driven negative (by a mod that removes
    // through `removeRefFromContainer` without having added through
    // `addItemToContainer`, or by a console `set`) wedges permanently:
    // the gate blocks the delivery, and only a delivery would clear it.
    // `addItemToContainer` is a bare `+= 1` with no floor, so staging
    // more letters does not dig the install out.
    //
    // Raising only, never lowering, because a value ABOVE the letter
    // count may be a legitimately pending non-letter delivery we cannot
    // see, and cancelling somebody else's courier is worse than the
    // spurious empty-handed visit an inflated count causes (which
    // `GiveItemsToPlayer` then clears anyway).
    //
    // Returns the value the global held before, if a repair happened;
    // nullopt when none was needed or nothing resolved. Logs at warn on
    // repair -- a line in a user's log is how this diagnosis gets
    // confirmed from the field rather than argued from the records.
    //
    // MAIN THREAD.
    std::optional<std::int32_t> RepairCourierItemCount();

    // Drop the cached resolution so the next call resolves afresh.
    //
    // Called from the SKSE revert / post-load handler alongside every other
    // module that caches state, which this one was missing. The quest form
    // itself is static for the process, but the `Container` alias's live
    // reference and the `WICourierContainerRef` REFR are per-save: a pointer
    // cached under one save has no business being handed out under another.
    void OnRevert();
} // namespace NarrativeEngine::CourierUtils
