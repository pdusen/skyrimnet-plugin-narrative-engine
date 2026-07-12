#include <CourierUtils.h>

#include <logger.h>

#include <RE/Skyrim.h>

#include <atomic>
#include <string>

namespace NarrativeEngine::CourierUtils
{
    namespace
    {
        constexpr const char* kWICourierEditorID         = "WICourier";
        constexpr const char* kCourierContainerAliasName = "Container";
        constexpr const char* kCourierContainerEditorID  = "WICourierContainerRef";

        std::atomic<bool>  g_resolved = false;
        RE::TESQuest*      g_quest = nullptr;
        RE::BGSBaseAlias*  g_containerAlias = nullptr;
        RE::TESObjectREFR* g_containerRefFallback = nullptr;
    }

    RE::TESQuest* ResolveCourierQuest()
    {
        if (g_resolved.load(std::memory_order_acquire)) {
            return g_quest;
        }
        auto* form = RE::TESForm::LookupByEditorID(kWICourierEditorID);
        RE::TESQuest* quest = form ? form->As<RE::TESQuest>() : nullptr;
        if (!quest) {
            logger::error(
                "CourierUtils: vanilla WICourier quest did not resolve "
                "(LookupByEditorID '{}' failed); courier-mediated flows disabled",
                kWICourierEditorID);
        } else {
            for (auto* alias : quest->aliases) {
                if (alias && alias->aliasName == kCourierContainerAliasName) {
                    g_containerAlias = alias;
                    break;
                }
            }
            if (auto* containerForm =
                    RE::TESForm::LookupByEditorID(kCourierContainerEditorID)) {
                g_containerRefFallback = containerForm->AsReference();
            }
            logger::info(
                "CourierUtils: WICourier resolved (formID=0x{:08X}, "
                "isRunning={}, stage={}); alias '{}' = {}, fallback REFR '{}' = {}",
                quest->GetFormID(),
                quest->IsRunning(),
                quest->GetCurrentStageID(),
                kCourierContainerAliasName,
                g_containerAlias ? "found" : "MISSING",
                kCourierContainerEditorID,
                g_containerRefFallback
                    ? fmt::format("0x{:08X}", g_containerRefFallback->GetFormID())
                    : std::string{"NOT FOUND"});
            if (!g_containerAlias && !g_containerRefFallback) {
                logger::warn(
                    "CourierUtils: neither the '{}' alias nor a '{}' REFR "
                    "resolved; every dispatch will roll back.",
                    kCourierContainerAliasName, kCourierContainerEditorID);
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
        if (bookFormID == 0) return 0;
        auto* containerRef = GetCourierContainerRef();
        if (!containerRef) return 0;
        auto* bookForm = RE::TESForm::LookupByID(bookFormID);
        auto* book = bookForm ? bookForm->As<RE::TESBoundObject>() : nullptr;
        if (!book) return 0;
        const auto counts = containerRef->GetInventoryCounts(
            [book](RE::TESBoundObject& obj) { return &obj == book; });
        auto it = counts.find(book);
        return it != counts.end() ? it->second : 0;
    }
}
