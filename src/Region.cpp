#include <Region.h>

#include <logger.h>

#include <array>
#include <string_view>
#include <unordered_map>

namespace NarrativeEngine::Region
{
    namespace
    {
        struct HoldEntry
        {
            std::string_view editorId;
            Climate climate;
            std::string_view displayName;
        };

        // Curated table of vanilla per-hold TESRegion EditorIDs. The
        // Tamriel root region is deliberately unmapped — it's the parent
        // of every outdoor cell in Skyrim and matching it would swallow
        // every hold. Only hold-level entries live here.
        //
        // Mirror lives at docs/vanilla/regions/holds.csv (source of truth
        // for edits — keep both in sync). Confirm any additions against
        // the CK / xEdit; a wrong EditorID silently degrades to
        // Climate::Unknown for that hold.
        constexpr std::array<HoldEntry, 10> kHoldTable = {
            HoldEntry{"HoldWhiterunRegion", Climate::Tundra, "Whiterun Hold"},
            HoldEntry{"HoldFalkreathRegion", Climate::Pine, "Falkreath Hold"},
            HoldEntry{"HoldHjaalmarchRegion", Climate::Marsh, "Hjaalmarch"},
            HoldEntry{"HoldWinterholdRegion", Climate::Snow, "Winterhold"},
            HoldEntry{"HoldThePaleRegion", Climate::Snow, "The Pale"},
            HoldEntry{"HoldTheReachRegion", Climate::Reach, "The Reach"},
            HoldEntry{"HoldTheRiftRegion", Climate::Rift, "The Rift"},
            HoldEntry{"HoldHaafingarRegion", Climate::Coast, "Haafingar"},
            HoldEntry{"HoldEastmarchRegion", Climate::Volcanic, "Eastmarch"},
            HoldEntry{"DLC2SolstheimRegion", Climate::Solstheim, "Solstheim"},
        };

        // Cached resolution table: FormID → HoldEntry pointer. Built once
        // on first ForCell / ForPlayer call; subsequent lookups are a
        // single hash-map hit per region in the cell's region list.
        //
        // Unresolved EditorIDs log a single warning at cache-build time
        // (missing powerofthree's Tweaks, or the CK renamed a region
        // between game versions). Filter degrades open — resolves to
        // Climate::Unknown for callers of that hold.
        const std::unordered_map<RE::FormID, const HoldEntry*>& ResolvedTable()
        {
            static const auto cache = []() {
                std::unordered_map<RE::FormID, const HoldEntry*> out;
                out.reserve(kHoldTable.size());
                for (const auto& entry : kHoldTable) {
                    auto* form = RE::TESForm::LookupByEditorID(entry.editorId);
                    if (!form) {
                        logger::warn(
                            "Region: EditorID '{}' did not resolve; hold degraded to Unknown (is powerofthree's Tweaks installed?)",
                            entry.editorId);
                        continue;
                    }
                    if (form->GetFormType() != RE::FormType::Region) {
                        logger::warn("Region: EditorID '{}' resolved to non-Region form (type=0x{:02X})",
                                     entry.editorId,
                                     static_cast<unsigned>(form->GetFormType()));
                        continue;
                    }
                    out.emplace(form->GetFormID(), &entry);
                }
                return out;
            }();
            return cache;
        }
    } // namespace

    Resolution ForPlayer()
    {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) {
            return {};
        }
        return ForCell(pc->GetParentCell());
    }

    Resolution ForCell(RE::TESObjectCELL* cell)
    {
        if (!cell) {
            return {};
        }
        auto* list = cell->GetRegionList(false);
        if (!list) {
            return {};
        }
        const auto& table = ResolvedTable();
        for (auto* region : *list) {
            if (!region) {
                continue;
            }
            const auto it = table.find(region->GetFormID());
            if (it == table.end()) {
                continue;
            }
            const HoldEntry* entry = it->second;
            Resolution out;
            out.climate = entry->climate;
            out.holdRegionFormID = region->GetFormID();
            out.holdDisplayName = std::string(entry->displayName);
            return out;
        }
        return {};
    }
} // namespace NarrativeEngine::Region
