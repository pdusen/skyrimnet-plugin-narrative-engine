#pragma once

#include <PluginThread.h>
#include <RenderCheck.h>

#include <string>

// The dashboard's *Run checks* button: what can only be checked in the running
// game, done in one press.
//
//   * The render check (RenderCheck.h), sent to SkyrimNet as a custom prompt on
//     the Director's model, and judged from the reply.
//   * A notification of accented letters ([Notify] font_sample), shown as soon
//     as the dashboard closes, for the player to read: the game draws it with
//     the font set the player's language uses.
//
// The dashboard words the rest itself: the font check from the language block
// it is sent with every state, and whether its own Intl.PluralRules exists.
namespace NarrativeEngine::RunChecks
{
    struct Snapshot
    {
        bool running = false;
        bool ran = false;
        enum class Failure
        {
            None,
            // SkyrimNet would not take the request.
            NotQueued,
            // SkyrimNet took it and reported an error: a render error, or the
            // model's.
            Reported,
        };
        Failure failure = Failure::None;
        // What SkyrimNet reported, as it said it.
        std::string detail;
        RenderCheck::Result result;
    };

    // Run the checks, unless a run is already waiting on its reply. Pushes the
    // dashboard's state when the reply arrives.
    void Start(const PluginThread::Token& pt);

    // The last run, for the dashboard. Any thread.
    [[nodiscard]] Snapshot Get();

    // What the ne_render_check decorator answers: the running check's record,
    // or the empty string. Called on SkyrimNet's render thread, which holds no
    // token; it reads only this module's own, locked, state.
    [[nodiscard]] std::string DecoratorRecord();
} // namespace NarrativeEngine::RunChecks
