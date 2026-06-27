#pragma once

#include <string_view>

// Executes Skyrim console commands programmatically from C++ — the same
// path the player exercises by typing into the in-game `~` console. The
// engine's quest / scene / actor systems treat console-issued commands as
// authoritative game state changes, which has historically been a more
// reliable trigger for full quest promotion (stage fragment + FMR alias
// evaluation) than the direct `TESQuest::Start` / `ResetAndUpdate` paths.
//
// This is intentionally a temporary measure: it works around a gap in our
// understanding of which `TESQuest` C++ entry point fully replicates the
// console `startquest` behavior. Once that's resolved, callers should
// migrate back to direct API calls and this helper can be removed.
namespace NarrativeEngine::ConsoleCommand
{
    // Compile and execute `command` as if the player typed it into the
    // console. Must be called on the main thread (Script::CompileAndRun
    // dispatches into the engine's script VM, which is not thread-safe).
    // Returns true if the script object was successfully constructed and
    // dispatched; false if the engine refused to create the script form
    // (rare — usually only at shutdown).
    bool Run(std::string_view command);
}
