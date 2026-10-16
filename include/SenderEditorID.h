#pragma once

#include <cstdint>
#include <string_view>

// SenderEditorID — the EditorID the sender blacklist matches an NPC against.
//
// TESNPC::GetFormEditorID() is empty at runtime even with powerofthree's Tweaks
// installed: Tweaks does not restore the engine's virtual getter for NPCs, it
// keeps the EditorIDs the engine discards in a cache of its own and serves them
// through a `GetFormEditorID(FormID)` export on po3_Tweaks.dll (the same export
// CLibUtil's editorID.hpp reads). Asking only the engine left the EditorID arm
// of sBlacklistedSenders unable to match anyone, so every entry written as an
// EditorID silently let its NPC through.
//
// Resolution is kept free of engine types so the order it applies is testable
// on its own; the caller reads the native ID and resolves the export.
namespace NarrativeEngine::SenderCandidatePool
{
    // Signature of po3_Tweaks.dll's `GetFormEditorID` export.
    using EditorIDLookup = const char* (*)(std::uint32_t);

    struct SenderEditorID
    {
        // Empty when neither source resolved. Points into the engine's string
        // or Tweaks' cache, both of which outlive one viability walk.
        std::string_view value;
        // "native", "po3-cache" or "unresolved", for the walk's debug log.
        const char* source = "unresolved";
    };

    // A non-empty native ID wins and the cache is never asked. Otherwise the
    // cache is asked for `baseFormID` — the NPC base's FormID, not the actor
    // reference's, because the EditorID belongs to the base record. A null
    // lookup (Tweaks not loaded, or the export missing) or an empty answer
    // leaves the ID unresolved, and the display-name arm still applies.
    inline SenderEditorID ResolveSenderEditorID(const char* nativeID, std::uint32_t baseFormID, EditorIDLookup lookup)
    {
        if (nativeID && *nativeID) {
            return {nativeID, "native"};
        }
        if (lookup) {
            if (const char* cachedID = lookup(baseFormID); cachedID && *cachedID) {
                return {cachedID, "po3-cache"};
            }
        }
        return {};
    }
} // namespace NarrativeEngine::SenderCandidatePool
