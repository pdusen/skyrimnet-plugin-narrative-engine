#include <PlotRejection.h>

#include <array>
#include <cstddef>
#include <string>
#include <utility>

namespace NarrativeEngine::PlotRejection
{
    namespace
    {
        constexpr std::array<std::string_view, static_cast<std::size_t>(Code::Count)> kIds{
            "",
            "call_failed",
            "parse_threw",
            "no_shortlist",
            "not_json",
            "missing_choice",
            "empty_choice",
            "choice_not_form_id",
            "choice_wrong_type",
            "choice_not_listed",
            "missing_text",
            "empty_text",
            "names_player",
            "too_long",
            "scheme_accomplished",
            "goal_unknown",
            "goal_not_finishing",
            "finish_not_verb",
            "role_names_player",
            "roles_missing",
            "roles_unfit",
            "scheme_not_verb",
            "scheme_is_ambition",
            "role_unnamed",
            "role_is_mastermind",
            "role_dead",
            "role_left_out",
            "role_names_nobody",
            "role_ambiguous",
            "role_not_admitted",
            "place_unknown",
            "thing_unknown",
        };
    } // namespace

    std::string_view Id(Code code) noexcept
    {
        const auto i = static_cast<std::size_t>(code);
        return i < kIds.size() ? kIds[i] : std::string_view{};
    }

    nlohmann::json Rejection::ToJson() const
    {
        nlohmann::json out = args.is_object() ? args : nlohmann::json::object();
        out["code"] = std::string{Id(code)};
        return out;
    }

    Rejection Make(Code code, nlohmann::json args)
    {
        return {code, std::move(args)};
    }

    nlohmann::json Role(std::string_view verb, std::string_view slot)
    {
        return {{"verb", verb}, {"name", slot}};
    }
} // namespace NarrativeEngine::PlotRejection
