#include <Region.h>

#include <HoldGrid.h>
#include <logger.h>

#include <array>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace NarrativeEngine::Region
{
    namespace
    {
        // Upper bound on parentLoc traversal depth. Vanilla chains are
        // typically 2–4 deep; cap defensively against malformed data.
        constexpr int kMaxParentDepth = 16;

        // Small side table: hold-level BGSLocation EditorID → Climate.
        // Populates the Climate field of the resolved Resolution so
        // future biome-gated beats can filter by climate without doing
        // their own hold lookup. Everything else — hold identity and
        // display name — comes straight from the resolved location.
        //
        // Empty entries or missing holds are non-fatal: they resolve to
        // Climate::Unknown but still carry a valid holdFormID +
        // display name so travel event detection still works.
        //
        // If any EditorID fails to resolve at cache-build time, one
        // warning is logged and the entry is skipped. Mirror lives at
        // docs/vanilla/regions/holds.csv.
        struct HoldClimateEntry
        {
            std::string_view editorId;
            Climate climate;
        };
        constexpr std::array<HoldClimateEntry, 10> kHoldClimateTable = {
            HoldClimateEntry{"WhiterunHoldLocation", Climate::Tundra},
            HoldClimateEntry{"FalkreathHoldLocation", Climate::Pine},
            HoldClimateEntry{"HjaalmarchHoldLocation", Climate::Marsh},
            HoldClimateEntry{"WinterholdHoldLocation", Climate::Snow},
            HoldClimateEntry{"PaleHoldLocation", Climate::Snow},
            HoldClimateEntry{"ReachHoldLocation", Climate::Reach},
            HoldClimateEntry{"RiftHoldLocation", Climate::Rift},
            HoldClimateEntry{"HaafingarHoldLocation", Climate::Coast},
            HoldClimateEntry{"EastmarchHoldLocation", Climate::Volcanic},
            HoldClimateEntry{"DLC2SolstheimLocation", Climate::Solstheim},
        };

        // Cached LocTypeHold keyword pointer. Resolves once on first
        // call; nullptr on unrecoverable failure (missing powerofthree's
        // Tweaks or the vanilla keyword renamed), in which case every
        // Region query returns an empty Resolution.
        RE::BGSKeyword* LocTypeHoldKeyword()
        {
            static RE::BGSKeyword* cached = []() -> RE::BGSKeyword* {
                auto* form = RE::TESForm::LookupByEditorID("LocTypeHold");
                if (!form) {
                    logger::warn("Region: LocTypeHold keyword did not resolve; hold detection disabled "
                                 "(is powerofthree's Tweaks installed?)");
                    return nullptr;
                }
                auto* kw = form->As<RE::BGSKeyword>();
                if (!kw) {
                    logger::warn("Region: 'LocTypeHold' resolved to non-keyword form (type=0x{:02X}); "
                                 "hold detection disabled",
                                 static_cast<unsigned>(form->GetFormType()));
                    return nullptr;
                }
                return kw;
            }();
            return cached;
        }

        // FormID → Climate cache, built once on first call from the
        // hardcoded EditorID table. Unresolved EditorIDs log one warning
        // and are skipped (the hold still resolves — it just gets
        // Climate::Unknown).
        const std::unordered_map<RE::FormID, Climate>& ResolvedClimateTable()
        {
            static const auto cache = []() {
                std::unordered_map<RE::FormID, Climate> out;
                out.reserve(kHoldClimateTable.size());
                for (const auto& entry : kHoldClimateTable) {
                    auto* form = RE::TESForm::LookupByEditorID(entry.editorId);
                    if (!form) {
                        logger::warn("Region: climate-table EditorID '{}' did not resolve; hold will resolve "
                                     "with Climate::Unknown",
                                     entry.editorId);
                        continue;
                    }
                    out.emplace(form->GetFormID(), entry.climate);
                }
                return out;
            }();
            return cache;
        }

        // Log one warning the first time we see a hold-level location
        // whose FormID isn't in the climate table. Helps identify what
        // additions kHoldClimateTable needs.
        void NoteUnclassifiedHold(RE::BGSLocation* loc)
        {
            if (!loc)
                return;
            static std::unordered_set<RE::FormID> logged;
            const auto id = loc->GetFormID();
            if (logged.insert(id).second) {
                const char* edid = loc->GetFormEditorID();
                const char* full = loc->GetFullName();
                logger::info("Region: hold-level location [0x{:08X}] EditorID='{}' FullName='{}' has no "
                             "Climate mapping (Climate::Unknown returned)",
                             id,
                             (edid && *edid) ? edid : "?",
                             (full && *full) ? full : "?");
            }
        }

        // Build a full Resolution from a known hold-level BGSLocation.
        // Shared by both the HoldGrid fast-path (which returns a raw
        // FormID) and the parent-walk fallback (which returns the
        // location pointer directly). Populates display name and
        // climate from the location + climate table.
        Resolution BuildResolution(RE::BGSLocation* holdLoc)
        {
            Resolution r;
            if (!holdLoc)
                return r;
            r.holdFormID = holdLoc->GetFormID();
            if (const char* full = holdLoc->GetFullName(); full && *full) {
                r.holdDisplayName = full;
            } else if (const char* edid = holdLoc->GetFormEditorID(); edid && *edid) {
                r.holdDisplayName = edid;
            }
            const auto& table = ResolvedClimateTable();
            if (const auto it = table.find(r.holdFormID); it != table.end()) {
                r.climate = it->second;
            } else {
                NoteUnclassifiedHold(holdLoc);
            }
            return r;
        }
    } // namespace

    Resolution ForPlayer()
    {
        // Fast path: HoldGrid precomputed a coordinate → hold FormID
        // partition for every exterior cell at kDataLoaded. If the
        // player is on an exterior cell that landed in the grid, we
        // get an O(1) hash lookup with no location-chain walk.
        if (const auto holdFormID = HoldGrid::LookupPlayer(); holdFormID != 0) {
            auto* form = RE::TESForm::LookupByID(holdFormID);
            auto* holdLoc = form ? form->As<RE::BGSLocation>() : nullptr;
            if (holdLoc) {
                return BuildResolution(holdLoc);
            }
        }

        // Fallback: parent-walk on the player's current location.
        // Covers interior cells (which the grid doesn't index) and
        // any exterior cells the BFS couldn't reach from a seed.
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) {
            return {};
        }
        return ForLocation(pc->GetCurrentLocation());
    }

    Resolution ForLocation(RE::BGSLocation* startLoc)
    {
        if (!startLoc) {
            return {};
        }
        auto* holdKw = LocTypeHoldKeyword();
        if (!holdKw) {
            return {};
        }

        RE::BGSLocation* loc = startLoc;
        for (int depth = 0; loc != nullptr && depth < kMaxParentDepth; ++depth) {
            if (loc->HasKeyword(holdKw)) {
                return BuildResolution(loc);
            }
            loc = loc->parentLoc;
        }
        return {};
    }
} // namespace NarrativeEngine::Region
