#include <BeatParamHelpers.h>

#include <nlohmann/json.hpp>

#include <RE/A/Actor.h>
#include <RE/T/TESForm.h>

namespace NarrativeEngine::BeatParamHelpers
{
    std::optional<RE::FormID> ParseSenderFormID(const nlohmann::json& parameters, std::string* failureReason)
    {
        auto fail = [failureReason](const char* reason) -> std::optional<RE::FormID> {
            if (failureReason)
                *failureReason = reason;
            return std::nullopt;
        };
        if (!parameters.is_object())
            return fail("parameters_not_object");
        auto it = parameters.find("sender_npc_form_id");
        if (it == parameters.end() || !it->is_string()) {
            return fail("sender_npc_form_id_missing");
        }
        const auto idStr = it->get<std::string>();
        RE::FormID formID = 0;
        try {
            formID = static_cast<RE::FormID>(std::stoul(idStr, nullptr, /*base=*/0));
        } catch (...) {
            return fail("sender_npc_form_id_unparseable");
        }
        if (formID == 0)
            return fail("sender_npc_form_id_zero");
        return formID;
    }

    UrgencyHint ParseUrgencyHint(const nlohmann::json& parameters)
    {
        if (!parameters.is_object())
            return UrgencyHint::Medium;
        auto it = parameters.find("urgency_hint");
        if (it == parameters.end() || !it->is_string()) {
            return UrgencyHint::Medium;
        }
        const auto v = it->get<std::string>();
        if (v == "low")
            return UrgencyHint::Low;
        if (v == "high")
            return UrgencyHint::High;
        return UrgencyHint::Medium;
    }

    RE::Actor* ResolveLiveSenderActor(RE::FormID senderFormID, std::string* failureReason)
    {
        auto fail = [failureReason](const char* reason) -> RE::Actor* {
            if (failureReason)
                *failureReason = reason;
            return nullptr;
        };
        auto* form = RE::TESForm::LookupByID(senderFormID);
        auto* sender = form ? form->As<RE::Actor>() : nullptr;
        if (!sender)
            return fail("sender_no_longer_resolves");
        if (sender->IsDead())
            return fail("sender_died_during_compose");
        if (sender->IsDisabled())
            return fail("sender_disabled_during_compose");
        return sender;
    }
} // namespace NarrativeEngine::BeatParamHelpers
