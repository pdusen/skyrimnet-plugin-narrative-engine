#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// The dashboard's render check: one prompt that confirms, in the running game,
// three things about SkyrimNet's template rendering that the prompts rely on
// and that were settled only by reading SkyrimNet's binary and rendering
// offline:
//
//   * render_template takes a name built in the template, not only a literal
//     (the beat descriptions in action_select are found that way);
//   * a decorator's answer, when it is JSON, is an object the template reads
//     fields from (ne_plot_awareness answers that way);
//   * an {% include %} sees the includer's loop variable and {% set %}
//     variables (every shared component is reached that way).
//
// Each leaves a sentinel in the rendered prompt, and the model is asked to
// repeat them. A sentinel missing from its reply means that part did not
// render. The template is narrative_engine_render_check.prompt.
//
// Pure: the request's context, the decorator's answer and the reply's verdict.
// RunChecks sends it.
namespace NarrativeEngine::RenderCheck
{
    inline constexpr const char* kTemplate = "narrative_engine_render_check";

    // What components/narrative_engine/render_check_sentinel.prompt holds. The
    // prompt reaches that file only through a render_template name it builds.
    inline constexpr std::string_view kFileSentinel = "NE-FILE-4QX7";

    // The other sentinels, fresh for each run, so a reply cannot pass by
    // having seen them before.
    struct Sentinels
    {
        // ne_render_check's answer carries it, as a field of its JSON.
        std::string decorator;
        // The include words both: the loop's item, and a {% set %} value.
        std::string loop;
        std::string set;
    };

    [[nodiscard]] Sentinels Make(std::uint64_t seed);

    // The request's context: the name's last part, the loop's one item, and
    // the value the template sets.
    [[nodiscard]] std::string Context(const Sentinels& sentinels);

    // What ne_render_check answers while the check runs.
    [[nodiscard]] std::string DecoratorRecord(const Sentinels& sentinels);

    struct Result
    {
        bool file = false;
        bool decorator = false;
        // Both the loop's item and the set value reached the include.
        bool include = false;
    };

    [[nodiscard]] Result Evaluate(std::string_view reply, const Sentinels& sentinels);
} // namespace NarrativeEngine::RenderCheck
