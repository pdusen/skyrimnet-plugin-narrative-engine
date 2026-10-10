#include <CourierUtils.h>

#include <logger.h>

#include <RE/Skyrim.h>

#include <atomic>
#include <cmath>
#include <string>

namespace NarrativeEngine::CourierUtils
{
    namespace
    {
        constexpr const char* kWICourierEditorID = "WICourier";
        constexpr const char* kCourierContainerAliasName = "Container";
        constexpr const char* kCourierContainerEditorID = "WICourierContainerRef";
        constexpr const char* kCourierItemCountEditorID = "WICourierItemCount";

        // Skyrim.esm is always load index 0x00, so the literal is exact.
        // Kept as a fallback because the editor-ID arm needs a runtime
        // EditorID-retention mod and this global is too load-bearing to
        // lose to a missing dependency.
        constexpr RE::FormID kCourierItemCountFormID = 0x00039FBBu;

        std::atomic<bool> g_resolved = false;
        RE::TESQuest* g_quest = nullptr;
        RE::BGSBaseAlias* g_containerAlias = nullptr;
        RE::TESObjectREFR* g_containerRefFallback = nullptr;
        RE::TESGlobal* g_itemCountGlobal = nullptr;

        std::int32_t ReadGlobal(RE::TESGlobal* global)
        {
            // lround rather than a cast: the field is a float and the
            // record is a GlobalShort, so a value that has been through
            // the engine's own arithmetic can sit a hair under the
            // integer it means. Truncating -0.9999 to 0 would hide
            // exactly the wedge this module exists to find.
            return static_cast<std::int32_t>(std::lround(global->value));
        }
    } // namespace

    RE::TESQuest* ResolveCourierQuest()
    {
        if (g_resolved.load(std::memory_order_acquire)) {
            return g_quest;
        }
        auto* form = RE::TESForm::LookupByEditorID(kWICourierEditorID);
        RE::TESQuest* quest = form ? form->As<RE::TESQuest>() : nullptr;
        if (!quest) {
            logger::error("CourierUtils: vanilla WICourier quest did not resolve "
                          "(LookupByEditorID '{}' failed); courier-mediated flows disabled",
                          kWICourierEditorID);
        } else {
            for (auto* alias : quest->aliases) {
                if (alias && alias->aliasName == kCourierContainerAliasName) {
                    g_containerAlias = alias;
                    break;
                }
            }
            if (auto* containerForm = RE::TESForm::LookupByEditorID(kCourierContainerEditorID)) {
                g_containerRefFallback = containerForm->AsReference();
            }
            auto* countForm = RE::TESForm::LookupByEditorID(kCourierItemCountEditorID);
            if (!countForm) {
                countForm = RE::TESForm::LookupByID(kCourierItemCountFormID);
            }
            g_itemCountGlobal = countForm ? countForm->As<RE::TESGlobal>() : nullptr;
            logger::info("CourierUtils: WICourier resolved (formID=0x{:08X}, "
                         "isRunning={}, stage={}); alias '{}' = {}, fallback REFR '{}' = {}",
                         quest->GetFormID(),
                         quest->IsRunning(),
                         quest->GetCurrentStageID(),
                         kCourierContainerAliasName,
                         g_containerAlias ? "found" : "MISSING",
                         kCourierContainerEditorID,
                         g_containerRefFallback ? fmt::format("0x{:08X}", g_containerRefFallback->GetFormID())
                                                : std::string{"NOT FOUND"});
            logger::info("CourierUtils: '{}' = {}",
                         kCourierItemCountEditorID,
                         g_itemCountGlobal ? fmt::format("0x{:08X} (currently {})",
                                                         g_itemCountGlobal->GetFormID(),
                                                         ReadGlobal(g_itemCountGlobal))
                                           : std::string{"NOT FOUND"});
            if (!g_containerAlias && !g_containerRefFallback) {
                logger::warn("CourierUtils: neither the '{}' alias nor a '{}' REFR "
                             "resolved; every dispatch will roll back.",
                             kCourierContainerAliasName,
                             kCourierContainerEditorID);
            }
        }
        g_quest = quest;
        g_resolved.store(true, std::memory_order_release);
        return quest;
    }

    RE::TESObjectREFR* GetCourierContainerRef()
    {
        if (g_containerAlias) {
            if (auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(g_containerAlias)) {
                if (auto* r = refAlias->GetReference()) {
                    return r;
                }
            }
        }
        return g_containerRefFallback;
    }

    std::int32_t GetCourierInventoryCount(RE::FormID bookFormID)
    {
        if (bookFormID == 0)
            return 0;
        auto* containerRef = GetCourierContainerRef();
        if (!containerRef)
            return 0;
        auto* bookForm = RE::TESForm::LookupByID(bookFormID);
        auto* book = bookForm ? bookForm->As<RE::TESBoundObject>() : nullptr;
        if (!book)
            return 0;
        const auto counts = containerRef->GetInventoryCounts([book](RE::TESBoundObject& obj) { return &obj == book; });
        auto it = counts.find(book);
        return it != counts.end() ? it->second : 0;
    }

    RE::TESGlobal* GetCourierItemCountGlobal()
    {
        return g_itemCountGlobal;
    }

    std::int32_t CountLettersInCourierContainer()
    {
        auto* containerRef = GetCourierContainerRef();
        if (!containerRef)
            return 0;
        const auto counts =
            containerRef->GetInventoryCounts([](RE::TESBoundObject& obj) { return obj.Is(RE::FormType::Book); });
        std::int32_t total = 0;
        for (const auto& [obj, count] : counts) {
            if (count > 0)
                total += count;
        }
        return total;
    }

    std::optional<std::int32_t> RepairCourierItemCount()
    {
        if (!g_itemCountGlobal)
            return std::nullopt;
        const std::int32_t letters = CountLettersInCourierContainer();
        const std::int32_t current = ReadGlobal(g_itemCountGlobal);
        if (current >= letters)
            return std::nullopt;

        g_itemCountGlobal->value = static_cast<float>(letters);
        logger::warn("CourierUtils: repaired {}: {} -> {} ({} letter(s) staged)",
                     kCourierItemCountEditorID,
                     current,
                     letters,
                     letters);
        return current;
    }

    void OnRevert()
    {
        g_containerAlias = nullptr;
        g_containerRefFallback = nullptr;
        g_itemCountGlobal = nullptr;
        g_quest = nullptr;
        // Released last, so a concurrent reader either sees the whole previous
        // resolution or re-resolves; it can never see a half-cleared one.
        g_resolved.store(false, std::memory_order_release);
    }
} // namespace NarrativeEngine::CourierUtils
