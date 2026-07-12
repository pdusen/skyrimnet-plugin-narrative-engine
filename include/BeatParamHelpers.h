#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <RE/T/TESForm.h>

#include <nlohmann/json_fwd.hpp>

namespace RE { class Actor; }

// BeatParamHelpers — small self-contained helpers for the parameter-
// parsing surface shared by beats that accept an LLM-supplied sender
// and urgency hint (currently NPCLetterBeat and NPCVisitBeat; any
// future sender-picking beat gets the same shape).
namespace NarrativeEngine::BeatParamHelpers
{
    // Neutral urgency scale passed from the beat-select LLM into each
    // beat's compose prompt. Kept as a shared enum so both letter and
    // visit compose paths speak the same type without translation.
    enum class UrgencyHint : std::uint8_t
    {
        Low    = 0,
        Medium = 1,
        High   = 2,
    };

    // Parse `parameters["sender_npc_form_id"]` (hex string, e.g. "0xA2C8E").
    // On any failure path (parameters isn't an object, field missing,
    // wrong type, unparseable, resolves to zero) sets `*failureReason`
    // to a stable snake_case literal and returns nullopt. Never
    // touches the engine — just JSON + stoul.
    std::optional<RE::FormID> ParseSenderFormID(
        const nlohmann::json& parameters,
        std::string*          failureReason);

    // Parse `parameters["urgency_hint"]` (string "low" / "medium" /
    // "high"). Missing / wrong-type / unrecognized values return
    // Medium — the neutral default the compose prompt expects.
    UrgencyHint ParseUrgencyHint(const nlohmann::json& parameters);

    // Main thread only. Look up an Actor by FormID and enforce the
    // dispatch-time liveness gates: form resolves, form is an Actor,
    // actor is not dead, actor is not disabled. On any failure sets
    // `*failureReason` to a stable snake_case literal and returns
    // nullptr.
    RE::Actor* ResolveLiveSenderActor(
        RE::FormID   senderFormID,
        std::string* failureReason);
}
