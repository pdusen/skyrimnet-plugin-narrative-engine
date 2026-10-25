#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string_view>

// Why plot birth refused a model's answer, as a code and its arguments
// rather than a sentence.
//
// A refused answer is asked again with the reason, and the prompt words it:
// components/narrative_engine/plot_rejection.prompt has one branch per code,
// which narrative_engine_plot_cast and narrative_engine_plot_ambition both
// include. PlotLog records the same code and arguments. A test fails the
// build when a code has no branch in that file, so a new refusal cannot reach
// the model as nothing.
//
// The arguments each code carries, all strings unless noted. A `role` is
// {verb, name}: the goal's id and the slot's name, which the prompt says as
// "the mark (whom it is stolen from)". `admitted` is {names [string], more
// (bool)}: who or what the role could be instead, `more` when the list was
// cut short.
//
//   call_failed          (none) -- the call itself failed; never retried
//   parse_threw          detail -- the parser threw, with what it said
//   no_shortlist         (none)
//   not_json             (none)
//   missing_choice       (none)
//   empty_choice         (none)
//   choice_not_form_id   text
//   choice_wrong_type    (none)
//   choice_not_listed    form (hex, no prefix), count (number)
//   missing_text         key -- a field the answer did not carry
//   empty_text           key
//   names_player         key
//   too_long             key, length, limit (numbers)
//   scheme_accomplished  scheme
//   goal_unknown         goal
//   goal_not_finishing   goal
//   finish_not_verb      finish
//   role_names_player    role
//   roles_missing        goal, missing [role]
//   roles_unfit          problems [rejection] -- each one of the role_* codes
//                        below, or place_unknown or thing_unknown
//   scheme_not_verb      scheme
//   scheme_is_ambition   (none)
//
//   role_unnamed         role
//   role_is_mastermind   role, who, admitted
//   role_dead            role, who, admitted
//   role_left_out        role, who, why, admitted. `why` is ghost,
//                        not_person, no_location, no_hold or child
//   role_names_nobody    role, text, admitted
//   role_ambiguous       role, text, people [string]
//   role_not_admitted    role, who, admitted
//   place_unknown        role, text, admitted
//   thing_unknown        role, text, pool [string]
namespace NarrativeEngine::PlotRejection
{
    enum class Code : std::uint8_t
    {
        None,
        CallFailed,
        ParseThrew,
        NoShortlist,
        NotJson,
        MissingChoice,
        EmptyChoice,
        ChoiceNotFormId,
        ChoiceWrongType,
        ChoiceNotListed,
        MissingText,
        EmptyText,
        NamesPlayer,
        TooLong,
        SchemeAccomplished,
        GoalUnknown,
        GoalNotFinishing,
        FinishNotVerb,
        RoleNamesPlayer,
        RolesMissing,
        RolesUnfit,
        SchemeNotVerb,
        SchemeIsAmbition,
        RoleUnnamed,
        RoleIsMastermind,
        RoleDead,
        RoleLeftOut,
        RoleNamesNobody,
        RoleAmbiguous,
        RoleNotAdmitted,
        PlaceUnknown,
        ThingUnknown,

        Count
    };

    // The code as the prompt and the log read it: "goal_unknown". Empty for
    // None.
    [[nodiscard]] std::string_view Id(Code code) noexcept;

    struct Rejection
    {
        Code code = Code::None;
        nlohmann::json args = nlohmann::json::object();

        [[nodiscard]] bool Empty() const noexcept
        {
            return code == Code::None;
        }

        // {"code": id, ...args}: what a retry and the log read.
        [[nodiscard]] nlohmann::json ToJson() const;
    };

    [[nodiscard]] Rejection Make(Code code, nlohmann::json args = nlohmann::json::object());

    // A role's argument: the goal's id and the slot's name.
    [[nodiscard]] nlohmann::json Role(std::string_view verb, std::string_view slot);
} // namespace NarrativeEngine::PlotRejection
