#pragma once

#include <cstddef>
#include <cstdint>

// Shared state between the fake po3_Tweaks.dll and the test that drives it.
//
// powerofthree's Tweaks keeps the EditorIDs the engine discards and serves them
// through one export, `GetFormEditorID(FormID)`. SenderCandidatePool reaches it
// with GetModuleHandle("po3_Tweaks.dll") and GetProcAddress and nothing in
// between, so — as for PrismaUI and SkyrimNet — the harness builds a real DLL of
// that name rather than inventing a seam the production code does not have.
//
// The DLL owns the state; the test reaches it through a second export. Plain
// fixed-size storage only: the two binaries have separate heaps.
namespace NarrativeEngine::Testing
{
    struct FakePo3TweaksState
    {
        static constexpr int kMaxEntries = 16;
        static constexpr std::size_t kMaxEditorIDLength = 64;

        // The cache, keyed by FormID exactly as Tweaks keys it. An unknown
        // FormID answers "" — never nullptr — because the real export returns
        // the c_str() of a static empty std::string on a miss (po3-Tweaks,
        // src/Cache.cpp).
        int entries = 0;
        std::uint32_t formIDs[kMaxEntries]{};
        char editorIDs[kMaxEntries][kMaxEditorIDLength]{};

        // Every FormID the export was asked about, so a test can tell the
        // base-form lookup from a reference-form one, and can ask about one
        // NPC without depending on which other candidates the walk visited.
        static constexpr int kMaxRecordedLookups = 64;
        int lookups = 0;
        std::uint32_t askedFor[kMaxRecordedLookups]{};

        bool AskedFor(std::uint32_t formID) const
        {
            const int recorded = lookups < kMaxRecordedLookups ? lookups : kMaxRecordedLookups;
            for (int i = 0; i < recorded; ++i) {
                if (askedFor[i] == formID) {
                    return true;
                }
            }
            return false;
        }

        void Add(std::uint32_t formID, const char* editorID)
        {
            if (entries >= kMaxEntries || !editorID) {
                return;
            }
            formIDs[entries] = formID;
            std::size_t i = 0;
            for (; i + 1 < kMaxEditorIDLength && editorID[i] != '\0'; ++i) {
                editorIDs[entries][i] = editorID[i];
            }
            editorIDs[entries][i] = '\0';
            ++entries;
        }

        void Reset()
        {
            *this = FakePo3TweaksState{};
        }
    };

    using FakePo3TweaksStateFunc = FakePo3TweaksState* (*)();
    inline constexpr const char* kFakePo3TweaksStateExport = "NarrativeEngineFakePo3TweaksState";
} // namespace NarrativeEngine::Testing
